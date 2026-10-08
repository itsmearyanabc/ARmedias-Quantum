// Implementation of the MT5 C ABI.
//
// The governing rule in this file: FAIL CLOSED. Every path that cannot
// establish that trading is safe must produce FLATTEN_AND_HALT or, for a
// condition that is safe to wait out, refuse the entry. A bridge that fails
// open keeps trading through the exact conditions the guards exist to catch --
// a stale feed, a corrupted context, an exception nobody expected -- and it
// does so silently, because the failure looks like normal operation.
//
// The strategy runs through xau::LiveSession: the same bar assembler and the
// same Strategy contract the backtest engine uses, proven bar-for-bar
// identical by test_live.cpp. Orders mirror the engine's entry rules (stop
// inside the spread refused, broker minimum distance refused, size from the
// decision or the risk layer), so the thing that trades is the thing that was
// tested -- test_bridge.cpp drives this ABI through a simulated broker and
// checks it against BacktestEngine trade for trade.

#define XAU_BRIDGE_BUILD 1
#include "xau_bridge.h"

#include "xau/bar.hpp"
#include "xau/live.hpp"
#include "xau/order.hpp"
#include "xau/registry.hpp"
#include "xau/risk.hpp"
#include "xau/symbol_spec.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

// The layout contract with MQL5, pinned. MQL5 packs at 1 byte and C aligns
// naturally; these hold only because no field ever needs padding.
static_assert(sizeof(xau_market) == 128, "xau_market layout");
static_assert(offsetof(xau_market, bid) == 24, "xau_market layout");
static_assert(offsetof(xau_market, gross_lots) == 88, "xau_market layout");
static_assert(offsetof(xau_market, value_per_price_lot) == 96, "xau_market layout");
static_assert(offsetof(xau_market, pos_side) == 104, "xau_market layout");
static_assert(offsetof(xau_market, own_orders) == 124, "xau_market layout");
static_assert(sizeof(xau_decision) == 112, "xau_decision layout");
static_assert(offsetof(xau_decision, sl_dist) == 24, "xau_decision layout");
static_assert(offsetof(xau_decision, action) == 40, "xau_decision layout");
static_assert(offsetof(xau_decision, reason) == 48, "xau_decision layout");
static_assert(sizeof(xau_limits) == 576, "xau_limits layout");
static_assert(offsetof(xau_limits, max_open_positions) == 40, "xau_limits layout");
static_assert(offsetof(xau_limits, kill_file) == 56, "xau_limits layout");
static_assert(offsetof(xau_limits, state_file) == 316, "xau_limits layout");
static_assert(sizeof(xau_strategy_config) == 128, "xau_strategy_config layout");
static_assert(offsetof(xau_strategy_config, timeframe) == 48, "xau_strategy_config layout");
static_assert(offsetof(xau_strategy_config, strategy) == 64, "xau_strategy_config layout");
static_assert(sizeof(xau_rate) == 56, "xau_rate layout");
static_assert(offsetof(xau_rate, tick_volume) == 48, "xau_rate layout");

namespace {

namespace fs = std::filesystem;

using xau::Decision;
using xau::Points;
using xau::Side;
using xau::TimeUs;

// --- persisted state -------------------------------------------------------
//
// What must survive a restart of MetaTrader: a halt, and the two equity anchors
// the loss limits are measured from. Losing any of them hands back an allowance
// the limits had already spent.
struct Persisted {
    bool        halted = false;
    int32_t     halt_reason = XAU_HALT_NONE;
    std::string halt_message;
    int32_t     day = 0;                 // broker trading day, yyyymmdd
    double      day_start_equity = 0.0;
    double      peak_equity = 0.0;
};

constexpr const char* kStateHeader = "xau_bridge_state 1";

constexpr double  kLimitEps = 1e-9;
constexpr int64_t kSaveRetryMs = 1000;      // a failed state write is retried this often
constexpr int     kCloseAttempts = 5;       // a wanted exit is tried this many times, then halts
constexpr int64_t kCloseRetryGapMs = 1000;

// State files in use by a live context in this process. MetaTrader loads one
// copy of the DLL for every EA in the terminal, so two EAs given the same file
// (same symbol and magic) would each overwrite the other's halts and anchors.
std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}
std::set<std::string>& registry() {
    static std::set<std::string> r;
    return r;
}

// Pending order lifecycle. One decision, one order: nothing new is sent until
// the last one has been reported and, if it filled, shows up at the broker.
enum class Pending : uint8_t { None, AwaitResult, AwaitOpen, AwaitFlat };

struct Context {
    // A magic word so a stale or wild pointer from MQL5 is caught rather than
    // dereferenced. MQL5 hands us back whatever long it was told to keep, and
    // "whatever" includes zero after a failed init.
    static constexpr uint32_t kMagic = 0x58415533;   // 'XAU3'
    uint32_t                  magic = kMagic;

    std::string symbol;
    xau_limits  limits{};
    fs::path    kill_path;                // decoded from UTF-8 once, at create
    fs::path    state_path;
    std::string registered;               // key in the state-file registry; empty = none
    Persisted   st;
    double      persisted_peak = 0.0;     // peak as last written to disk
    bool        state_dirty = false;      // memory holds something the file does not
    int64_t     last_save_try_ms = 0;
    std::string last_message;
    int64_t     last_kill_check_ms = 0;
    int64_t     last_now_ms = 0;          // latest wall clock the EA reported

    // armed strategy
    std::unique_ptr<xau::Strategy>    strategy;
    std::unique_ptr<xau::LiveSession> session;
    xau::SymbolSpec                   spec{};
    xau_strategy_config               cfg{};
    TimeUs                            last_tick_us = 0;
    bool                              warmed = false;

    Pending  pending = Pending::None;
    int64_t  pending_since_ms = 0;
    int32_t  pending_side = 0;            // the side an entry is waiting for

    // An exit the strategy asked for and the broker has not yet carried out.
    // It is retried, never dropped: a lost exit is a position nobody manages.
    bool        close_owed = false;
    int         close_attempts = 0;
    int64_t     close_last_try_ms = 0;
    std::string close_reason;

