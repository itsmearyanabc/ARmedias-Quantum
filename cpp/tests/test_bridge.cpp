// Bridge tests: the guards, the order lifecycle, persistence, and parity.
//
// Every guard test describes a way the bridge could lose real money quietly.
// The governing property is FAIL CLOSED: whenever the bridge cannot establish
// that trading is safe, the correct output is FLATTEN_AND_HALT or, for a
// condition worth waiting out, a refused entry -- never a trade.
//
// The parity test at the end is the one the rest depend on: it drives this ABI
// tick by tick through a simulated broker and requires the trades to match
// BacktestEngine's on the same ticks, exactly. Without it, "the thing that
// trades is the thing that was tested" is a sentence, not a property.

#include "fixture.hpp"
#include "harness.hpp"

#include "xau_bridge.h"

#include "xau/engine.hpp"
#include "xau/registry.hpp"
#include "xau/session.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int64_t kT0ms = 1'704'067'200'000LL;   // 2024-01-01T00:00:00Z, a Monday
constexpr int64_t kMinMs = 60'000;
constexpr int64_t kDayMs = 86'400'000;

xau_limits default_limits() {
    xau_limits l{};
    l.max_daily_loss_frac = 0.04;
    l.max_drawdown_frac = 0.08;
    l.initial_balance = 0.0;
    l.max_spread = 1.00;
    l.max_lots = 0.10;
    l.max_open_positions = 1;
    l.max_quote_age_ms = 10'000;
    l.pending_timeout_ms = 30'000;
    return l;
}

void set_path(char (&dst)[260], const std::string& p) {
    std::memset(dst, 0, sizeof(dst));
    std::strncpy(dst, p.c_str(), sizeof(dst) - 1);
}

xau_market healthy_market(int64_t t_ms = kT0ms + 10 * kMinMs) {
    xau_market m{};
    m.tick_time_ms = t_ms;
    m.now_ms = t_ms;
    m.bid = 2650.00;
    m.ask = 2650.30;
    m.equity = 10'000.0;
    m.balance = 10'000.0;
    m.server_day = 20240101;
    m.session_open = 1;
    return m;
}

xau_strategy_config gold_config(const char* strategy, int32_t tf, double lots) {
    xau_strategy_config k{};
    std::strncpy(k.strategy, strategy, sizeof(k.strategy) - 1);
    k.timeframe = tf;
    k.fixed_lots = lots;
    k.contract_size = 100.0;
    k.volume_min = 0.01;
    k.volume_max = 100.0;
    k.volume_step = 0.01;
    k.point_den = 1000;
    return k;
}

struct Ctx {
    void* p = nullptr;
    explicit Ctx(const xau_limits& l) : p(xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l)) {}
    ~Ctx() { xau_destroy(p); }
    Ctx(const Ctx&) = delete;
    Ctx& operator=(const Ctx&) = delete;
};

xau_decision tick(void* ctx, const xau_market& m) {
    xau_decision d{};
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    return d;
}

std::string message(void* ctx) {
    char buf[256];
    xau_last_message(ctx, buf, sizeof(buf));
    return buf;
}

// Arm BuyAndHold on M1: it asks to enter on the first bar to close, which is
// the tick that crosses the first minute boundary. A strategy with a known,
// immediate entry is what lets the gating around entries be tested precisely.
void arm_buy_and_hold(void* ctx, double lots = 0.05) {
    const xau_strategy_config k = gold_config("BuyAndHold", XAU_TF_M1, lots);
    REQUIRE(xau_arm(ctx, &k) == XAU_OK);
}

}  // namespace

// ---------------------------------------------------------------------------
// the contract
// ---------------------------------------------------------------------------

XAU_TEST(abi_version_and_struct_sizes_are_exposed_and_enforced) {
    CHECK_EQ(xau_abi_version(), XAU_BRIDGE_ABI_VERSION);
    CHECK_EQ(XAU_BRIDGE_ABI_VERSION, 2);
    // The EA compares these against its own sizeof. Every size is a multiple
    // of 8 so MQL5's 1-byte packing and C's natural alignment agree.
    CHECK_EQ(xau_struct_size(0), 120);
    CHECK_EQ(xau_struct_size(1), 96);
    CHECK_EQ(xau_struct_size(2), 576);
    CHECK_EQ(xau_struct_size(3), 128);
    CHECK_EQ(xau_struct_size(4), 56);
    CHECK_EQ(xau_struct_size(5), 0);

    // A version mismatch must REFUSE, not adapt.
    const xau_limits l = default_limits();
    CHECK(xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION + 1, &l) == nullptr);
    CHECK(xau_create("XAUUSD", 1, &l) == nullptr);
    CHECK(xau_create(nullptr, XAU_BRIDGE_ABI_VERSION, &l) == nullptr);
}

