// Phase 1 gate tests.
//
// An unvalidated backtester is a random number generator with good typography.
// Two tests here are the gate itself:
//
//   buy_and_hold_reconciles_to_hand_calculation
//       every number worked out on paper, matched to the cent
//
//   null_model_expectancy_equals_predicted_cost
//       random entries on a flat price must lose EXACTLY the round-turn cost.
//       Zero sampling noise by construction, so it is a direct test that the
//       fill model and the cost model agree with each other.
//
// The rest pin the fill behaviours that separate an honest engine from a
// flattering one: stops slip and fill through gaps, limits do not.

#include "fixture.hpp"
#include "harness.hpp"

#include "xau/baselines.hpp"
#include "xau/engine.hpp"

#include <vector>

using namespace xau;
using fixture::kMinute;
using fixture::kT0;
using fixture::tick;

namespace {

// Frictionless: every cost switched off so a hand calculation is possible.
BacktestConfig bare_config() {
    BacktestConfig c;
    c.spec = SymbolSpec::xauusd_default();
    c.spec.stops_level_pts = 0;
    c.spec.swap_long_pts = 0.0;
    c.spec.swap_short_pts = 0.0;
    c.costs.slip_base_pts = 0.0;
    c.costs.slip_vol_coef = 0.0;
    c.costs.latency_us = 0;
    c.costs.commission_per_lot_round_usd = 0.0;
    c.tf = Timeframe::M15;
    c.initial_balance = 10'000.0;
    c.apply_swap = false;
    return c;
}

// Ticks one minute apart, bid rising by `slope` points each minute.
std::vector<Tick> ramp(int n, Points bid0 = 2'650'000, Points slope = 100,
                       std::uint16_t spread = 200) {
    std::vector<Tick> v;
    v.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        v.push_back(tick(kT0 + static_cast<TimeUs>(i) * kMinute,
                         static_cast<Points>(bid0 + slope * i), spread));
    }
    return v;
}

// Enters once with a chosen stop and target, then never acts again.
class ScriptedEntry final : public Strategy {
public:
    ScriptedEntry(Side side, Points sl, Points tp, double lots)
        : side_(side), sl_(sl), tp_(tp), lots_(lots) {}

    [[nodiscard]] const char* name() const noexcept override { return "Scripted"; }
    [[nodiscard]] Decision on_bar(const BarContext& c) override {
        if (done_ || c.position.is_open()) return Decision::hold();
        done_ = true;
        Decision d = Decision::enter(side_, sl_, tp_, "scripted");
        d.lots = lots_;
        return d;
    }

private:
    Side   side_;
    Points sl_;
    Points tp_;
    double lots_;
    bool   done_ = false;
};

// CHECK_EQ streams both operands to build its failure message, and a scoped
// enum has no operator<<. Comparing the underlying values keeps the harness
// dependency-free rather than adding stream operators to the core library.
constexpr int as_int(Side s) noexcept { return static_cast<int>(s); }
constexpr int as_int(ExitReason r) noexcept { return static_cast<int>(r); }

}  // namespace

// ---------------------------------------------------------------------------
// GATE 1 — reconciliation against a hand calculation
// ---------------------------------------------------------------------------