    // counters, for the status panel
    uint64_t entries_sent = 0;
    uint64_t closes_sent = 0;
    uint64_t refusals = 0;
    char     last_refusal[64] = {0};

    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context() {
        if (registered.empty()) return;
        try {
            const std::lock_guard<std::mutex> lk(registry_mutex());
            registry().erase(registered);
        } catch (...) {
        }
    }

    [[nodiscard]] bool valid() const noexcept { return magic == kMagic; }
    [[nodiscard]] bool armed() const noexcept { return session != nullptr; }
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
    out->sl_dist = 0.0;
    out->tp_dist = 0.0;
    out->halt_reason = halt_reason;
    // strncpy without the terminator guarantee is the classic way to hand a
    // non-terminated buffer to another language's string reader.
    std::memset(out->reason, 0, sizeof(out->reason));
    if (why != nullptr) std::strncpy(out->reason, why, sizeof(out->reason) - 1);
}

std::string cstr(const char* buf, std::size_t cap) {
    return std::string(buf, ::strnlen(buf, cap));
}

// The EA's paths are UTF-8. A path built from a plain std::string is read in
// the ANSI code page on Windows, which turns any non-ASCII user name in
// "C:\Users\...\MQL5\Files" into a different, nonexistent file -- and a kill
// file that is never found is a kill switch that does nothing.
fs::path utf8_path(const char* buf, std::size_t cap) {
    const std::string s = cstr(buf, cap);
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string display(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// Written to a temporary and renamed over the original, so a crash mid-write
// leaves either the old state or the new one, never half of each. A failure to
// persist does not halt -- the in-memory state is still right, and halting a
// healthy session over a full disk would be its own failure -- but it is
// marked dirty and retried every second until it lands.
void save_state(Context& c) noexcept {
    try {
        if (c.state_path.empty()) return;
        c.state_dirty = true;
        fs::path tmp = c.state_path;
        tmp += u8".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f) {
                c.last_message = "state file not writable: " + display(c.state_path);
                return;
            }
            std::string msg = c.st.halt_message;
            std::replace(msg.begin(), msg.end(), '\n', ' ');
            f << kStateHeader << '\n'
              << "halted=" << (c.st.halted ? 1 : 0) << '\n'
              << "halt_reason=" << c.st.halt_reason << '\n'
              << "halt_message=" << msg << '\n'
              << "day=" << c.st.day << '\n';
            f.precision(17);
            f << "day_start_equity=" << c.st.day_start_equity << '\n'
              << "peak_equity=" << c.st.peak_equity << '\n';
            f.flush();
            if (!f) {
                c.last_message = "state file write failed: " + display(c.state_path);
                return;
            }
        }
        std::error_code ec;
        fs::rename(tmp, c.state_path, ec);   // replaces on POSIX and Windows
        if (ec) {
            c.last_message = "state file rename failed: " + ec.message();
            return;
        }
        c.persisted_peak = c.st.peak_equity;
        c.state_dirty = false;
    } catch (...) {
        c.state_dirty = true;
        c.last_message = "state file write threw";
    }
}

// Throttled: a disk that refuses one write usually refuses the next, and a
// write per tick would put the failing filesystem on the hot path.
void retry_save(Context& c, int64_t now_ms) noexcept {
    if (!c.state_dirty) return;
    if (c.last_save_try_ms != 0 && now_ms >= c.last_save_try_ms &&
        now_ms - c.last_save_try_ms < kSaveRetryMs)
        return;
    c.last_save_try_ms = now_ms;
    save_state(c);
}

enum class ReadResult { Missing, Loaded, Unreadable };

// Missing: a first run. Present but unreadable: we cannot know whether it said
// "halted", so the caller treats it as if it did.
ReadResult read_state(const fs::path& path, Persisted& out) {
    std::error_code ec;
    const bool present = fs::exists(path, ec);
    if (ec) return ReadResult::Unreadable;
    if (!present) return ReadResult::Missing;

    std::ifstream f(path);
    std::string   line;
    if (!f || !std::getline(f, line) || line != kStateHeader) return ReadResult::Unreadable;
    Persisted p;
    int       fields = 0;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "halted") { p.halted = std::stoi(v) != 0; ++fields; }
            else if (k == "halt_reason") { p.halt_reason = std::stoi(v); ++fields; }
            else if (k == "halt_message") { p.halt_message = v; ++fields; }
            else if (k == "day") { p.day = std::stoi(v); ++fields; }
            else if (k == "day_start_equity") { p.day_start_equity = std::stod(v); ++fields; }
            else if (k == "peak_equity") { p.peak_equity = std::stod(v); ++fields; }
        } catch (...) {
            fields = -100;   // a value that does not parse poisons the file
        }
    }
    if (fields != 6) return ReadResult::Unreadable;
    out = p;
    return ReadResult::Loaded;
}

void load_state(Context& c) {
    if (c.state_path.empty()) return;
    Persisted p;
    switch (read_state(c.state_path, p)) {
        case ReadResult::Missing: return;
        case ReadResult::Loaded:
            c.st = p;
            c.persisted_peak = p.peak_equity;
            return;
        case ReadResult::Unreadable:
            // Not saved: the file is the evidence, and overwriting it with
            // zeroed anchors would hand back the allowance it recorded.
            c.st = Persisted{};
            c.st.halted = true;
            c.st.halt_reason = XAU_HALT_STATE_FILE;
            c.st.halt_message = "state file unreadable: " + display(c.state_path) +
                                " -- fix or delete it, then resume";
            return;
    }
}

void halt(Context& c, int32_t reason, const std::string& msg) {
    c.pending = Pending::None;
    c.close_owed = false;      // FLATTEN supersedes any exit still owed
    if (c.st.halted && c.st.halt_reason == XAU_HALT_STATE_FILE) {
        // Stays a STATE_FILE halt: the anchors it lost are not in memory, and
        // saving now would overwrite the file that still holds them.
        c.last_message = "HALT: " + msg + " (state file still unreadable)";
        return;
    }
    c.st.halted = true;
    c.st.halt_reason = reason;
    c.st.halt_message = msg;
    c.last_message = "HALT: " + msg;
    if (reason != XAU_HALT_STATE_FILE) save_state(c);
}

bool kill_file_exists(const Context& c) noexcept {
    if (c.kill_path.empty()) return false;
    std::error_code ec;
    return fs::exists(c.kill_path, ec) && !ec;
}

