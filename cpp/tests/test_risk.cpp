// The risk layer decides how much real money is exposed. Every property here
// is one that, if inverted, loses money quietly rather than crashing.

#include "harness.hpp"

#include "xau/risk.hpp"

#include <cmath>
#include <vector>

using namespace xau;

namespace {

// A trade distribution with a genuine edge: 45% win rate at 2:1, so the
// expectancy is positive and a challenge is actually passable.
std::vector<double> winning_edge(double unit = 0.01) {
    std::vector<double> v;
    for (int i = 0; i < 45; ++i) v.push_back(2.0 * unit);
    for (int i = 0; i < 55; ++i) v.push_back(-1.0 * unit);
    return v;
}

// The distribution we actually measured: negative expectancy.
std::vector<double> losing_edge(double unit = 0.01) {
    std::vector<double> v;
    for (int i = 0; i < 28; ++i) v.push_back(2.0 * unit);
    for (int i = 0; i < 72; ++i) v.push_back(-1.0 * unit);
    return v;
}

}  // namespace

XAU_TEST(pass_probability_is_not_monotonic_in_risk) {
    // The central claim of the whole module. If P(pass) rose forever with risk,
    // the correct play would be maximum leverage, and the solver would be
    // pointless. It does not: there is an interior peak.
    PropRules rules;
    const auto returns = winning_edge();

    const RiskSolution sol =
        solve_risk_scale(returns, rules, 2.0, 0.1, 6.0, 20, 2000, 42);

    CHECK(sol.curve.size() == 20);
    CHECK(sol.best_scale > 0.1);

    // The peak is interior, not at the top of the range.
    CHECK(sol.best_scale < 6.0);

    // And the far end of the range is worse than the peak.
    const double at_max = sol.curve.back().second;
    CHECK(at_max < sol.best.passed);
}

XAU_TEST(more_risk_past_the_optimum_lowers_pass_and_raises_ruin) {
    PropRules  rules;
    const auto returns = winning_edge();

    const PassResult modest = simulate_challenge(returns, rules, 1.0, 2.0, 4000, 7);
    const PassResult reckless = simulate_challenge(returns, rules, 8.0, 2.0, 4000, 7);

    // Both at once: this is the part people get backwards.
    CHECK(reckless.passed < modest.passed);
    CHECK(reckless.busted_drawdown + reckless.busted_daily >
          modest.busted_drawdown + modest.busted_daily);
}

XAU_TEST(a_losing_edge_has_no_viable_risk_fraction) {
    // The honest failure mode. With negative expectancy no amount of sizing
    // makes the challenge likely -- and the solver must say so rather than
    // returning whichever scale got the luckiest draw.
    PropRules  rules;
    const auto returns = losing_edge();

    const RiskSolution sol = solve_risk_scale(returns, rules, 2.0, 0.1, 5.0, 15, 2000, 11);
    CHECK(!sol.any_viable);
    CHECK(sol.best.passed < 0.5);
}

XAU_TEST(outcomes_partition_exactly) {
    PropRules        rules;
    const PassResult r = simulate_challenge(winning_edge(), rules, 1.0, 2.0, 3000, 5);
    const double     total = r.passed + r.busted_drawdown + r.busted_daily + r.expired;
    // Every path ends in exactly one of the four states. A gap here means
    // paths are being lost, and a lost path is usually a lost loss.
    CHECK_NEAR(total, 1.0, 1e-9);
}

XAU_TEST(trailing_drawdown_is_harder_than_static) {
    // A trailing limit ratchets up behind you, so a path that profits then
    // gives it back busts under trailing but survives under static. Firms
    // differ on this and it materially changes the answer.
    const auto returns = winning_edge();

    PropRules stat;
    stat.trailing_drawdown = false;
    PropRules trail;
    trail.trailing_drawdown = true;

    const PassResult a = simulate_challenge(returns, stat, 2.0, 2.0, 4000, 3);
    const PassResult b = simulate_challenge(returns, trail, 2.0, 2.0, 4000, 3);
    CHECK(b.passed <= a.passed);
}

XAU_TEST(a_deadline_makes_tiny_risk_fail_by_expiry) {
    // The other side of the non-monotonicity: risk too little and the clock
    // beats you. This is why "just trade small" is not a free win.
    PropRules rules;
    rules.max_days = 20;

    const PassResult tiny = simulate_challenge(winning_edge(), rules, 0.05, 2.0, 3000, 9);
    CHECK(tiny.expired > 0.8);
    CHECK(tiny.passed < 0.1);
}