XAU_TEST(a_null_or_wild_context_reports_halted_and_flattens) {
    CHECK_EQ(xau_is_halted(nullptr), 1);
    xau_decision     d{};
    const xau_market m = healthy_market();
    CHECK_EQ(xau_on_tick(nullptr, &m, &d), XAU_ERR_BAD_CONTEXT);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(xau_on_timer(nullptr, kT0ms, &d), XAU_ERR_BAD_CONTEXT);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
}

XAU_TEST(unarmed_the_bridge_runs_its_guards_and_trades_nothing) {
    Ctx c(default_limits());
    REQUIRE(c.p != nullptr);
    for (int i = 0; i < 5; ++i) {
        const xau_decision d = tick(c.p, healthy_market(kT0ms + i * kMinMs));
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    }
    CHECK_EQ(xau_is_halted(c.p), 0);
}

// ---------------------------------------------------------------------------
// guards that halt
// ---------------------------------------------------------------------------

XAU_TEST(daily_loss_measures_from_the_first_equity_of_the_broker_day) {
    Ctx c(default_limits());
    xau_market m = healthy_market();
    tick(c.p, m);                         // anchors the day at 10,000

    m.tick_time_ms = m.now_ms = m.tick_time_ms + 1000;
    m.equity = 9'610.0;                   // -3.9%: inside
    CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_NONE));

    m.tick_time_ms = m.now_ms = m.tick_time_ms + 1000;
    m.equity = 9'600.0;                   // -4.0%: the limit
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));

    // Sticky: recovering equity does not re-arm anything.
    m.equity = 10'050.0;
    CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(xau_is_halted(c.p), 1);
}

XAU_TEST(a_new_broker_day_resets_the_daily_anchor) {
    Ctx c(default_limits());
    xau_market m = healthy_market();
    tick(c.p, m);
    m.equity = 9'700.0;                   // -3% today
    tick(c.p, m);

    m.server_day = 20240102;              // tomorrow anchors at 9,700
    m.tick_time_ms = m.now_ms = m.tick_time_ms + kDayMs;
    tick(c.p, m);
    m.equity = 9'400.0;                   // -3.1% from 9,700: inside
    CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(xau_is_halted(c.p), 0);
}

XAU_TEST(drawdown_measures_from_the_peak_or_from_initial_balance) {
    {
        xau_limits l = default_limits();
        l.max_daily_loss_frac = 0.50;     // isolate the drawdown guard
        Ctx        c(l);
        xau_market m = healthy_market();
        m.equity = 12'000.0;
        tick(c.p, m);                     // peak
        m.equity = 11'000.0;              // -8.3% from the peak
        const xau_decision d = tick(c.p, m);
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
        CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_MAX_DRAWDOWN));
    }
    {
        // Funded-trader rules: the floor is fixed off the starting balance and
        // does not trail the peak.
        xau_limits l = default_limits();
        l.max_daily_loss_frac = 0.50;
        l.max_drawdown_frac = 0.10;
        l.initial_balance = 10'000.0;
        Ctx        c(l);
        xau_market m = healthy_market();
        m.equity = 12'000.0;
        tick(c.p, m);
        m.equity = 9'100.0;               // -24% from peak, -9% from initial
        CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_NONE));
        m.equity = 9'000.0;               // -10% from initial
        CHECK_EQ(tick(c.p, m).halt_reason, static_cast<int32_t>(XAU_HALT_MAX_DRAWDOWN));
    }
}

XAU_TEST(a_wide_spread_never_bypasses_the_loss_limits) {
    // The v1 bridge returned at the spread check, BEFORE the daily-loss and
    // drawdown checks: during exactly the news spikes that blow spreads out,
    // the loss limits were not looked at.
    Ctx        c(default_limits());
    xau_market m = healthy_market();
    tick(c.p, m);
    m.equity = 9'500.0;                   // -5% on the day
    m.ask = m.bid + 3.00;                 // and a 3 USD spread
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));
}