// Checked at most once a second: MQL5 calls OnTick on every quote, and gold
// can produce thousands per second. A stat() per tick would put the filesystem
// on the hot path of a trading loop. Timed on the wall clock the EA passes,
// not the quote's own time, so a frozen feed cannot freeze the check -- and a
// clock that steps backward makes the check due rather than blinding it until
// the clock catches up.
bool kill_file_due(Context& c, int64_t now_ms) noexcept {
    if (c.kill_path.empty()) return false;
    const int64_t last = c.last_kill_check_ms;
    if (last != 0 && now_ms >= last && now_ms - last < 1000) return false;
    c.last_kill_check_ms = now_ms;
    return kill_file_exists(c);
}

Points to_pts(const xau::SymbolSpec& spec, double price) noexcept {
    return static_cast<Points>(std::llround(price * static_cast<double>(spec.point_den) /
                                            static_cast<double>(spec.point_num)));
}

xau::Tick make_tick(const xau::SymbolSpec& spec, TimeUs ts_us, double bid, double ask) noexcept {
    xau::Tick t{};
    t.ts_us = ts_us;
    t.bid_pts = to_pts(spec, bid);
    const Points spread = std::max<Points>(0, to_pts(spec, ask) - t.bid_pts);
    t.spread_pts = static_cast<std::uint16_t>(std::min<Points>(spread, 0xFFFF));
    t.flags = static_cast<std::uint16_t>(xau::TF_BID | xau::TF_ASK |
                                         (spread > 0xFFFF ? xau::TF_SPREAD_SAT : 0));
    return t;
}

// The broker's view of our position, in the engine's terms. The strategy never
// infers its own position: what the broker holds is what it holds.
xau::Position broker_position(const Context& c, const xau_market& m) noexcept {
    xau::Position p{};
    if (m.pos_side == 0 || m.own_positions <= 0) return p;
    p.side = m.pos_side > 0 ? Side::Long : Side::Short;
    p.lots = m.pos_lots;
    p.entry_pts = to_pts(c.spec, m.pos_open_price);
    p.entry_ts = static_cast<TimeUs>(m.pos_open_time_ms) * 1000;
    p.sl_pts = m.pos_sl > 0.0 ? to_pts(c.spec, m.pos_sl) : 0;
    p.tp_pts = m.pos_tp > 0.0 ? to_pts(c.spec, m.pos_tp) : 0;
    return p;
}

void refuse(Context& c, xau_decision* out, int32_t reason, const char* why) {
    ++c.refusals;
    std::strncpy(c.last_refusal, why, sizeof(c.last_refusal) - 1);
    c.last_message = std::string("entry refused: ") + why;
    set_decision(out, XAU_ACTION_NONE, reason, why);
}

// In whole points, then converted once: the price grid is exact, and a stop
// computed as price - distance * 0.001 in floating point lands a hair off it.
// A broker -- like the engine -- compares exact prices, so a quote equal to the
// stop must trigger it, and a hair's difference would not.
double to_price(const xau::SymbolSpec& spec, Points p) noexcept {
    return static_cast<double>(p) * static_cast<double>(spec.point_num) /
           static_cast<double>(spec.point_den);
}

// The engine's entry rules (engine.cpp, try_enter), applied to a live quote.
// A rule the backtest enforces and live does not is a trade live takes that
// was never tested.
void emit_entry(Context& c, const Decision& d, const xau_market& m, xau_decision* out) {
    const Points spread = std::max<Points>(0, to_pts(c.spec, m.ask) - to_pts(c.spec, m.bid));
    if (d.sl_dist_pts > 0 && d.sl_dist_pts <= spread) {
        refuse(c, out, XAU_HALT_NONE, "stop inside the spread");
        return;
    }
    if (!c.spec.stop_distance_ok(d.sl_dist_pts) || !c.spec.stop_distance_ok(d.tp_dist_pts)) {
        refuse(c, out, XAU_HALT_NONE, "stop closer than the broker allows");
        return;
    }
    // The broker measures its minimum distance from the price that would
    // trigger the stop -- bid for a long, ask for a short -- not from the entry.
    // A long's stop at ask - d sits d - spread below the bid, and the broker
    // rejects the order when that is under its level. Stricter than the
    // engine's rule only when the broker has a level; with none it is the
    // spread rule above.
    const Points level = c.spec.stops_level_pts;
    if (level > 0 && ((d.sl_dist_pts > 0 && d.sl_dist_pts < level + spread) ||
                      (d.tp_dist_pts > 0 && d.tp_dist_pts < level - spread))) {
        refuse(c, out, XAU_HALT_NONE, "stop inside the broker's level from bid/ask");
        return;
    }

    double lots = 0.0;
    if (d.lots > 0.0) {
        lots = c.spec.round_lots(d.lots);
    } else {
        // Risk is lost in the ACCOUNT's currency. Contract size times price is
        // USD per lot; on a EUR or GBP account that sizes every trade wrong by
        // the exchange rate. The broker's tick value is already in the
        // account currency, so it stands in for the contract size here.
        if (!(m.value_per_price_lot > 0.0)) {
            refuse(c, out, XAU_HALT_NONE, "no tick value from the broker; cannot size by risk");
            return;
        }
        xau::SymbolSpec acct = c.spec;
        acct.contract_size = m.value_per_price_lot;
        xau::SizingConfig sc;
        sc.risk_per_trade = c.cfg.risk_per_trade;
        sc.vol_target = false;
        sc.max_lots = c.limits.max_lots;
        sc.min_lots = c.spec.volume_min;
        lots = xau::size_by_risk(m.equity, d.sl_dist_pts, 0.0, acct, sc);
    }
    // The ceiling is a guard, so it cuts rather than refuses: a decision for
    // more than the limit trades the limit, never more.
    lots = std::min(lots, c.spec.round_lots(c.limits.max_lots));
    if (!(lots > 0.0)) {
        refuse(c, out, XAU_HALT_NONE, "size rounds below the broker minimum");
        return;
    }

    const bool   buy = d.side == Side::Long;
    const double ref = buy ? m.ask : m.bid;
    const Points ref_pts = to_pts(c.spec, ref);
    const Points sgn = buy ? 1 : -1;

    set_decision(out, buy ? XAU_ACTION_BUY : XAU_ACTION_SELL, XAU_HALT_NONE,
                 d.reason != nullptr ? d.reason : "");
    out->lots = lots;
    out->sl_price = d.sl_dist_pts > 0 ? to_price(c.spec, ref_pts - sgn * d.sl_dist_pts) : 0.0;
    out->tp_price = d.tp_dist_pts > 0 ? to_price(c.spec, ref_pts + sgn * d.tp_dist_pts) : 0.0;
    out->sl_dist = d.sl_dist_pts > 0 ? to_price(c.spec, d.sl_dist_pts) : 0.0;
    out->tp_dist = d.tp_dist_pts > 0 ? to_price(c.spec, d.tp_dist_pts) : 0.0;

    c.pending = Pending::AwaitResult;
    c.pending_since_ms = m.now_ms;
    c.pending_side = buy ? 1 : -1;
    ++c.entries_sent;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s %.2f lots near %.5g, sl %.5g tp %.5g (%s)",
                  buy ? "BUY" : "SELL", lots, ref, out->sl_price, out->tp_price, out->reason);
    c.last_message = buf;
}

