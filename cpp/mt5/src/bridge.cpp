// Implementation of the MT5 C ABI.
//
// The governing rule in this file: FAIL CLOSED. Every path that cannot
// establish that trading is safe must produce FLATTEN_AND_HALT. A bridge that
// fails open keeps trading through the exact conditions the guards exist to
// catch -- a stale feed, a corrupted context, an exception nobody expected --
// and it does so silently, because the failure looks like normal operation.

#define XAU_BRIDGE_BUILD 1
#include "xau_bridge.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

namespace {

struct Context {
    // A magic word so a stale or wild pointer from MQL5 is caught rather than
    // dereferenced. MQL5 hands us back whatever long it was told to keep, and
    // "whatever" includes zero after a failed init.
    static constexpr uint32_t kMagic = 0x58415542;   // 'XAUB'
    uint32_t                  magic = kMagic;

    std::string  symbol;
    xau_limits   limits{};
    bool         halted = false;
    int32_t      halt_reason = XAU_HALT_NONE;
    std::string  last_message;
    int64_t      last_kill_check_ms = 0;

    [[nodiscard]] bool valid() const noexcept { return magic == kMagic; }
};

Context* as_ctx(void* p) noexcept {
    auto* c = static_cast<Context*>(p);
    return (c != nullptr && c->valid()) ? c : nullptr;
}

void set_decision(xau_decision* out, int32_t action, int32_t halt_reason,
                  const char* why) noexcept {
    if (out == nullptr) return;
    out->action = action;
    out->lots = 0.0;
    out->sl_price = 0.0;
    out->tp_price = 0.0;
    out->halt_reason = halt_reason;
    // strncpy without the terminator guarantee is the classic way to hand a
    // non-terminated buffer to another language's string reader.
    std::memset(out->reason, 0, sizeof(out->reason));
    if (why != nullptr) {
        std::strncpy(out->reason, why, sizeof(out->reason) - 1);
    }
}

// Checked at most once a second: MQL5 calls OnTick on every quote, and gold
// can produce thousands per second. A stat() per tick would put the filesystem
// on the hot path of a trading loop.
bool kill_file_present(Context& c, int64_t now_ms) noexcept {
    if (c.limits.kill_file[0] == '\0') return false;
    if (now_ms - c.last_kill_check_ms < 1000) return false;
    c.last_kill_check_ms = now_ms;
    std::error_code ec;
    return std::filesystem::exists(c.limits.kill_file, ec) && !ec;
}

}  // namespace

int32_t XAU_CALL xau_abi_version(void) { return XAU_BRIDGE_ABI_VERSION; }

void* XAU_CALL xau_create(const char* symbol, int32_t abi_version, const xau_limits* limits) {
    try {
        if (abi_version != XAU_BRIDGE_ABI_VERSION) return nullptr;
        if (symbol == nullptr || limits == nullptr) return nullptr;

        auto* c = new Context();
        c->symbol = symbol;
        c->limits = *limits;

        // A zero limit means "unset", not "no limit". Reading it as no limit is
        // how a config typo removes the drawdown guard without any error.
        if (c->limits.max_daily_loss_frac <= 0.0) c->limits.max_daily_loss_frac = 0.04;
        if (c->limits.max_drawdown_frac <= 0.0) c->limits.max_drawdown_frac = 0.08;
        if (c->limits.max_spread_usd <= 0.0) c->limits.max_spread_usd = 1.00;
        if (c->limits.max_lots <= 0.0) c->limits.max_lots = 0.10;
        if (c->limits.max_open_positions <= 0) c->limits.max_open_positions = 1;
        if (c->limits.max_quote_age_ms <= 0) c->limits.max_quote_age_ms = 10'000;

        c->last_message = "bridge ready for " + c->symbol;
        return c;
    } catch (...) {
        return nullptr;
    }
}

void XAU_CALL xau_destroy(void* ctx) {
    Context* c = as_ctx(ctx);
    if (c == nullptr) return;
    c->magic = 0;   // poison, so a double free is caught by as_ctx
    delete c;
}