XAU_TEST(an_implausible_quote_halts) {
    Ctx        c(default_limits());
    xau_market m = healthy_market();
    m.ask = m.bid - 1.0;                  // inverted book
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_STALE_QUOTES));
}

XAU_TEST(a_foreign_position_or_too_many_of_ours_halts) {
    {
        Ctx        c(default_limits());
        xau_market m = healthy_market();
        m.foreign_positions = 1;          // a manual trade on the symbol
        m.gross_lots = 0.01;
        CHECK_EQ(tick(c.p, m).halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
    }
    {
        Ctx        c(default_limits());
        xau_market m = healthy_market();
        m.own_positions = 2;              // limit is 1
        m.pos_side = 1;
        m.pos_lots = 0.05;
        m.gross_lots = 0.10;
        CHECK_EQ(tick(c.p, m).halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
    }
}

XAU_TEST(lot_exposure_is_gross_not_net) {
    // A long and a short of 0.06 net to zero and are still 0.12 lots of
    // exposure. v1 measured net and saw nothing.
    xau_limits l = default_limits();
    l.max_open_positions = 2;
    Ctx        c(l);
    xau_market m = healthy_market();
    m.own_positions = 2;
    m.gross_lots = 0.12;
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
}

XAU_TEST(zero_limits_become_conservative_defaults_not_no_limits) {
    xau_limits l{};
    Ctx        c(l);
    REQUIRE(c.p != nullptr);
    xau_market m = healthy_market();
    tick(c.p, m);
    m.equity = 9'000.0;                   // -10% on the day
    CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
}

XAU_TEST(halt_is_sticky_and_resume_is_explicit) {
    Ctx c(default_limits());
    CHECK_EQ(xau_halt(c.p, XAU_HALT_MANUAL), XAU_OK);
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(tick(c.p, healthy_market(kT0ms + i * 1000)).action,
                 static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    }
    CHECK_EQ(xau_is_halted(c.p), 1);
    CHECK_EQ(xau_resume(c.p), XAU_OK);
    CHECK_EQ(xau_is_halted(c.p), 0);
}

XAU_TEST(the_reason_buffer_is_always_null_terminated) {
    Ctx        c(default_limits());
    xau_market m = healthy_market();
    tick(c.p, m);
    m.equity = 9'000.0;
    xau_decision d;
    std::memset(&d, 'X', sizeof(d));
    CHECK_EQ(xau_on_tick(c.p, &m, &d), XAU_OK);
    bool terminated = false;
    for (char ch : d.reason) terminated = terminated || ch == '\0';
    CHECK(terminated);
    char buf[8];
    CHECK_EQ(xau_last_message(c.p, buf, sizeof(buf)), XAU_OK);
    CHECK_EQ(buf[sizeof(buf) - 1], '\0');
    char sbuf[16];
    CHECK_EQ(xau_status_text(c.p, sbuf, sizeof(sbuf)), XAU_OK);
    CHECK_EQ(sbuf[sizeof(sbuf) - 1], '\0');
}

// ---------------------------------------------------------------------------
// persistence: what a restart of MetaTrader must not undo
// ---------------------------------------------------------------------------

XAU_TEST(a_halt_survives_a_restart) {
    fixture::TempDir dir;
    xau_limits       l = default_limits();
    set_path(l.state_file, (dir.path() / "state.txt").string());
    {
        Ctx        c(l);
        xau_market m = healthy_market();
        tick(c.p, m);
        m.equity = 9'500.0;
        CHECK_EQ(tick(c.p, m).halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));
    }
    {
        // v1 kept the halt in memory: re-attaching the EA came back armed.
        Ctx c(l);
        CHECK_EQ(xau_is_halted(c.p), 1);
        const xau_decision d = tick(c.p, healthy_market());
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
        CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));
        CHECK_EQ(xau_resume(c.p), XAU_OK);
    }
    {
        Ctx c(l);                         // and the resume persisted too
        CHECK_EQ(xau_is_halted(c.p), 0);
    }
}