void emit_close(Context& c, int64_t now_ms, xau_decision* out) {
    set_decision(out, XAU_ACTION_CLOSE, XAU_HALT_NONE,
                 c.close_reason.empty() ? "close" : c.close_reason.c_str());
    c.close_owed = true;
    c.close_last_try_ms = now_ms;
    c.pending = Pending::AwaitResult;
    c.pending_since_ms = now_ms;
    c.pending_side = 0;
    ++c.closes_sent;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "CLOSE (%s), attempt %d of %d", out->reason,
                  c.close_attempts, kCloseAttempts);
    c.last_message = buf;
}

// Persistence of the anchors, and the day roll. The daily anchor belongs to the
// broker's trading day; it is set from the first equity seen that day and is
// never moved by a restart, because it is read back from disk.
// yyyymmdd of a UTC instant, for an EA that sends no broker trading day. The
// daily limit must never be left without an anchor, so a missing day falls
// back to the UTC date rather than to "no daily limit".
int32_t utc_yyyymmdd(int64_t ms) noexcept {
    const long long days = (ms >= 0 ? ms : ms - 86'399'999) / 86'400'000;   // floor
    // Hinnant's civil_from_days.
    const long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long d = doy - (153 * mp + 2) / 5 + 1;
    const long long mo = mp < 10 ? mp + 3 : mp - 9;
    const long long y = yoe + era * 400 + (mo <= 2 ? 1 : 0);
    return static_cast<int32_t>(y * 10000 + mo * 100 + d);
}

void update_anchors(Context& c, const xau_market& m) {
    bool          dirty = false;
    const int32_t day = m.server_day > 0 ? m.server_day : utc_yyyymmdd(m.tick_time_ms);
    if (day != c.st.day) {
        c.st.day = day;
        c.st.day_start_equity = m.equity;
        dirty = true;
    }
    if (m.equity > c.st.peak_equity) {
        c.st.peak_equity = m.equity;
        // Not every new high: a rising market would write a file per tick.
        // A crash can lose at most 0.05% of peak, which errs lenient by that
        // much and no more.
        if (c.persisted_peak <= 0.0 || c.st.peak_equity >= c.persisted_peak * 1.0005) dirty = true;
    }
    if (dirty) save_state(c);
}

// A clock that steps backward restarts the wait rather than stretching it by
// the size of the step.
bool pending_overdue(Context& c, int64_t now_ms) noexcept {
    if (c.pending == Pending::None || c.limits.pending_timeout_ms <= 0) return false;
    if (now_ms < c.pending_since_ms) c.pending_since_ms = now_ms;
    return now_ms - c.pending_since_ms > c.limits.pending_timeout_ms;
}

const char* pending_name(Pending p) noexcept {
    switch (p) {
        case Pending::AwaitResult: return "awaiting order result";
        case Pending::AwaitOpen:   return "awaiting position";
        case Pending::AwaitFlat:   return "awaiting close";
        default:                   return "none";
    }
}

