// Bridge guard tests.
//
// Every test here describes a way the bridge could lose real money quietly.
// The governing property is FAIL CLOSED: whenever the bridge cannot establish
// that trading is safe, the correct output is FLATTEN_AND_HALT, never "no
// action" -- because "no action" leaves an open position running through
// exactly the condition that triggered the doubt.

#include "harness.hpp"

#include "xau_bridge.h"

#include <cstring>
#include <string>

namespace {

xau_limits default_limits() {
    xau_limits l{};
    l.max_daily_loss_frac = 0.04;
    l.max_drawdown_frac = 0.08;
    l.max_spread_usd = 1.00;
    l.max_lots = 0.10;
    l.max_open_positions = 1;
    l.max_quote_age_ms = 10'000;
    l.kill_file[0] = '\0';
    return l;
}

xau_market healthy_market() {
    xau_market m{};
    m.time_ms = 1'700'000'000'000LL;
    m.bid = 2650.00;
    m.ask = 2650.30;
    m.equity = 10'000.0;
    m.balance = 10'000.0;
    m.day_start_equity = 10'000.0;
    m.peak_equity = 10'000.0;
    m.open_positions = 0;
    m.open_lots = 0.0;
    return m;
}

}  // namespace

XAU_TEST(abi_version_is_exposed_and_enforced) {
    CHECK_EQ(xau_abi_version(), XAU_BRIDGE_ABI_VERSION);

    // A version mismatch must REFUSE, not adapt. MQL5 cannot see our headers,
    // so a struct layout change with a stale EA silently reinterprets fields --
    // a "lots" value read as a "price" places an order a thousand times too big.
    const xau_limits l = default_limits();
    CHECK(xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION + 1, &l) == nullptr);
    CHECK(xau_create("XAUUSD", 0, &l) == nullptr);
}

XAU_TEST(a_null_or_wild_context_reports_halted_and_flattens) {
    // MQL5 hands back whatever handle it was told to keep, including 0 after a
    // failed init. Reporting "running fine" for a context we cannot validate is
    // the worst available answer.
    CHECK_EQ(xau_is_halted(nullptr), 1);

    xau_decision d{};
    const xau_market m = healthy_market();
    CHECK_EQ(xau_on_tick(nullptr, &m, &d), XAU_ERR_BAD_CONTEXT);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
}

XAU_TEST(a_healthy_tick_produces_no_action_not_a_trade) {
    // No strategy is armed until one clears Phase 6. The bridge must sit still
    // rather than invent something to do.
    const xau_limits l = default_limits();
    void*            ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);
    CHECK(ctx != nullptr);

    xau_decision     d{};
    const xau_market m = healthy_market();
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(xau_is_halted(ctx), 0);
    xau_destroy(ctx);
}

XAU_TEST(daily_loss_limit_flattens_and_halts) {
    const xau_limits l = default_limits();
    void*            ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);
    CHECK(ctx != nullptr);

    xau_market m = healthy_market();
    m.equity = 9'600.0;   // -4.0% exactly on the day

    xau_decision d{};
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_DAILY_LOSS));

    // And it STAYS halted once the equity recovers. A limit that clears itself
    // when the market bounces is not a limit.
    m.equity = 10'050.0;
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(xau_is_halted(ctx), 1);
    xau_destroy(ctx);
}

XAU_TEST(drawdown_limit_measures_from_the_peak) {
    const xau_limits l = default_limits();
    void*            ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);

    xau_market m = healthy_market();
    // Up to 12,000 then back to 11,000: only -8.3% from peak, but the balance
    // is still well ABOVE where the day started. A guard that only watched the
    // day would see a profit and wave this through.
    m.peak_equity = 12'000.0;
    m.equity = 11'000.0;
    m.day_start_equity = 10'500.0;

    xau_decision d{};
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_MAX_DRAWDOWN));
    xau_destroy(ctx);
}