XAU_TEST(the_daily_anchor_survives_a_mid_day_restart) {
    fixture::TempDir dir;
    xau_limits       l = default_limits();
    set_path(l.state_file, (dir.path() / "state.txt").string());
    xau_market m = healthy_market();
    {
        Ctx c(l);
        tick(c.p, m);                     // anchor 10,000
        m.equity = 9'800.0;               // -2%
        tick(c.p, m);
    }
    {
        // Same broker day. v1 re-anchored at whatever equity it restarted on,
        // handing back a fresh 4% after having lost 2.
        Ctx c(l);
        m.tick_time_ms = m.now_ms = m.tick_time_ms + 60'000;
        m.equity = 9'590.0;               // -4.1% from the persisted 10,000
        const xau_decision d = tick(c.p, m);
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
        CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));
    }
}

XAU_TEST(an_unreadable_state_file_starts_halted) {
    fixture::TempDir dir;
    const auto       path = dir.path() / "state.txt";
    xau_limits       l = default_limits();
    set_path(l.state_file, path.string());
    {
        Ctx c(l);                         // no file yet: a first run, clean
        CHECK_EQ(xau_is_halted(c.p), 0);
    }
    for (const char* body : {"garbage\n", "xau_bridge_state 1\nhalted=0\n",
                             "xau_bridge_state 1\nhalted=x\nhalt_reason=0\nhalt_message=\nday=1\n"
                             "day_start_equity=1\npeak_equity=1\n"}) {
        std::ofstream(path, std::ios::trunc) << body;
        Ctx c(l);
        // It might have said "halted". Not knowing is treated as if it did.
        CHECK_EQ(xau_is_halted(c.p), 1);
        CHECK_EQ(tick(c.p, healthy_market()).halt_reason,
                 static_cast<int32_t>(XAU_HALT_STATE_FILE));
    }
}

// ---------------------------------------------------------------------------
// the kill file and the timer
// ---------------------------------------------------------------------------

XAU_TEST(the_kill_file_halts_from_the_timer_without_any_tick) {
    fixture::TempDir dir;
    const auto       kill = dir.path() / "STOP";
    xau_limits       l = default_limits();
    set_path(l.kill_file, kill.string());
    Ctx c(l);

    xau_decision d{};
    CHECK_EQ(xau_on_timer(c.p, kT0ms, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));

    std::ofstream(kill) << "stop";
    // The market can be closed or the feed frozen; the operator's stop must
    // still land. v1 only looked on a tick.
    CHECK_EQ(xau_on_timer(c.p, kT0ms + 1500, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_KILL_FILE));

    // Deleting the file does not resume; resuming while it exists is refused.
    CHECK_EQ(xau_resume(c.p), XAU_ERR_REFUSED);
    std::filesystem::remove(kill);
    CHECK_EQ(xau_resume(c.p), XAU_OK);
}

XAU_TEST(a_kill_file_present_at_start_up_starts_halted) {
    fixture::TempDir dir;
    const auto       kill = dir.path() / "STOP";
    std::ofstream(kill) << "stop";
    xau_limits l = default_limits();
    set_path(l.kill_file, kill.string());
    Ctx c(l);
    CHECK_EQ(xau_is_halted(c.p), 1);
}

// ---------------------------------------------------------------------------
// entries, and what refuses them
// ---------------------------------------------------------------------------