int32_t on_tick_impl(Context& c, const xau_market& m, xau_decision* out) {
    const TimeUs ts_us = static_cast<TimeUs>(m.tick_time_ms) * 1000;
    const bool   plausible = m.bid > 0.0 && m.ask > 0.0 && m.ask >= m.bid;
    c.last_now_ms = m.now_ms;
    retry_save(c, m.now_ms);   // halted or not: a halt that never reached disk is the worst kind

    // The session hears every plausible, in-order tick whatever the guards
    // decide below, halted or not: a bar assembled from the ticks that happened
    // to arrive while trading was allowed is not the bar the backtest built.
    Decision d = Decision::hold();
    if (c.armed() && plausible && ts_us >= c.last_tick_us) {
        c.last_tick_us = ts_us;
        d = c.session->on_tick(make_tick(c.spec, ts_us, m.bid, m.ask), broker_position(c, m),
                               m.equity, m.balance);
    }

    if (c.st.halted) {
        set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, c.st.halt_reason, "halted");
        return XAU_OK;
    }

    // --- guards that halt, in order of how badly they end ------------------
    if (kill_file_due(c, m.now_ms)) {
        halt(c, XAU_HALT_KILL_FILE, "kill file present");
        set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_KILL_FILE, "kill file");
        return XAU_OK;
    }
    if (!plausible) {
        halt(c, XAU_HALT_STALE_QUOTES, "implausible quote (zero or inverted)");
        set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_STALE_QUOTES, "bad quote");
        return XAU_OK;
    }

    update_anchors(c, m);

    if (c.st.day_start_equity > 0.0) {
        const double day_loss = 1.0 - m.equity / c.st.day_start_equity;
        // A hair of tolerance toward halting: 1 - 9600/10000 is not exactly
        // 0.04 in binary, and a limit must not be missed by a rounding error.
        if (day_loss >= c.limits.max_daily_loss_frac - kLimitEps) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "daily loss %.2f%% reached the %.2f%% limit",
                          day_loss * 100.0, c.limits.max_daily_loss_frac * 100.0);
            halt(c, XAU_HALT_DAILY_LOSS, buf);
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_DAILY_LOSS, "daily loss");
            return XAU_OK;
        }
    }
    const double dd_base = c.limits.initial_balance > 0.0 ? c.limits.initial_balance
                                                          : c.st.peak_equity;
    if (dd_base > 0.0) {
        const double dd = 1.0 - m.equity / dd_base;
        if (dd >= c.limits.max_drawdown_frac - kLimitEps) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "drawdown %.2f%% from %s reached the %.2f%% limit",
                          dd * 100.0, c.limits.initial_balance > 0.0 ? "initial" : "peak",
                          c.limits.max_drawdown_frac * 100.0);
            halt(c, XAU_HALT_MAX_DRAWDOWN, buf);
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_MAX_DRAWDOWN, "max drawdown");
            return XAU_OK;
        }
    }

    // A book we do not understand is not one to add to. Gross, not net: a long
    // and a short of the same size net to zero and are still two positions'
    // worth of exposure to a gap.
    if (m.foreign_positions > 0) {
        halt(c, XAU_HALT_RECONCILE_DRIFT, "a position on this symbol is not ours");
        set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_RECONCILE_DRIFT, "foreign position");
        return XAU_OK;
    }
    if (m.own_positions > c.limits.max_open_positions) {
        halt(c, XAU_HALT_RECONCILE_DRIFT, "more of our positions open than allowed");
        set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_RECONCILE_DRIFT, "position drift");
        return XAU_OK;
    }
    if (m.gross_lots > c.limits.max_lots + 1e-9) {
        halt(c, XAU_HALT_RECONCILE_DRIFT, "gross open lots exceed the limit");
        set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_RECONCILE_DRIFT, "lot drift");
        return XAU_OK;
    }

    // --- the pending order ---------------------------------------------------
    if (c.pending == Pending::AwaitOpen && m.own_positions > 0 && m.pos_side == c.pending_side) {
        c.pending = Pending::None;
        c.last_message = "position open at the broker";
    } else if (c.pending == Pending::AwaitFlat && m.own_positions == 0) {
        c.pending = Pending::None;
        c.last_message = "position closed at the broker";
    }
    if (pending_overdue(c, m.now_ms)) {
        if (c.pending == Pending::AwaitOpen && m.own_orders == 0) {
            // Filled, but never seen open: a tight stop or target can close it
            // before the next quote arrives, and a placed order the broker
            // then cancelled leaves nothing either. Nothing of ours is open or
            // working, so the book is understood: no halt.
            c.pending = Pending::None;
            c.last_message = "entry never seen open (closed at once by its stop, or cancelled)";
        } else if (c.pending == Pending::AwaitFlat) {
            // The close was accepted and the position is still there. Not a
            // reason to give up on the exit: it is owed, and retried below.
            c.pending = Pending::None;
            c.close_owed = true;
            c.last_message = "close accepted but the position is still open; retrying";
        } else {
            const std::string what = c.pending == Pending::AwaitOpen
                                         ? std::string("order still working after the timeout")
                                         : std::string("order never confirmed (") +
                                               pending_name(c.pending) + ")";
            halt(c, XAU_HALT_RECONCILE_DRIFT, what);
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_RECONCILE_DRIFT,
                         "order unconfirmed");
            return XAU_OK;
        }
    }
    if (c.pending != Pending::None) {
        set_decision(out, XAU_ACTION_NONE, XAU_HALT_NONE, pending_name(c.pending));
        return XAU_OK;
    }

    // --- an exit still owed --------------------------------------------------
    if (c.close_owed) {
        if (m.own_positions == 0) {
            c.close_owed = false;
            c.close_attempts = 0;
            c.last_message = "position closed at the broker";
            // falls through: the strategy sees this tick, as it would have
        } else if (c.close_attempts >= kCloseAttempts) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "could not close the position in %d attempts",
                          c.close_attempts);
            halt(c, XAU_HALT_RECONCILE_DRIFT, buf);
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_RECONCILE_DRIFT,
                         "close failed");
            return XAU_OK;
        } else if (m.now_ms < c.close_last_try_ms ||
                   m.now_ms - c.close_last_try_ms >= kCloseRetryGapMs) {
            ++c.close_attempts;
            emit_close(c, m.now_ms, out);
            return XAU_OK;
        } else {
            set_decision(out, XAU_ACTION_NONE, XAU_HALT_NONE, "close owed; retrying shortly");
            return XAU_OK;
        }
    }

    // --- the strategy --------------------------------------------------------
    if (!c.armed()) {
        set_decision(out, XAU_ACTION_NONE, XAU_HALT_NONE, "no strategy armed");
        return XAU_OK;
    }

    if (d.kind == Decision::Kind::Close) {
        if (m.own_positions > 0) {
            c.close_reason = d.reason != nullptr ? d.reason : "close";
            c.close_attempts = 1;
            emit_close(c, m.now_ms, out);
            return XAU_OK;
        }
        set_decision(out, XAU_ACTION_NONE, XAU_HALT_NONE, "close: already flat");
        return XAU_OK;
    }

    if (d.kind == Decision::Kind::Enter) {
        // Closing is always allowed; opening is what these refuse. A stale
        // quote and a blown-out spread are both conditions to wait out, not
        // reasons to halt: the stop orders already at the broker protect what
        // is open.
        if (m.own_positions > 0) {
            refuse(c, out, XAU_HALT_NONE, "already in a position");
            return XAU_OK;
        }
        if (m.own_orders > 0) {
            // A working order of ours could fill at any moment; a second entry
            // beside it is a double position.
            refuse(c, out, XAU_HALT_NONE, "an order of ours is still working");
            return XAU_OK;
        }
        if (m.session_open != 0 && c.limits.max_quote_age_ms > 0 &&
            m.now_ms - m.tick_time_ms > c.limits.max_quote_age_ms) {
            refuse(c, out, XAU_HALT_STALE_QUOTES, "quote is stale");
            return XAU_OK;
        }
        if (m.ask - m.bid > c.limits.max_spread) {
            refuse(c, out, XAU_HALT_SPREAD_BLOWOUT, "spread too wide");
            return XAU_OK;
        }
        emit_entry(c, d, m, out);
        return XAU_OK;
    }

    set_decision(out, XAU_ACTION_NONE, XAU_HALT_NONE, "");
    return XAU_OK;
}