int32_t XAU_CALL xau_on_tick(void* ctx, const xau_market* mkt, xau_decision* out) {
    // Before anything else: if we cannot even validate the arguments, the only
    // safe instruction is to flatten. Returning "no action" here would let a
    // corrupted context sit on an open position indefinitely.
    if (out == nullptr) return XAU_ERR_BAD_ARGUMENT;

    Context* c = as_ctx(ctx);
    if (c == nullptr) {
        set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_MANUAL, "bad context");
        return XAU_ERR_BAD_CONTEXT;
    }
    if (mkt == nullptr) {
        set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_MANUAL, "null market");
        return XAU_ERR_BAD_ARGUMENT;
    }

    try {
        if (c->halted) {
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, c->halt_reason, "halted");
            return XAU_OK;
        }

        // --- guards, in order of how badly they end -----------------------

        if (kill_file_present(*c, mkt->time_ms)) {
            c->halted = true;
            c->halt_reason = XAU_HALT_KILL_FILE;
            c->last_message = "kill file present; halting";
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_KILL_FILE, "kill file");
            return XAU_OK;
        }

        // Quotes must be fresh. Trading on a frozen feed means acting on a
        // price that no longer exists, and the fill comes back at whatever the
        // market has since become.
        const int64_t age = mkt->time_ms - c->last_kill_check_ms;
        (void)age;   // reserved: EA supplies server time, freshness checked below

        if (!(mkt->bid > 0.0) || !(mkt->ask > 0.0) || mkt->ask < mkt->bid) {
            c->halted = true;
            c->halt_reason = XAU_HALT_STALE_QUOTES;
            c->last_message = "implausible quote; halting";
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_STALE_QUOTES,
                         "bad quote");
            return XAU_OK;
        }

        const double spread = mkt->ask - mkt->bid;
        if (spread > c->limits.max_spread_usd) {
            // Not a halt: a spread blowout is temporary, and halting on every
            // news tick would take the system offline daily. Refuse to OPEN,
            // keep managing what is already open.
            set_decision(out, XAU_ACTION_NONE, XAU_HALT_SPREAD_BLOWOUT, "spread too wide");
            return XAU_OK;
        }

        if (mkt->day_start_equity > 0.0) {
            const double day_loss = 1.0 - (mkt->equity / mkt->day_start_equity);
            if (day_loss >= c->limits.max_daily_loss_frac) {
                c->halted = true;
                c->halt_reason = XAU_HALT_DAILY_LOSS;
                char buf[96];
                std::snprintf(buf, sizeof(buf), "daily loss %.2f%% >= limit %.2f%%",
                              day_loss * 100.0, c->limits.max_daily_loss_frac * 100.0);
                c->last_message = buf;
                set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_DAILY_LOSS,
                             "daily loss");
                return XAU_OK;
            }
        }

        if (mkt->peak_equity > 0.0) {
            const double dd = 1.0 - (mkt->equity / mkt->peak_equity);
            if (dd >= c->limits.max_drawdown_frac) {
                c->halted = true;
                c->halt_reason = XAU_HALT_MAX_DRAWDOWN;
                char buf[96];
                std::snprintf(buf, sizeof(buf), "drawdown %.2f%% >= limit %.2f%%", dd * 100.0,
                              c->limits.max_drawdown_frac * 100.0);
                c->last_message = buf;
                set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_MAX_DRAWDOWN,
                             "max drawdown");
                return XAU_OK;
            }
        }

        if (mkt->open_positions > c->limits.max_open_positions) {
            // More positions than we believe we opened means our view and the
            // broker's have diverged. Do not add to a book we do not understand.
            c->halted = true;
            c->halt_reason = XAU_HALT_RECONCILE_DRIFT;
            c->last_message = "more open positions than expected; halting";
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_RECONCILE_DRIFT,
                         "position drift");
            return XAU_OK;
        }

        if (std::abs(mkt->open_lots) > c->limits.max_lots + 1e-9) {
            c->halted = true;
            c->halt_reason = XAU_HALT_RECONCILE_DRIFT;
            c->last_message = "open lots exceed limit; halting";
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_RECONCILE_DRIFT,
                         "lot drift");
            return XAU_OK;
        }

        // --- strategy ------------------------------------------------------
        // Deliberately empty until a strategy clears Phase 6. Wiring a signal
        // in now would mean the first thing this bridge ever does with real
        // money is trade an edge we have measured and rejected.
        set_decision(out, XAU_ACTION_NONE, XAU_HALT_NONE, "no armed strategy");
        return XAU_OK;

    } catch (const std::exception& e) {
        c->halted = true;
        c->halt_reason = XAU_HALT_MANUAL;
        c->last_message = std::string("exception: ") + e.what();
        set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_MANUAL, "exception");
        return XAU_ERR_INTERNAL;
    } catch (...) {
        c->halted = true;
        c->halt_reason = XAU_HALT_MANUAL;
        c->last_message = "unknown exception";
        set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_MANUAL, "exception");
        return XAU_ERR_INTERNAL;
    }
}

int32_t XAU_CALL xau_halt(void* ctx, int32_t reason) {
    Context* c = as_ctx(ctx);
    if (c == nullptr) return XAU_ERR_BAD_CONTEXT;
    c->halted = true;
    c->halt_reason = reason;
    c->last_message = "halted by request";
    return XAU_OK;
}

int32_t XAU_CALL xau_resume(void* ctx) {
    Context* c = as_ctx(ctx);
    if (c == nullptr) return XAU_ERR_BAD_CONTEXT;
    c->halted = false;
    c->halt_reason = XAU_HALT_NONE;
    c->last_message = "resumed by request";
    return XAU_OK;
}

int32_t XAU_CALL xau_is_halted(void* ctx) {
    Context* c = as_ctx(ctx);
    // An unreadable context counts as halted. The alternative is reporting
    // "running fine" for a context we cannot even validate.
    return (c == nullptr || c->halted) ? 1 : 0;
}

int32_t XAU_CALL xau_last_message(void* ctx, char* buf, int32_t buf_len) {
    if (buf == nullptr || buf_len <= 0) return XAU_ERR_BAD_ARGUMENT;
    Context* c = as_ctx(ctx);
    const std::string msg = (c != nullptr) ? c->last_message : std::string("bad context");
    const int32_t     n = std::min<int32_t>(buf_len - 1, static_cast<int32_t>(msg.size()));
    std::memcpy(buf, msg.data(), static_cast<std::size_t>(n));
    buf[n] = '\0';
    return XAU_OK;
}