XAU_TEST(sizing_risks_the_requested_fraction) {
    SymbolSpec   spec = SymbolSpec::xauusd_default();
    SizingConfig cfg;
    cfg.risk_per_trade = 0.01;
    cfg.vol_target = false;
    cfg.max_lots = 100.0;   // testing the formula, not the safety cap

    // 10,000 USD equity, 1% risk = 100 USD. Gold moves 0.10 USD per point per
    // lot, so a 1,000-point (1.00 USD) stop means 100 USD per lot -> 1.00 lot.
    const double lots = size_by_risk(10'000.0, 1000, 0.0, spec, cfg);
    CHECK_NEAR(lots, 1.0, 1e-9);

    // Half the stop distance, twice the size.
    const double tighter = size_by_risk(10'000.0, 500, 0.0, spec, cfg);
    CHECK_NEAR(tighter, 2.0, 1e-9);
}

XAU_TEST(sizing_refuses_rather_than_fudging) {
    SymbolSpec spec = SymbolSpec::xauusd_default();
    spec.stops_level_pts = 300;
    SizingConfig cfg;
    cfg.vol_target = false;

    // Inside the broker's minimum stop: the order would be REJECTED, so the
    // honest size is zero rather than a widened stop nobody asked for.
    CHECK_NEAR(size_by_risk(10'000.0, 100, 0.0, spec, cfg), 0.0, 1e-12);

    // Too small to reach the minimum lot: also zero, not a rounded-up bet
    // that silently takes more risk than requested.
    CHECK_NEAR(size_by_risk(10.0, 100'000, 0.0, spec, cfg), 0.0, 1e-12);
}

XAU_TEST(vol_targeting_shrinks_size_when_volatility_is_high) {
    SymbolSpec   spec = SymbolSpec::xauusd_default();
    SizingConfig cfg;
    cfg.risk_per_trade = 0.01;
    cfg.vol_target = true;
    cfg.target_atr_pts = 2000.0;   // 2.00 USD is "normal"
    cfg.max_lots = 100.0;

    const double normal = size_by_risk(100'000.0, 1000, 2000.0, spec, cfg);
    const double wild = size_by_risk(100'000.0, 1000, 4000.0, spec, cfg);
    const double quiet = size_by_risk(100'000.0, 1000, 1000.0, spec, cfg);

    CHECK(wild < normal);
    CHECK(quiet > normal);
    // Twice the volatility, half the size.
    CHECK_NEAR(wild, normal / 2.0, 1e-6);
}

XAU_TEST(silver_and_gold_size_differently_for_the_same_risk) {
    // Silver's contract is 5,000 oz against gold's 100, so the same stop
    // distance in points is fifty times the money. Sizing both from one spec
    // would put fifty times the intended risk on one of them.
    SizingConfig cfg;
    cfg.risk_per_trade = 0.01;
    cfg.vol_target = false;

    cfg.max_lots = 100.0;
    const double au = size_by_risk(10'000.0, 1000, 0.0, SymbolSpec::xauusd_default(), cfg);
    const double ag = size_by_risk(10'000.0, 1000, 0.0, SymbolSpec::xagusd_default(), cfg);
    CHECK(au > 0.0);
    CHECK(ag > 0.0);
    CHECK_NEAR(au / ag, 50.0, 1e-6);
}

XAU_TEST(the_lot_cap_is_a_real_ceiling) {
    // max_lots defaults to 1.0 and it BINDS -- the first version of the tests
    // above silently measured the cap instead of the sizing formula and read
    // as a sizing bug. A cap that quietly truncates is the right behaviour for
    // a risk limit and the wrong behaviour to leave untested.
    SymbolSpec   spec = SymbolSpec::xauusd_default();
    SizingConfig cfg;
    cfg.risk_per_trade = 0.01;
    cfg.vol_target = false;
    cfg.max_lots = 1.0;

    // Would be 2.00 lots on the formula; the cap holds it at 1.00.
    CHECK_NEAR(size_by_risk(10'000.0, 500, 0.0, spec, cfg), 1.0, 1e-9);

    cfg.max_lots = 5.0;
    CHECK_NEAR(size_by_risk(10'000.0, 500, 0.0, spec, cfg), 2.0, 1e-9);
}