int32_t fail_closed(Context* c, xau_decision* out, const char* what) noexcept {
    if (c != nullptr) {
        try {
            halt(*c, XAU_HALT_MANUAL, std::string("internal error: ") + what);
        } catch (...) {
            c->st.halted = true;
            c->st.halt_reason = XAU_HALT_MANUAL;
        }
    }
    set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_MANUAL, "internal error");
    return XAU_ERR_INTERNAL;
}

}  // namespace

int32_t XAU_CALL xau_abi_version(void) { return XAU_BRIDGE_ABI_VERSION; }

int32_t XAU_CALL xau_struct_size(int32_t which) {
    switch (which) {
        case 0: return static_cast<int32_t>(sizeof(xau_market));
        case 1: return static_cast<int32_t>(sizeof(xau_decision));
        case 2: return static_cast<int32_t>(sizeof(xau_limits));
        case 3: return static_cast<int32_t>(sizeof(xau_strategy_config));
        case 4: return static_cast<int32_t>(sizeof(xau_rate));
        default: return 0;
    }
}

void* XAU_CALL xau_create(const char* symbol, int32_t abi_version, const xau_limits* limits) {
    try {
        if (abi_version != XAU_BRIDGE_ABI_VERSION) return nullptr;
        if (symbol == nullptr || limits == nullptr) return nullptr;

        auto c = std::make_unique<Context>();
        c->symbol = symbol;
        c->limits = *limits;
        // The two paths come from another language; terminate them ourselves.
        c->limits.kill_file[sizeof(c->limits.kill_file) - 1] = '\0';
        c->limits.state_file[sizeof(c->limits.state_file) - 1] = '\0';
        c->kill_path = utf8_path(c->limits.kill_file, sizeof(c->limits.kill_file));
        c->state_path = utf8_path(c->limits.state_file, sizeof(c->limits.state_file));

        // A zero limit means "unset", not "no limit". Reading it as no limit is
        // how a config typo removes the drawdown guard without any error.
        auto& L = c->limits;
        if (L.max_daily_loss_frac <= 0.0) L.max_daily_loss_frac = 0.04;
        if (L.max_drawdown_frac <= 0.0) L.max_drawdown_frac = 0.08;
        if (L.max_spread <= 0.0) L.max_spread = 1.00;
        if (L.max_lots <= 0.0) L.max_lots = 0.10;
        if (L.max_open_positions <= 0) L.max_open_positions = 1;
        if (L.max_quote_age_ms <= 0) L.max_quote_age_ms = 10'000;
        if (L.pending_timeout_ms <= 0) L.pending_timeout_ms = 30'000;
        if (L.initial_balance < 0.0) L.initial_balance = 0.0;

        // Claimed before the file is read or written, so a second context on
        // the same file touches nothing. Released by ~Context.
        if (!c->state_path.empty()) {
            std::error_code ec;
            fs::path key = fs::weakly_canonical(c->state_path, ec);
            if (ec || key.empty()) key = c->state_path.lexically_normal();
            const std::lock_guard<std::mutex> lk(registry_mutex());
            if (!registry().insert(display(key)).second) return nullptr;
            c->registered = display(key);
        }

        load_state(*c);
        if (kill_file_exists(*c) && !c->st.halted) {
            c->st.halted = true;
            c->st.halt_reason = XAU_HALT_KILL_FILE;
            c->st.halt_message = "kill file present at start-up";
            save_state(*c);
        }
        c->last_message = c->st.halted ? "starting HALTED: " + c->st.halt_message
                                       : "bridge ready for " + c->symbol;
        return c.release();
    } catch (...) {
        return nullptr;
    }
}

void XAU_CALL xau_destroy(void* ctx) {
    Context* c = as_ctx(ctx);
    if (c == nullptr) return;
    if (c->state_dirty) save_state(*c);   // last chance for a write that kept failing
    c->magic = 0;   // poison, so a double free is caught by as_ctx
    delete c;
}

int32_t XAU_CALL xau_arm(void* ctx, const xau_strategy_config* cfg) {
    Context* c = as_ctx(ctx);
    if (c == nullptr) return XAU_ERR_BAD_CONTEXT;
    if (cfg == nullptr) return XAU_ERR_BAD_ARGUMENT;
    try {
        if (c->armed()) {
            c->last_message = "already armed; re-create the context to change strategy";
            return XAU_ERR_REFUSED;
        }
        xau_strategy_config k = *cfg;
        k.strategy[sizeof(k.strategy) - 1] = '\0';
        const std::string name = cstr(k.strategy, sizeof(k.strategy));
        const xau::BaselineEntry* entry = xau::find_baseline(name.c_str());
        if (entry == nullptr) {
            c->last_message = "unknown strategy: '" + name + "'";
            return XAU_ERR_BAD_ARGUMENT;
        }
        if (k.timeframe < 0 || k.timeframe >= static_cast<int32_t>(xau::Timeframe::COUNT)) {
            c->last_message = "unknown timeframe";
            return XAU_ERR_BAD_ARGUMENT;
        }
        if (!(k.contract_size > 0.0) || !(k.volume_min > 0.0) || !(k.volume_step > 0.0) ||
            k.volume_max < k.volume_min) {
            c->last_message = "contract terms missing or inconsistent";
            return XAU_ERR_BAD_ARGUMENT;
        }
        if (!(k.fixed_lots > 0.0) && !(k.risk_per_trade > 0.0 && k.risk_per_trade <= 0.05)) {
            // Over 5% a trade is not position sizing, it is a coin toss with the
            // account. Refused rather than clamped, so the mistake is seen.
            c->last_message = "set fixed_lots, or risk_per_trade in (0, 0.05]";
            return XAU_ERR_BAD_ARGUMENT;
        }
        if (k.point_den <= 0) k.point_den = xau::XAUUSD_POINT_DEN;
        if (k.stops_level_pts < 0) k.stops_level_pts = 0;
        if (k.max_history_bars < 0) k.max_history_bars = 0;

        xau::SymbolSpec spec;
        spec.name = c->symbol;
        spec.point_num = 1;
        spec.point_den = k.point_den;
        spec.contract_size = k.contract_size;
        spec.volume_min = k.volume_min;
        spec.volume_max = k.volume_max;
        spec.volume_step = k.volume_step;
        spec.stops_level_pts = k.stops_level_pts;

        // Lots passed to the factory become the strategy's own decision.lots;
        // zero hands sizing to the risk layer, as the engine does.
        auto strategy = entry->make(k.fixed_lots > 0.0 ? k.fixed_lots : 0.0);
        if (!strategy) {
            c->last_message = "strategy factory failed";
            return XAU_ERR_INTERNAL;
        }
        auto session = std::make_unique<xau::LiveSession>(
            static_cast<xau::Timeframe>(k.timeframe), *strategy,
            static_cast<std::size_t>(k.max_history_bars));
        session->start(spec);

        c->cfg = k;
        c->spec = spec;
        c->strategy = std::move(strategy);
        c->session = std::move(session);
        c->last_message = "armed " + name + " on " +
                          xau::timeframe_name(static_cast<xau::Timeframe>(k.timeframe));
        return XAU_OK;
    } catch (const std::exception& e) {
        c->last_message = std::string("arm failed: ") + e.what();
        return XAU_ERR_BAD_ARGUMENT;
    } catch (...) {
        c->last_message = "arm failed";
        return XAU_ERR_INTERNAL;
    }
}