XAU_TEST(a_wide_spread_blocks_entry_without_halting) {
    // A blowout is temporary. Halting on every news tick would take the system
    // offline daily, so this refuses to OPEN while leaving management alive.
    const xau_limits l = default_limits();
    void*            ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);

    xau_market m = healthy_market();
    m.ask = m.bid + 3.00;   // 3 USD spread against a 1 USD limit

    xau_decision d{};
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_NONE));
    CHECK_EQ(xau_is_halted(ctx), 0);   // NOT halted

    // And it recovers on its own when the spread comes back in.
    m.ask = m.bid + 0.30;
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    CHECK_EQ(xau_is_halted(ctx), 0);
    xau_destroy(ctx);
}

XAU_TEST(an_implausible_quote_halts) {
    const xau_limits l = default_limits();
    void*            ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);

    xau_market m = healthy_market();
    m.ask = m.bid - 1.0;   // inverted book: the feed is broken

    xau_decision d{};
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_STALE_QUOTES));
    xau_destroy(ctx);
}

XAU_TEST(unexpected_positions_halt_as_reconciliation_drift) {
    // Our view and the broker's have diverged. Adding to a book we do not
    // understand is how a small bug becomes an unbounded one.
    const xau_limits l = default_limits();
    void*            ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);

    xau_market m = healthy_market();
    m.open_positions = 3;   // limit is 1

    xau_decision d{};
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
    xau_destroy(ctx);
}

XAU_TEST(excess_lots_halt_even_with_one_position) {
    const xau_limits l = default_limits();
    void*            ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);

    xau_market m = healthy_market();
    m.open_positions = 1;
    m.open_lots = -2.50;   // short 2.5 lots against a 0.10 limit

    xau_decision d{};
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    CHECK_EQ(d.halt_reason, static_cast<int32_t>(XAU_HALT_RECONCILE_DRIFT));
    xau_destroy(ctx);
}

XAU_TEST(halt_is_sticky_and_resume_is_explicit) {
    const xau_limits l = default_limits();
    void*            ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);

    CHECK_EQ(xau_halt(ctx, XAU_HALT_MANUAL), XAU_OK);
    CHECK_EQ(xau_is_halted(ctx), 1);

    // Many ticks must not clear it. Only an explicit resume does -- a system
    // that re-arms itself after a loss limit does not have a loss limit.
    xau_decision     d{};
    const xau_market m = healthy_market();
    for (int i = 0; i < 50; ++i) {
        CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    }
    CHECK_EQ(xau_is_halted(ctx), 1);

    CHECK_EQ(xau_resume(ctx), XAU_OK);
    CHECK_EQ(xau_is_halted(ctx), 0);
    xau_destroy(ctx);
}

XAU_TEST(zero_limits_become_conservative_defaults_not_no_limits) {
    // A zero field means "unset". Reading it as "no limit" is how a config typo
    // silently removes the drawdown guard.
    xau_limits l{};   // everything zero
    void*      ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);
    CHECK(ctx != nullptr);

    xau_market m = healthy_market();
    m.equity = 9'000.0;   // -10% on the day: past any sane default

    xau_decision d{};
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);
    CHECK_EQ(d.action, static_cast<int32_t>(XAU_ACTION_FLATTEN_AND_HALT));
    xau_destroy(ctx);
}

XAU_TEST(the_reason_buffer_is_always_null_terminated) {
    const xau_limits l = default_limits();
    void*            ctx = xau_create("XAUUSD", XAU_BRIDGE_ABI_VERSION, &l);

    xau_decision d{};
    std::memset(d.reason, 'X', sizeof(d.reason));   // no terminator anywhere
    xau_market m = healthy_market();
    m.equity = 9'000.0;
    CHECK_EQ(xau_on_tick(ctx, &m, &d), XAU_OK);

    // MQL5 will read this as a C string; an unterminated buffer walks off the
    // end of the struct.
    bool terminated = false;
    for (char ch : d.reason) {
        if (ch == '\0') { terminated = true; break; }
    }
    CHECK(terminated);

    char buf[16];
    std::memset(buf, 'Y', sizeof(buf));
    CHECK_EQ(xau_last_message(ctx, buf, sizeof(buf)), XAU_OK);
    CHECK_EQ(buf[sizeof(buf) - 1], '\0');
    xau_destroy(ctx);
}