XAU_TEST(buy_and_hold_reconciles_to_hand_calculation) {
    fixture::TempDir dir;
    // 60 one-minute ticks. Bid starts at 2,650.000 USD and rises 0.100 USD/min.
    // Spread is a flat 200 pts = 0.200 USD.
    const TickStore store = fixture::make_store(dir, ramp(60));

    BacktestConfig cfg = bare_config();
    BacktestEngine engine(store, cfg);
    BuyAndHold     strat(0.10, Side::Long);
    const BacktestResult r = engine.run(strat);

    // Worked by hand:
    //   M15 bar 0 spans [t0, t0+15m) and closes on the tick at t0+15m.
    //   Latency is zero, so the order fills on that same tick, i = 15.
    //     entry  = ask = bid(15) + spread = 2,651,500 + 200 = 2,651,700 pts
    //   close_at_end sells the bid on the final tick, i = 59.
    //     exit   = bid(59)                                 = 2,655,900 pts
    //   move     = 2,655,900 - 2,651,700                   =     4,200 pts
    //   USD/pt/lot = contract 100 oz x 0.001 USD           =      0.100
    //   gross    = 4,200 x 0.100 x 0.10 lots               =     42.00 USD
    REQUIRE(r.trades.size() == 1);
    const Trade& t = r.trades[0];

    CHECK_EQ(as_int(t.side), as_int(Side::Long));
    CHECK_EQ(t.entry_pts, Points{2'651'700});
    CHECK_EQ(t.exit_pts, Points{2'655'900});
    CHECK_EQ(t.entry_ts, kT0 + 15 * kMinute);
    CHECK_EQ(t.exit_ts, kT0 + 59 * kMinute);
    CHECK_EQ(as_int(t.exit_reason), as_int(ExitReason::EndOfData));
    CHECK_NEAR(t.lots, 0.10, 1e-12);
    CHECK_NEAR(t.gross_usd, 42.00, 1e-6);
    CHECK_NEAR(t.commission_usd, 0.0, 1e-12);
    CHECK_NEAR(t.swap_usd, 0.0, 1e-12);
    CHECK_NEAR(t.net_usd, 42.00, 1e-6);
    CHECK_NEAR(r.final_balance, 10'042.00, 1e-6);
    CHECK_NEAR(r.metrics.net_profit, 42.00, 1e-6);

    // Excursions are measured on the side we would exit at, so a long that
    // filled on the ask starts one spread underwater and only crosses into
    // profit once the bid passes the entry. MAE is therefore -100, not 0: at
    // the first tick after the fill the bid was 2,651,600 against an entry of
    // 2,651,700. Paying the spread on entry is real, and the number should
    // show it.
    CHECK_EQ(t.mae_pts, Points{-100});
    CHECK_EQ(t.mfe_pts, Points{4'200});
}

XAU_TEST(short_side_reconciles_too) {
    fixture::TempDir dir;
    // Falling market: bid drops 0.100 USD/min, so a short gains.
    const TickStore store = fixture::make_store(dir, ramp(60, 2'650'000, -100));

    BacktestConfig cfg = bare_config();
    BacktestEngine engine(store, cfg);
    BuyAndHold     strat(0.10, Side::Short);
    const BacktestResult r = engine.run(strat);

    // entry sells the bid: bid(15) = 2,650,000 - 1,500 = 2,648,500
    // exit buys the ask:   bid(59) + 200 = 2,644,100 + 200 = 2,644,300
    // move (short) = -(2,644,300 - 2,648,500) = 4,200 pts in favour
    REQUIRE(r.trades.size() == 1);
    const Trade& t = r.trades[0];
    CHECK_EQ(t.entry_pts, Points{2'648'500});
    CHECK_EQ(t.exit_pts, Points{2'644'300});
    CHECK_NEAR(t.gross_usd, 42.00, 1e-6);
}

// ---------------------------------------------------------------------------
// GATE 2 — the cost model, tested exactly
// ---------------------------------------------------------------------------

XAU_TEST(null_model_expectancy_equals_predicted_cost) {
    fixture::TempDir dir;
    // Perfectly flat price. With no price movement the expected gross P&L of a
    // random entry is not merely zero on average, it is zero on every single
    // trade — so the measured expectancy must equal the predicted round-turn
    // cost with no sampling error at all.
    const TickStore store = fixture::make_store(dir, ramp(30'000, 2'650'000, 0, 200));

    BacktestConfig cfg = bare_config();
    cfg.costs.slip_base_pts = 30.0;
    cfg.costs.slip_vol_coef = 0.0;
    cfg.costs.commission_per_lot_round_usd = 7.0;

    RandomEntry::Config rc;
    rc.entry_prob = 0.02;
    rc.hold_bars = 8;
    rc.lots = 0.10;
    RandomEntry strat(rc);

    BacktestEngine       engine(store, cfg);
    const BacktestResult r = engine.run(strat);

    //   spread          200 pts   paid once: buy the ask, sell the bid
    //   slippage     2 x 30 pts   once in, once out
    //   total        =  260 pts x 0.100 USD/pt/lot x 0.10 lots = 2.60 USD
    //   commission    7.00 USD/lot x 0.10 lots                 = 0.70 USD
    const double predicted = -(200.0 + 2 * 30.0) * 0.100 * 0.10 - 7.00 * 0.10;
    CHECK_NEAR(predicted, -3.30, 1e-12);

    REQUIRE(r.trades.size() > 10);
    CHECK_NEAR(r.metrics.expectancy_usd, predicted, 1e-9);

    // Every individual trade, not just the average.
    bool all_match = true;
    for (const Trade& t : r.trades) {
        if (std::fabs(t.net_usd - predicted) > 1e-9) all_match = false;
    }
    CHECK(all_match);

    // A cost-only null model must lose money, and must lose it monotonically.
    CHECK(r.metrics.net_profit < 0.0);
    CHECK_EQ(r.metrics.wins, std::size_t{0});
    CHECK_NEAR(r.final_balance,
               10'000.0 + predicted * static_cast<double>(r.trades.size()), 1e-6);
}

XAU_TEST(stress_multipliers_double_the_cost) {
    fixture::TempDir dir;
    const TickStore store = fixture::make_store(dir, ramp(30'000, 2'650'000, 0, 200));

    BacktestConfig cfg = bare_config();
    cfg.costs.slip_base_pts = 30.0;
    cfg.costs.commission_per_lot_round_usd = 0.0;
    cfg.costs = cfg.costs.stressed(2.0);   // section 8 requires surviving this

    RandomEntry::Config rc;
    rc.entry_prob = 0.02;
    rc.hold_bars = 8;
    rc.lots = 0.10;
    RandomEntry strat(rc);

    const BacktestResult r = BacktestEngine(store, cfg).run(strat);

    // spread 400, slippage 2 x 60 -> 520 pts
    const double predicted = -(400.0 + 2 * 60.0) * 0.100 * 0.10;
    REQUIRE(r.trades.size() > 10);
    CHECK_NEAR(r.metrics.expectancy_usd, predicted, 1e-9);
}

// ---------------------------------------------------------------------------
// fill behaviour
// ---------------------------------------------------------------------------

XAU_TEST(stops_fill_through_the_gap_not_at_the_stop_price) {
    fixture::TempDir dir;
    std::vector<Tick> ticks = ramp(16, 2'650'000, 0, 200);   // flat, entry at i=15
    // The market jumps straight through the stop, as it does over a weekend or
    // a data release. There was no trade at the stop price, so we must not
    // pretend to have got one.
    for (int i = 16; i < 22; ++i) {
        ticks.push_back(tick(kT0 + static_cast<TimeUs>(i) * kMinute, 2'640'000, 200));
    }
    const TickStore store = fixture::make_store(dir, ticks);

    BacktestConfig cfg = bare_config();
    BacktestEngine engine(store, cfg);
    ScriptedEntry  strat(Side::Long, /*sl*/ 5'000, /*tp*/ 0, 0.10);
    const BacktestResult r = engine.run(strat);

    REQUIRE(r.trades.size() == 1);
    const Trade& t = r.trades[0];
    CHECK_EQ(t.entry_pts, Points{2'650'200});          // ask
    CHECK_EQ(t.sl_pts, Points{2'645'200});             // entry - 5,000
    CHECK_EQ(as_int(t.exit_reason), as_int(ExitReason::StopLoss));
    CHECK_EQ(t.exit_pts, Points{2'640'000});           // where the market was
    CHECK(t.exit_pts < t.sl_pts);                      // worse than the stop
    CHECK_NEAR(t.gross_usd, (2'640'000.0 - 2'650'200.0) * 0.100 * 0.10, 1e-6);
}

XAU_TEST(limits_fill_at_their_price_and_never_slip) {
    fixture::TempDir dir;
    std::vector<Tick> ticks = ramp(16, 2'650'000, 0, 200);
    for (int i = 16; i < 22; ++i) {
        ticks.push_back(tick(kT0 + static_cast<TimeUs>(i) * kMinute, 2'660'000, 200));
    }
    const TickStore store = fixture::make_store(dir, ticks);

    BacktestConfig cfg = bare_config();
    BacktestEngine engine(store, cfg);
    ScriptedEntry  strat(Side::Long, /*sl*/ 0, /*tp*/ 3'000, 0.10);
    const BacktestResult r = engine.run(strat);

    REQUIRE(r.trades.size() == 1);
    const Trade& t = r.trades[0];
    CHECK_EQ(t.tp_pts, Points{2'653'200});
    CHECK_EQ(as_int(t.exit_reason), as_int(ExitReason::TakeProfit));
    // The market gapped to 2,660,000 but a resting limit fills at its own
    // price. Crediting the gap would be inventing profit.
    CHECK_EQ(t.exit_pts, Points{2'653'200});
}

XAU_TEST(slippage_always_works_against_the_trader) {
    fixture::TempDir dir;
    const TickStore store = fixture::make_store(dir, ramp(60, 2'650'000, 0, 200));

    BacktestConfig cfg = bare_config();
    cfg.costs.slip_base_pts = 50.0;

    {
        BacktestEngine e(store, cfg);
        BuyAndHold     s(0.10, Side::Long);
        const auto     r = e.run(s);
        REQUIRE(r.trades.size() == 1);
        // Buying pays ask + slip; selling receives bid - slip.
        CHECK_EQ(r.trades[0].entry_pts, Points{2'650'250});
        CHECK_EQ(r.trades[0].exit_pts, Points{2'649'950});
    }
    {
        BacktestEngine e(store, cfg);
        BuyAndHold     s(0.10, Side::Short);
        const auto     r = e.run(s);
        REQUIRE(r.trades.size() == 1);
        CHECK_EQ(r.trades[0].entry_pts, Points{2'649'950});   // sell bid - slip
        CHECK_EQ(r.trades[0].exit_pts, Points{2'650'250});    // buy ask + slip
    }
}

XAU_TEST(latency_delays_the_fill_to_a_later_tick) {
    fixture::TempDir dir;
    const TickStore store = fixture::make_store(dir, ramp(60));

    BacktestConfig cfg = bare_config();
    cfg.costs.latency_us = 90'000'000;   // 90 s, so the fill skips a tick

    BacktestEngine engine(store, cfg);
    BuyAndHold     strat(0.10, Side::Long);
    const auto     r = engine.run(strat);

    // Bar closes at t0+15m; the order becomes executable at t0+16m30s, so the
    // first tick at or after that is i = 17, not i = 15.
    REQUIRE(r.trades.size() == 1);
    CHECK_EQ(r.trades[0].entry_ts, kT0 + 17 * kMinute);
    CHECK_EQ(r.trades[0].entry_pts, Points{2'651'900});   // bid(17) + spread
}

// ---------------------------------------------------------------------------
// rejections — counted, never silently dropped
// ---------------------------------------------------------------------------

XAU_TEST(stop_inside_the_spread_is_rejected) {
    fixture::TempDir dir;
    const TickStore store = fixture::make_store(dir, ramp(60, 2'650'000, 0, 200));

    BacktestConfig cfg = bare_config();
    BacktestEngine engine(store, cfg);
    // A long fills on the ask and its stop triggers on the bid, so a stop
    // narrower than the spread is taken out the instant it is placed.
    ScriptedEntry strat(Side::Long, /*sl*/ 150, /*tp*/ 0, 0.10);
    const auto    r = engine.run(strat);

    CHECK_EQ(r.trades.size(), std::size_t{0});
    CHECK_EQ(r.stats.rejected_stop_inside_spread, std::uint64_t{1});
    CHECK_EQ(r.stats.signals, std::uint64_t{1});
}

XAU_TEST(stop_closer_than_the_broker_minimum_is_rejected) {
    fixture::TempDir dir;
    const TickStore store = fixture::make_store(dir, ramp(60, 2'650'000, 0, 200));

    BacktestConfig cfg = bare_config();
    cfg.spec.stops_level_pts = 1'000;
    BacktestEngine engine(store, cfg);
    ScriptedEntry  strat(Side::Long, /*sl*/ 500, /*tp*/ 0, 0.10);
    const auto     r = engine.run(strat);

    CHECK_EQ(r.trades.size(), std::size_t{0});
    CHECK_EQ(r.stats.rejected_stop_too_close, std::uint64_t{1});
}

// ---------------------------------------------------------------------------
// sizing
// ---------------------------------------------------------------------------

XAU_TEST(fractional_sizing_risks_the_requested_fraction) {
    const SymbolSpec spec = SymbolSpec::xauusd_default();
    RiskConfig       risk;
    risk.mode = RiskConfig::Mode::FixedFractional;
    risk.risk_fraction = 0.01;

    // 1% of 10,000 = 100 USD at risk over a 2,000 pt stop.
    // 2,000 pts x 0.100 USD/pt/lot = 200 USD per lot -> 0.50 lots.
    CHECK_NEAR(size_position(risk, spec, 10'000.0, 2'000), 0.50, 1e-12);

    // Snapping is downward, never up: 0.333 lots must not become 0.34.
    CHECK_NEAR(size_position(risk, spec, 10'000.0, 3'000), 0.33, 1e-12);

    // Fractional sizing without a stop has no distance to divide by. Refusing
    // is correct; inventing a default stop would size off a number nobody chose.
    CHECK_NEAR(size_position(risk, spec, 10'000.0, 0), 0.0, 1e-12);

    // Below the broker minimum means no trade, not a rounded-up trade.
    CHECK_NEAR(size_position(risk, spec, 10.0, 2'000), 0.0, 1e-12);

    risk.mode = RiskConfig::Mode::FixedLots;
    risk.fixed_lots = 0.07;
    CHECK_NEAR(size_position(risk, spec, 10'000.0, 2'000), 0.07, 1e-12);
}

XAU_TEST(lot_rounding_snaps_down_and_clamps) {
    SymbolSpec spec = SymbolSpec::xauusd_default();
    CHECK_NEAR(spec.round_lots(0.10), 0.10, 1e-12);
    CHECK_NEAR(spec.round_lots(0.199), 0.19, 1e-12);
    CHECK_NEAR(spec.round_lots(0.009), 0.0, 1e-12);     // under volume_min
    CHECK_NEAR(spec.round_lots(1e6), spec.volume_max, 1e-12);
    // 0.01 is not representable in binary; the snap must not drift.
    CHECK_NEAR(spec.round_lots(0.03), 0.03, 1e-12);
    CHECK_NEAR(spec.round_lots(0.07), 0.07, 1e-12);
}

XAU_TEST(pnl_arithmetic_matches_the_contract) {
    const SymbolSpec spec = SymbolSpec::xauusd_default();
    // 100 oz per lot, 1 point = 0.001 USD -> 0.100 USD per point per lot.
    CHECK_NEAR(spec.usd_per_point_per_lot(), 0.100, 1e-12);
    // A 1.00 USD move on 1.00 lot is 100 USD, the familiar figure for gold.
    CHECK_NEAR(spec.pnl_usd(1'000, 1.0), 100.0, 1e-9);
    CHECK_NEAR(spec.pnl_usd(-1'000, 0.5), -50.0, 1e-9);
    CHECK_NEAR(spec.price_usd(2'650'000), 2650.0, 1e-9);
    CHECK_EQ(spec.points_from_usd(2650.0), Points{2'650'000});
}

// ---------------------------------------------------------------------------
// swap
// ---------------------------------------------------------------------------

XAU_TEST(swap_accrues_per_night_and_triples_on_wednesday) {
    fixture::TempDir dir;
    // Hourly ticks for 72 hours from 2020-01-01T00:00Z, which is a Wednesday.
    std::vector<Tick> ticks;
    for (int i = 0; i < 72; ++i) {
        ticks.push_back(tick(kT0 + static_cast<TimeUs>(i) * 3'600'000'000LL, 2'650'000, 200));
    }
    const TickStore store = fixture::make_store(dir, ticks);

    BacktestConfig cfg = bare_config();
    cfg.tf = Timeframe::H1;
    cfg.apply_swap = true;
    cfg.swap_hour_utc = 21;
    cfg.spec.swap_long_pts = -50.0;      // pay 50 pts per night
    cfg.spec.triple_swap_weekday = 3;    // Wednesday

    BacktestEngine engine(store, cfg);
    BuyAndHold     strat(0.10, Side::Long);
    const auto     r = engine.run(strat);

    // Rollovers inside the window: 21:00 on Jan 1 (Wed, triple), Jan 2, Jan 3.
    //   -50 pts x 0.100 USD/pt/lot x 0.10 lots = -0.50 USD per night
    //   Wed x3 + Thu + Fri = -1.50 - 0.50 - 0.50 = -2.50 USD
    REQUIRE(r.trades.size() == 1);
    CHECK_EQ(r.stats.swap_charges, std::uint64_t{3});
    CHECK_NEAR(r.trades[0].swap_usd, -2.50, 1e-9);
    CHECK_NEAR(r.trades[0].net_usd, r.trades[0].gross_usd - 2.50, 1e-9);
}

// ---------------------------------------------------------------------------
// determinism
// ---------------------------------------------------------------------------

XAU_TEST(identical_runs_produce_identical_trades) {
    fixture::TempDir dir;
    // A wandering price so the run is not trivially empty.
    std::vector<Tick> ticks;
    Points            bid = 2'650'000;
    std::uint64_t     s = 99;
    for (int i = 0; i < 20'000; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        bid += static_cast<Points>((s >> 60) % 7) - 3;
        ticks.push_back(tick(kT0 + static_cast<TimeUs>(i) * kMinute, bid, 200));
    }
    const TickStore store = fixture::make_store(dir, ticks);

    BacktestConfig cfg = bare_config();
    cfg.costs.slip_base_pts = 20.0;

    auto run_once = [&] {
        RandomEntry::Config rc;
        rc.entry_prob = 0.05;
        rc.hold_bars = 6;
        rc.lots = 0.10;
        RandomEntry strat(rc);
        return BacktestEngine(store, cfg).run(strat);
    };

    const BacktestResult a = run_once();
    const BacktestResult b = run_once();

    REQUIRE(a.trades.size() == b.trades.size());
    CHECK(a.trades.size() > 20);

    bool identical = true;
    for (std::size_t i = 0; i < a.trades.size(); ++i) {
        const Trade& x = a.trades[i];
        const Trade& y = b.trades[i];
        if (x.entry_ts != y.entry_ts || x.exit_ts != y.exit_ts ||
            x.entry_pts != y.entry_pts || x.exit_pts != y.exit_pts || x.side != y.side ||
            x.net_usd != y.net_usd) {
            identical = false;
        }
    }
    CHECK(identical);
    CHECK_EQ(a.final_balance, b.final_balance);
}

XAU_TEST(strategy_never_sees_the_forming_bar) {
    fixture::TempDir dir;
    const TickStore store = fixture::make_store(dir, ramp(120));

    // Records the newest bar it is shown on each call. Every one of them must
    // already be complete: its close time can never be in the future relative
    // to the moment the strategy was asked.
    class Peeker final : public Strategy {
    public:
        [[nodiscard]] const char* name() const noexcept override { return "Peeker"; }
        [[nodiscard]] Decision on_bar(const BarContext& c) override {
            ++calls;
            if (c.bar().close_time_us(c.tf) > c.now_us) leaked = true;
            // The bar handed over must be the one that just closed.
            if (c.bar().close_time_us(c.tf) != c.now_us) mismatched = true;
            return Decision::hold();
        }
        int  calls = 0;
        bool leaked = false;
        bool mismatched = false;
    } peeker;

    const auto r = BacktestEngine(store, bare_config()).run(peeker);
    CHECK(peeker.calls > 0);
    CHECK(!peeker.leaked);
    CHECK(!peeker.mismatched);
    CHECK_EQ(static_cast<std::uint64_t>(peeker.calls), r.stats.bars);
}