int32_t XAU_CALL xau_warmup(void* ctx, const xau_rate* rates, int32_t n) {
    Context* c = as_ctx(ctx);
    if (c == nullptr) return XAU_ERR_BAD_CONTEXT;
    if (!c->armed()) return XAU_ERR_NOT_INITIALISED;
    if (n < 0 || (n > 0 && rates == nullptr)) return XAU_ERR_BAD_ARGUMENT;
    try {
        const xau::Position flat{};
        std::size_t         fed = 0, skipped = 0;
        for (int32_t i = 0; i < n; ++i) {
            const xau_rate& r = rates[i];
            if (!(r.low > 0.0) || r.high < r.low || r.open < r.low || r.open > r.high ||
                r.close < r.low || r.close > r.high || r.time_ms <= 0) {
                ++skipped;
                continue;
            }
            const TimeUs t0 = static_cast<TimeUs>(r.time_ms) * 1000;
            if (t0 < c->last_tick_us) {
                ++skipped;   // out of order: the assembler only moves forward
                continue;
            }
            // Open, the extreme nearer the open, the other extreme, close: the
            // path a bar of that shape most plausibly took. Spread is the bar's.
            const bool   up = r.close >= r.open;
            const double path[4] = {r.open, up ? r.low : r.high, up ? r.high : r.low, r.close};
            const TimeUs at[4] = {0, 15'000'000, 30'000'000, 59'000'000};
            for (int k = 0; k < 4; ++k) {
                const double bid = path[k];
                const double ask = bid + std::max(0.0, r.spread);
                // Decisions are discarded: nothing is traded on history.
                (void)c->session->on_tick(make_tick(c->spec, t0 + at[k], bid, ask), flat, 0.0, 0.0);
            }
            c->last_tick_us = t0 + at[3];
            ++fed;
        }
        c->warmed = c->warmed || fed > 0;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "warm-up: %zu minutes fed, %zu skipped, %zu bars closed",
                      fed, skipped, c->session->bars_closed());
        c->last_message = buf;
        return XAU_OK;
    } catch (...) {
        c->last_message = "warm-up failed";
        return XAU_ERR_INTERNAL;
    }
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
        return on_tick_impl(*c, *mkt, out);
    } catch (const std::exception& e) {
        return fail_closed(c, out, e.what());
    } catch (...) {
        return fail_closed(c, out, "unknown exception");
    }
}

int32_t XAU_CALL xau_on_timer(void* ctx, int64_t now_ms, xau_decision* out) {
    if (out == nullptr) return XAU_ERR_BAD_ARGUMENT;
    Context* c = as_ctx(ctx);
    if (c == nullptr) {
        set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_MANUAL, "bad context");
        return XAU_ERR_BAD_CONTEXT;
    }
    try {
        c->last_now_ms = now_ms;
        retry_save(*c, now_ms);
        if (c->st.halted) {
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, c->st.halt_reason, "halted");
            return XAU_OK;
        }
        if (kill_file_due(*c, now_ms)) {
            halt(*c, XAU_HALT_KILL_FILE, "kill file present");
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_KILL_FILE, "kill file");
            return XAU_OK;
        }
        // Only a result that never came back. A position that has yet to show
        // open or flat is the tick's to judge: it needs the broker's book,
        // which the timer does not see, and a close still in flight is retried
        // there rather than halted on here.
        if (c->pending == Pending::AwaitResult && pending_overdue(*c, now_ms)) {
            halt(*c, XAU_HALT_RECONCILE_DRIFT,
                 std::string("order never confirmed (") + pending_name(c->pending) + ")");
            set_decision(out, XAU_ACTION_FLATTEN_AND_HALT, XAU_HALT_RECONCILE_DRIFT,
                         "order unconfirmed");
            return XAU_OK;
        }
        set_decision(out, XAU_ACTION_NONE, XAU_HALT_NONE, "");
        return XAU_OK;
    } catch (const std::exception& e) {
        return fail_closed(c, out, e.what());
    } catch (...) {
        return fail_closed(c, out, "unknown exception");
    }
}

int32_t XAU_CALL xau_order_result(void* ctx, int32_t ok, int32_t retcode, double fill_price,
                                  double lots) {
    Context* c = as_ctx(ctx);
    if (c == nullptr) return XAU_ERR_BAD_CONTEXT;
    try {
        if (c->pending != Pending::AwaitResult) {
            c->last_message = "order result with no order outstanding; ignored";
            return XAU_ERR_REFUSED;
        }
        char       buf[128];
        const bool was_entry = c->pending_side != 0;
        if (ok == 0) {
            c->pending = Pending::None;
            if (was_entry) {
                // Not retried. The decision belonged to a bar that has closed;
                // a retry at a later price is a trade the backtest never took.
                std::snprintf(buf, sizeof(buf), "entry failed (retcode %d); not retried", retcode);
            } else {
                // An exit is retried: the strategy wants out, and dropping that
                // leaves a position nobody is managing.
                c->close_owed = true;
                std::snprintf(buf, sizeof(buf), "close failed (retcode %d); retrying", retcode);
            }
            c->last_message = buf;
            return XAU_OK;
        }
        // The wait for the broker's book starts now, not at the send: an
        // order placed rather than filled has the whole timeout to show.
        c->pending = was_entry ? Pending::AwaitOpen : Pending::AwaitFlat;
        c->pending_since_ms = c->last_now_ms;
        std::snprintf(buf, sizeof(buf), "%s done: %.2f lots at %.5g (retcode %d)",
                      was_entry ? "entry" : "close", lots, fill_price, retcode);
        c->last_message = buf;
        return XAU_OK;
    } catch (...) {
        return XAU_ERR_INTERNAL;
    }
}