XAU_TEST(an_armed_strategy_enters_on_the_bar_close_with_its_size) {
    Ctx c(default_limits());
    arm_buy_and_hold(c.p, 0.05);
    CHECK_EQ(tick(c.p, healthy_market(kT0ms + 5'000)).action, static_cast<int32_t>(XAU_ACTION_NONE));
    const xau_decision d = tick(c.p, healthy_market(kT0ms + kMinMs + 5'000));   // closes minute 0
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_BUY));
    CHECK_NEAR(d.lots, 0.05, 1e-12);
    CHECK_EQ(d.sl_price, 0.0);            // BuyAndHold carries no stop
}

XAU_TEST(the_lot_ceiling_cuts_an_oversized_decision) {
    Ctx c(default_limits());              // max_lots 0.10
    arm_buy_and_hold(c.p, 5.0);
    tick(c.p, healthy_market(kT0ms + 5'000));
    const xau_decision d = tick(c.p, healthy_market(kT0ms + kMinMs + 5'000));
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_BUY));
    CHECK_NEAR(d.lots, 0.10, 1e-12);
}

XAU_TEST(a_wide_spread_refuses_the_entry_without_halting) {
    Ctx c(default_limits());
    arm_buy_and_hold(c.p);
    tick(c.p, healthy_market(kT0ms + 5'000));
    xau_market m = healthy_market(kT0ms + kMinMs + 5'000);
    m.ask = m.bid + 3.00;
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_SPREAD_BLOWOUT));
    CHECK_EQ(xau_is_halted(c.p), 0);
}

XAU_TEST(a_stale_quote_refuses_the_entry_only_while_the_session_is_open) {
    {
        Ctx c(default_limits());          // max age 10 s
        arm_buy_and_hold(c.p);
        tick(c.p, healthy_market(kT0ms + 5'000));
        xau_market m = healthy_market(kT0ms + kMinMs + 5'000);
        m.now_ms = m.tick_time_ms + 15'000;   // the quote is 15 s old
        const xau_decision d = tick(c.p, m);
        // v1 computed this age and threw it away.
        CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
        CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_STALE_QUOTES));
        CHECK_EQ(xau_is_halted(c.p), 0);
    }
    {
        // The daily break: no quotes is normal when nothing trades.
        Ctx c(default_limits());
        arm_buy_and_hold(c.p);
        tick(c.p, healthy_market(kT0ms + 5'000));
        xau_market m = healthy_market(kT0ms + kMinMs + 5'000);
        m.now_ms = m.tick_time_ms + 15'000;
        m.session_open = 0;
        CHECK_EQ(tick(c.p, m).action, static_cast<int32_t>(XAU_ACTION_BUY));
    }
}

// ---------------------------------------------------------------------------
// one decision, one order
// ---------------------------------------------------------------------------

XAU_TEST(nothing_new_is_sent_until_the_order_is_confirmed) {
    Ctx c(default_limits());
    arm_buy_and_hold(c.p);
    tick(c.p, healthy_market(kT0ms + 5'000));
    REQUIRE(tick(c.p, healthy_market(kT0ms + kMinMs + 5'000)).action == XAU_ACTION_BUY);

    // No result reported yet (and inside the 30 s timeout): nothing goes out.
    xau_decision d = tick(c.p, healthy_market(kT0ms + kMinMs + 20'000));
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK(std::string(d.reason).find("awaiting") != std::string::npos);

    CHECK_EQ(xau_order_result(c.p, 1, 10009, 2650.30, 0.05), XAU_OK);
    // Filled; the position appears at the broker.
    xau_market m = healthy_market(kT0ms + kMinMs + 25'000);
    m.own_positions = 1;
    m.pos_side = 1;
    m.pos_lots = 0.05;
    m.gross_lots = 0.05;
    m.pos_open_price = 2650.30;
    m.pos_open_time_ms = kT0ms + kMinMs + 5'000;
    d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(xau_is_halted(c.p), 0);
    CHECK(message(c.p).find("open at the broker") != std::string::npos);

    // A result with no order outstanding is refused, not applied.
    CHECK_EQ(xau_order_result(c.p, 1, 10009, 2650.30, 0.05), XAU_ERR_REFUSED);
}

XAU_TEST(an_order_whose_result_never_comes_back_halts) {
    Ctx c(default_limits());              // 30 s timeout
    arm_buy_and_hold(c.p);
    tick(c.p, healthy_market(kT0ms + 5'000));
    REQUIRE(tick(c.p, healthy_market(kT0ms + kMinMs + 5'000)).action == XAU_ACTION_BUY);

    xau_decision d{};
    CHECK_EQ(xau_on_timer(c.p, kT0ms + kMinMs + 20'000, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(xau_on_timer(c.p, kT0ms + kMinMs + 40'000, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
}

XAU_TEST(a_failed_order_is_not_retried) {
    Ctx c(default_limits());
    arm_buy_and_hold(c.p);
    tick(c.p, healthy_market(kT0ms + 5'000));
    REQUIRE(tick(c.p, healthy_market(kT0ms + kMinMs + 5'000)).action == XAU_ACTION_BUY);
    CHECK_EQ(xau_order_result(c.p, 0, 10004, 0.0, 0.0), XAU_OK);   // requote
    for (int i = 2; i < 6; ++i) {
        CHECK_EQ(tick(c.p, healthy_market(kT0ms + i * kMinMs + 5'000)).action,
                 static_cast<int32_t>(XAU_ACTION_NONE));
    }
    CHECK_EQ(xau_is_halted(c.p), 0);
}

XAU_TEST(a_fill_closed_before_it_was_seen_open_is_not_a_drift) {
    Ctx c(default_limits());
    arm_buy_and_hold(c.p);
    tick(c.p, healthy_market(kT0ms + 5'000));
    REQUIRE(tick(c.p, healthy_market(kT0ms + kMinMs + 5'000)).action == XAU_ACTION_BUY);
    CHECK_EQ(xau_order_result(c.p, 1, 10009, 2650.30, 0.05), XAU_OK);
    // A tight stop took it out before the next quote: never seen open.
    const xau_decision d = tick(c.p, healthy_market(kT0ms + kMinMs + 40'000));
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(xau_is_halted(c.p), 0);
}

// ---------------------------------------------------------------------------
// arming and warm-up
// ---------------------------------------------------------------------------

XAU_TEST(arming_refuses_what_it_cannot_trade_safely) {
    Ctx c(default_limits());
    xau_strategy_config k = gold_config("NoSuchStrategy", XAU_TF_H1, 0.01);
    CHECK_EQ(xau_arm(c.p, &k), XAU_ERR_BAD_ARGUMENT);
    k = gold_config("BuyAndHold", 9, 0.01);
    CHECK_EQ(xau_arm(c.p, &k), XAU_ERR_BAD_ARGUMENT);
    k = gold_config("BuyAndHold", XAU_TF_H1, 0.0);
    k.risk_per_trade = 0.10;              // 10% a trade
    CHECK_EQ(xau_arm(c.p, &k), XAU_ERR_BAD_ARGUMENT);
    k = gold_config("BuyAndHold", XAU_TF_H1, 0.01);
    k.contract_size = 0.0;
    CHECK_EQ(xau_arm(c.p, &k), XAU_ERR_BAD_ARGUMENT);

    // Still unarmed after the refusals: nothing trades.
    tick(c.p, healthy_market(kT0ms + 5'000));
    CHECK_EQ(tick(c.p, healthy_market(kT0ms + kMinMs + 5'000)).action,
             static_cast<int32_t>(XAU_ACTION_NONE));

    k = gold_config("BuyAndHold", XAU_TF_M1, 0.01);
    CHECK_EQ(xau_arm(c.p, &k), XAU_OK);
    CHECK_EQ(xau_arm(c.p, &k), XAU_ERR_REFUSED);   // no silent swap mid-session
}

XAU_TEST(warm_up_makes_a_twelve_month_lookback_trade_on_its_first_live_bar) {
    // TimeSeriesMomentum needs a year of daily closes. Without warm-up it
    // would sit silent for a year after every restart.
    Ctx c(default_limits());
    const xau_strategy_config k = gold_config("TimeSeriesMomentum", XAU_TF_D1, 0.01);
    REQUIRE(xau_arm(c.p, &k) == XAU_OK);

    // 400 days of a slow rise, weekdays only, one rate per hour standing in
    // for the minutes (the assembler only needs the bars to close).
    std::vector<xau_rate> rates;
    const int64_t         start = kT0ms - 400 * kDayMs;
    for (int64_t t = start; t < kT0ms; t += 60 * kMinMs) {
        if (!xau::market_open(static_cast<xau::TimeUs>(t) * 1000)) continue;
        const double px = 1800.0 + 0.02 * static_cast<double>((t - start) / (60 * kMinMs));
        xau_rate r{};
        r.time_ms = t;
        r.open = px;
        r.high = px + 1.0;
        r.low = px - 1.0;
        r.close = px + 0.5;
        r.spread = 0.30;
        r.tick_volume = 100;
        rates.push_back(r);
    }
    CHECK_EQ(xau_warmup(c.p, rates.data(), static_cast<int32_t>(rates.size())), XAU_OK);
    CHECK(message(c.p).find("warm-up") != std::string::npos);

    // First live quotes: the first one closes the last warm-up day, and the
    // year behind it is up.
    xau_market m = healthy_market(kT0ms + 5'000);
    m.bid = 1800.0 + 0.02 * 9600.0;
    m.ask = m.bid + 0.30;
    m.server_day = 20240101;
    const xau_decision d = tick(c.p, m);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_BUY));
    CHECK(d.sl_price > 0.0 && d.sl_price < m.ask);   // its 4-ATR disaster stop
}

// ---------------------------------------------------------------------------
// parity: live through this ABI == the backtest engine, trade for trade
// ---------------------------------------------------------------------------

namespace {

struct SimTrade {
    int64_t entry_ms = 0, exit_ms = 0;
    int     side = 0;
    int64_t entry_pts = 0, exit_pts = 0;
    char    why = '?';
};

// A broker that fills the way the engine does with costs at zero: market
// orders at the touch, stops through gaps at the market, targets at their
// price. If the bridge and the engine disagree, this is where it shows.
std::vector<SimTrade> run_through_bridge(const std::vector<xau::Tick>& ticks, const char* strategy,
                                         int32_t tf, double lots, int& halts) {
    xau_limits l = default_limits();
    l.max_daily_loss_frac = 0.99;
    l.max_drawdown_frac = 0.99;
    l.max_spread = 1e9;
    l.max_lots = 1.0;
    void*                     ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);
    const xau_strategy_config k = gold_config(strategy, tf, lots);
    std::vector<SimTrade>     out;
    if (xau_arm(ctx, &k) != XAU_OK) {
        CHECK(false);                     // arming a registry strategy must work
        xau_destroy(ctx);
        return out;
    }

    SimTrade              open{};
    double                sl = 0.0, tp = 0.0;
    const auto            px = [](xau::Points p) { return static_cast<double>(p) / 1000.0; };
    const auto            pts = [](double v) { return static_cast<int64_t>(std::llround(v * 1000.0)); };

    for (const xau::Tick& t : ticks) {
        const int64_t ms = t.ts_us / 1000;
        const double  bid = px(t.bid_pts), ask = px(t.ask_pts());

        // 1) the broker's resting stop and target
        if (open.side != 0) {
            const double close_px = open.side > 0 ? bid : ask;
            bool         hit = false;
            if (sl > 0.0 && (open.side > 0 ? close_px <= sl : close_px >= sl)) {
                open.exit_pts = pts(open.side > 0 ? std::min(close_px, sl) : std::max(close_px, sl));
                open.why = 's';
                hit = true;
            } else if (tp > 0.0 && (open.side > 0 ? close_px >= tp : close_px <= tp)) {
                open.exit_pts = pts(tp);
                open.why = 't';
                hit = true;
            }
            if (hit) {
                open.exit_ms = ms;
                out.push_back(open);
                open = SimTrade{};
            }
        }

        // 2) the bridge
        xau_market m{};
        m.tick_time_ms = m.now_ms = ms;
        m.bid = bid;
        m.ask = ask;
        m.equity = m.balance = 10'000.0;
        m.server_day = 0;                 // falls back to the UTC date
        if (open.side != 0) {
            m.own_positions = 1;
            m.pos_side = open.side;
            m.pos_lots = lots;
            m.gross_lots = lots;
            m.pos_open_price = px(static_cast<xau::Points>(open.entry_pts));
            m.pos_open_time_ms = open.entry_ms;
            m.pos_sl = sl;
            m.pos_tp = tp;
        }
        xau_decision d{};
        xau_on_tick(ctx, &m, &d);

        // 3) execute at this tick's prices, and confirm
        if (d.action == XAU_ACTION_BUY || d.action == XAU_ACTION_SELL) {
            open = SimTrade{};
            open.side = d.action == XAU_ACTION_BUY ? 1 : -1;
            open.entry_ms = ms;
            open.entry_pts = pts(open.side > 0 ? ask : bid);
            sl = d.sl_price;
            tp = d.tp_price;
            xau_order_result(ctx, 1, 10009, open.side > 0 ? ask : bid, d.lots);
        } else if (d.action == XAU_ACTION_CLOSE && open.side != 0) {
            open.exit_ms = ms;
            open.exit_pts = pts(open.side > 0 ? bid : ask);
            open.why = 'c';
            out.push_back(open);
            open = SimTrade{};
            xau_order_result(ctx, 1, 10009, 0.0, lots);
        } else if (d.action == XAU_ACTION_FLATTEN_AND_HALT) {
            ++halts;
        }
    }
    xau_destroy(ctx);
    return out;
}

std::vector<xau::Tick> parity_ticks(int days, std::uint64_t seed) {
    std::vector<xau::Tick> v;
    xau::Points            bid = 2'000'000;
    std::uint64_t          s = seed;
    const xau::TimeUs      t0 = static_cast<xau::TimeUs>(kT0ms) * 1000;
    for (xau::TimeUs t = t0; t < t0 + static_cast<xau::TimeUs>(days) * xau::kUsPerDay;
         t += 3'000'000) {                 // a quote every 3 s, whole milliseconds
        if (!xau::market_open(t)) continue;
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        bid += static_cast<xau::Points>((s >> 59) % 31) - 15;
        const int  h = xau::utc_hour(t);
        const auto spread = static_cast<std::uint16_t>((h >= 7 && h < 17) ? 220 : 380);
        v.push_back(fixture::tick(t, bid, spread));
    }
    return v;
}

}  // namespace

XAU_TEST(bridge_trades_exactly_what_the_backtest_engine_trades) {
    const auto       ticks = parity_ticks(40, 7);
    fixture::TempDir dir;
    const auto       store = fixture::make_store(dir, ticks);

    struct Case {
        const char*    name;
        xau::Timeframe tf;
    };
    std::size_t total_trades = 0;
    for (const Case cs : {Case{"LondonOpeningRange", xau::Timeframe::M15},
                          Case{"Rsi2Extreme", xau::Timeframe::M15},
                          Case{"BollingerReversion", xau::Timeframe::M15},
                          Case{"InsideBarBreak", xau::Timeframe::H1},
                          Case{"MomentumContinuation", xau::Timeframe::M15}}) {
        const xau::BaselineEntry* e = xau::find_baseline(cs.name);
        REQUIRE(e != nullptr);

        xau::BacktestConfig cfg;
        cfg.spec = xau::SymbolSpec::xauusd_default();
        cfg.tf = cs.tf;
        cfg.apply_swap = false;
        cfg.costs.slip_base_pts = 0.0;
        cfg.costs.slip_vol_coef = 0.0;
        cfg.costs.latency_us = 0;
        cfg.costs.commission_per_lot_round_usd = 0.0;
        auto        strat = e->make(0.10);
        const auto  bt = xau::BacktestEngine(store, cfg).run(*strat);

        int        halts = 0;
        const auto live = run_through_bridge(ticks, cs.name, static_cast<int32_t>(cs.tf), 0.10, halts);
        CHECK_EQ(halts, 0);

        std::vector<const xau::Trade*> want;
        for (const xau::Trade& t : bt.trades)
            if (t.exit_reason != xau::ExitReason::EndOfData) want.push_back(&t);

        CHECK_EQ(live.size(), want.size());
        const std::size_t n = std::min(live.size(), want.size());
        bool              same = true;
        for (std::size_t i = 0; i < n; ++i) {
            const xau::Trade& b = *want[i];
            const SimTrade&   s = live[i];
            const char        why = b.exit_reason == xau::ExitReason::StopLoss   ? 's'
                                    : b.exit_reason == xau::ExitReason::TakeProfit ? 't'
                                                                                   : 'c';
            same = same && s.entry_ms * 1000 == b.entry_ts && s.exit_ms * 1000 == b.exit_ts &&
                   s.side == (b.side == xau::Side::Long ? 1 : -1) && s.entry_pts == b.entry_pts &&
                   s.exit_pts == b.exit_pts && s.why == why;
        }
        if (!same) {
            for (std::size_t i = 0; i < n; ++i) {
                const xau::Trade& b = *want[i];
                const SimTrade&   q = live[i];
                if (q.entry_ms * 1000 != b.entry_ts || q.exit_ms * 1000 != b.exit_ts ||
                    q.entry_pts != b.entry_pts || q.exit_pts != b.exit_pts) {
                    std::fprintf(stderr, "    %s #%zu engine %lld..%lld %d->%d r%d | bridge %lld..%lld %lld->%lld %c\n",
                                 cs.name, i, (long long)b.entry_ts / 1000, (long long)b.exit_ts / 1000,
                                 b.entry_pts, b.exit_pts, (int)b.exit_reason, (long long)q.entry_ms,
                                 (long long)q.exit_ms, (long long)q.entry_pts, (long long)q.exit_pts, q.why);
                    break;
                }
            }
        }
        CHECK(same);
        total_trades += want.size();
    }
    // A parity test that compares two empty lists proves nothing.
    CHECK(total_trades > 50);
}