int32_t XAU_CALL xau_halt(void* ctx, int32_t reason) {
    Context* c = as_ctx(ctx);
    if (c == nullptr) return XAU_ERR_BAD_CONTEXT;
    try {
        // STATE_FILE is the bridge's own finding, not the caller's to claim:
        // resume treats it specially.
        if (reason <= XAU_HALT_NONE || reason >= XAU_HALT_STATE_FILE) reason = XAU_HALT_MANUAL;
        halt(*c, reason, "halted by the operator");
        return XAU_OK;
    } catch (...) {
        c->st.halted = true;
        c->st.halt_reason = XAU_HALT_MANUAL;
        return XAU_ERR_INTERNAL;
    }
}

int32_t XAU_CALL xau_resume(void* ctx) {
    Context* c = as_ctx(ctx);
    if (c == nullptr) return XAU_ERR_BAD_CONTEXT;
    try {
        if (kill_file_exists(*c)) {
            c->last_message = "resume refused: the kill file still exists";
            return XAU_ERR_REFUSED;
        }
        if (c->st.halted && c->st.halt_reason == XAU_HALT_STATE_FILE) {
            // The halt stood in for anchors we could not read. Resuming on
            // zeroed ones would hand back the day's spent loss allowance, so
            // the file is read again first.
            Persisted p;
            switch (read_state(c->state_path, p)) {
                case ReadResult::Unreadable:
                    c->last_message = "resume refused: state file still unreadable: " +
                                      display(c->state_path) + " -- fix or delete it";
                    return XAU_ERR_REFUSED;
                case ReadResult::Loaded:
                    c->st = p;
                    c->persisted_peak = p.peak_equity;
                    c->state_dirty = false;
                    if (p.halted) {
                        c->last_message = "state file read; it records a halt (" +
                                          p.halt_message + "): resume again to clear it";
                        return XAU_ERR_REFUSED;
                    }
                    c->pending = Pending::None;
                    c->last_message = "state file read; resumed by the operator";
                    return XAU_OK;
                case ReadResult::Missing:
                    // Deleted by the operator: their decision to start afresh.
                    c->st = Persisted{};
                    c->pending = Pending::None;
                    c->close_owed = false;
                    c->last_message = "state file deleted; resumed with fresh anchors";
                    save_state(*c);
                    return XAU_OK;
            }
        }
        c->st.halted = false;
        c->st.halt_reason = XAU_HALT_NONE;
        c->st.halt_message.clear();
        c->pending = Pending::None;
        c->close_owed = false;
        c->last_message = "resumed by the operator";
        save_state(*c);
        return XAU_OK;
    } catch (...) {
        return XAU_ERR_INTERNAL;
    }
}

int32_t XAU_CALL xau_is_halted(void* ctx) {
    Context* c = as_ctx(ctx);
    // An unreadable context counts as halted. The alternative is reporting
    // "running fine" for a context we cannot even validate.
    return (c == nullptr || c->st.halted) ? 1 : 0;
}

namespace {
int32_t copy_out(const std::string& msg, char* buf, int32_t buf_len) noexcept {
    if (buf == nullptr || buf_len <= 0) return XAU_ERR_BAD_ARGUMENT;
    const int32_t n = std::min<int32_t>(buf_len - 1, static_cast<int32_t>(msg.size()));
    std::memcpy(buf, msg.data(), static_cast<std::size_t>(n));
    buf[n] = '\0';
    return XAU_OK;
}
}  // namespace

int32_t XAU_CALL xau_last_message(void* ctx, char* buf, int32_t buf_len) {
    Context* c = as_ctx(ctx);
    try {
        return copy_out(c != nullptr ? c->last_message : std::string("bad context"), buf, buf_len);
    } catch (...) {
        return XAU_ERR_INTERNAL;
    }
}

int32_t XAU_CALL xau_status_text(void* ctx, char* buf, int32_t buf_len) {
    Context* c = as_ctx(ctx);
    if (c == nullptr) return copy_out("bad context", buf, buf_len);
    try {
        std::ostringstream s;
        s.setf(std::ios::fixed);
        s.precision(2);
        s << "XAU bridge  " << c->symbol << "  |  ";
        if (c->st.halted) {
            s << "HALTED: " << c->st.halt_message;
        } else {
            s << "running";
        }
        s << "\nstrategy  ";
        if (c->armed()) {
            s << cstr(c->cfg.strategy, sizeof(c->cfg.strategy)) << " on "
              << xau::timeframe_name(static_cast<xau::Timeframe>(c->cfg.timeframe)) << ", "
              << c->session->bars_closed() << " bars" << (c->warmed ? "" : " (not warmed)");
        } else {
            s << "none armed";
        }
        s << "\nday start equity " << c->st.day_start_equity << "  peak "
          << c->st.peak_equity << "  limits " << c->limits.max_daily_loss_frac * 100.0
          << "% day / " << c->limits.max_drawdown_frac * 100.0 << "% dd";
        s << "\norders  entries " << c->entries_sent << "  closes " << c->closes_sent
          << "  refused " << c->refusals;
        if (c->last_refusal[0] != '\0') s << " (last: " << c->last_refusal << ")";
        if (c->pending != Pending::None) s << "\npending  " << pending_name(c->pending);
        if (c->close_owed)
            s << "\nclose owed  attempt " << c->close_attempts << " of " << kCloseAttempts;
        if (c->state_dirty) s << "\nWARNING state NOT persisted: " << display(c->state_path);
        return copy_out(s.str(), buf, buf_len);
    } catch (...) {
        return XAU_ERR_INTERNAL;
    }
}
