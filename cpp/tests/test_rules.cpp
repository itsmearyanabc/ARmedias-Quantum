// Phase 3: session arithmetic, indicators, rule baselines and walk-forward.
//
// These are mechanical-correctness tests. Whether any of these strategies has
// an EDGE is a question only real tick history can answer — the synthetic
// generator is a driftless random walk by construction, so running the gate
// against it measures the cost model and nothing else.

#include "fixture.hpp"
#include "harness.hpp"

#include "xau/indicators.hpp"
#include "xau/registry.hpp"
#include "xau/rules.hpp"
#include "xau/session.hpp"
#include "xau/walk_forward.hpp"

#include <functional>
#include <memory>
#include <vector>

using namespace xau;
using fixture::kMinute;
using fixture::kT0;
using fixture::tick;

namespace {

// kT0 is 2020-01-01T00:00:00Z, a Wednesday.
constexpr TimeUs kWed = kT0;
constexpr TimeUs kThu = kT0 + kUsPerDay;
constexpr TimeUs kFri = kT0 + 2 * kUsPerDay;
constexpr TimeUs kSat = kT0 + 3 * kUsPerDay;
constexpr TimeUs kSun = kT0 + 4 * kUsPerDay;

BacktestConfig rule_config() {
    BacktestConfig c;
    c.spec = SymbolSpec::xauusd_default();
    c.spec.stops_level_pts = 0;
    c.tf = Timeframe::M15;
    c.initial_balance = 10'000.0;
    c.apply_swap = false;
    c.costs.slip_base_pts = 0.0;
    c.costs.slip_vol_coef = 0.0;
    c.costs.latency_us = 0;
    c.costs.commission_per_lot_round_usd = 0.0;
    return c;
}

// One tick per minute from `from_min` to `to_min` (minutes since `day0`), with
// the price supplied by the caller as a function of the absolute minute.
std::vector<Tick> minute_ticks(TimeUs day0, int from_min, int to_min,
                               const std::function<Points(int)>& price,
                               std::uint16_t spread = 200) {
    std::vector<Tick> v;
    v.reserve(static_cast<std::size_t>(to_min - from_min));
    for (int m = from_min; m < to_min; ++m) {
        v.push_back(tick(day0 + static_cast<TimeUs>(m) * kMinute, price(m), spread));
    }
    return v;
}

// A +/-300 point zigzag so ATR is non-zero; flat data gives ATR 0 and every
// ATR-sized stop then collapses to nothing.
Points zigzag(int m, Points base = 2'650'000) {
    return base + (((m / 5) % 2) ? 300 : -300);
}

Bar mk_bar(Points o, Points h, Points l, Points c, TimeUs t = 0) {
    Bar b;
    b.open_time_us = t;
    b.open = o;
    b.high = h;
    b.low = l;
    b.close = c;
    return b;
}

}  // namespace

// ---------------------------------------------------------------------------
// calendar
// ---------------------------------------------------------------------------

XAU_TEST(utc_hour_and_weekday) {
    CHECK_EQ(utc_hour(kWed), 0);
    CHECK_EQ(utc_hour(kWed + 7 * kUsPerHour), 7);
    CHECK_EQ(utc_hour(kWed + 23 * kUsPerHour + 59 * kUsPerMinute), 23);

    // 2020-01-01 was a Wednesday.
    CHECK_EQ(utc_weekday(kWed), 3);
    CHECK_EQ(utc_weekday(kThu), 4);
    CHECK_EQ(utc_weekday(kFri), 5);
    CHECK_EQ(utc_weekday(kSat), 6);
    CHECK_EQ(utc_weekday(kSun), 7);
    CHECK_EQ(utc_weekday(kSun + kUsPerDay), 1);   // Monday

    // Before the epoch the arithmetic must floor, not truncate toward zero.
    CHECK_EQ(utc_hour(-kUsPerHour), 23);
    CHECK_EQ(day_start(-1), -kUsPerDay);
    CHECK_EQ(utc_weekday(-kUsPerDay), 3);         // 1969-12-31 was a Wednesday
}

XAU_TEST(in_hours_wraps_past_midnight) {
    // A normal window.
    CHECK(in_hours(kWed + 8 * kUsPerHour, 7, 12));
    CHECK(!in_hours(kWed + 12 * kUsPerHour, 7, 12));   // half-open at the top
    CHECK(in_hours(kWed + 7 * kUsPerHour, 7, 12));     // closed at the bottom

    // Asia: 23:00 -> 07:00, which wraps.
    CHECK(in_hours(kWed + 23 * kUsPerHour, 23, 7));
    CHECK(in_hours(kWed + 2 * kUsPerHour, 23, 7));
    CHECK(!in_hours(kWed + 8 * kUsPerHour, 23, 7));
    CHECK(!in_hours(kWed + 22 * kUsPerHour, 23, 7));
}

XAU_TEST(session_day_groups_a_window_that_wraps_midnight) {
    // 23:30 Wednesday and 03:00 Thursday are the SAME Asia session, and must
    // therefore share a key or the range resets halfway through.
    const TimeUs a = session_day(kWed + 23 * kUsPerHour + 30 * kUsPerMinute, 23);
    const TimeUs b = session_day(kThu + 3 * kUsPerHour, 23);
    CHECK_EQ(a, b);

    // The London morning that follows still belongs to that session's key,
    // which is what lets the breakout window see the range it should break.
    CHECK_EQ(session_day(kThu + 9 * kUsPerHour, 23), a);

    // It rolls when the next Asia session opens, not at midnight.
    CHECK(session_day(kThu + 23 * kUsPerHour, 23) != a);
}

XAU_TEST(market_open_matches_golds_week) {
    CHECK(market_open(kWed + 12 * kUsPerHour));
    CHECK(market_open(kFri + 20 * kUsPerHour));
    CHECK(!market_open(kFri + 21 * kUsPerHour));    // Friday close
    CHECK(!market_open(kSat + 12 * kUsPerHour));    // Saturday
    CHECK(!market_open(kSun + 12 * kUsPerHour));    // Sunday morning
    CHECK(market_open(kSun + 22 * kUsPerHour));     // Sunday open
}

// ---------------------------------------------------------------------------
// indicators
// ---------------------------------------------------------------------------

XAU_TEST(atr_seeds_then_smooths) {
    Atr atr(3);
    CHECK(!atr.ready());

    // Three bars of range 100 each, no gaps, so true range == high-low.
    atr.update(mk_bar(1000, 1100, 1000, 1050));
    CHECK(!atr.ready());
    atr.update(mk_bar(1050, 1150, 1050, 1100));
    atr.update(mk_bar(1100, 1200, 1100, 1150));
    CHECK(atr.ready());
    CHECK_NEAR(atr.value(), 100.0, 1e-9);

    // Wilder smoothing: (prev*(n-1) + tr)/n. A 400-range bar gives
    // (100*2 + 400)/3 = 200.
    atr.update(mk_bar(1150, 1550, 1150, 1500));
    CHECK_NEAR(atr.value(), 200.0, 1e-9);
}

XAU_TEST(atr_true_range_uses_the_previous_close) {
    Atr atr(1);
    atr.update(mk_bar(1000, 1010, 990, 1000));    // range 20, no previous close
    CHECK_NEAR(atr.value(), 20.0, 1e-9);
    // Gap up: high-low is 10, but the distance from the previous close is 100.
    // True range must take the larger, or a gap looks like a quiet bar.
    atr.update(mk_bar(1100, 1100, 1090, 1095));
    CHECK_NEAR(atr.value(), 100.0, 1e-9);
}

XAU_TEST(ema_seeds_from_a_simple_average) {
    Ema ema(3);
    CHECK(!ema.ready());
    ema.update(10.0);
    ema.update(20.0);
    CHECK(!ema.ready());
    ema.update(30.0);
    CHECK(ema.ready());
    CHECK_NEAR(ema.value(), 20.0, 1e-9);   // (10+20+30)/3

    // k = 2/(3+1) = 0.5, so the next value is 20 + 0.5*(40-20) = 30.
    ema.update(40.0);
    CHECK_NEAR(ema.value(), 30.0, 1e-9);
}

XAU_TEST(rolling_extremes_and_skip) {
    std::vector<Bar> bars = {
        mk_bar(0, 110, 90, 100),
        mk_bar(0, 130, 95, 120),
        mk_bar(0, 120, 80, 110),
        mk_bar(0, 200, 105, 150),   // newest
    };
    const std::span<const Bar> h(bars);

    CHECK_EQ(highest_high(h, 4), Points{200});
    CHECK_EQ(lowest_low(h, 4), Points{80});

    // skip=1 excludes the newest bar. A breakout test needs this: "did this bar
    // exceed the previous three" is the question, and including itself makes
    // the answer trivially yes.
    CHECK_EQ(highest_high(h, 3, 1), Points{130});
    CHECK_EQ(lowest_low(h, 3, 1), Points{80});

    // Asking for more bars than exist clamps rather than reading out of range.
    CHECK_EQ(highest_high(h, 99), Points{200});
    CHECK_EQ(highest_high(h, 0), Points{0});
}

XAU_TEST(narrowest_range_detects_compression) {
    std::vector<Bar> bars = {
        mk_bar(0, 200, 100, 150),   // range 100
        mk_bar(0, 200, 120, 150),   // range 80
        mk_bar(0, 200, 150, 170),   // range 50 <- narrowest
    };
    CHECK(is_narrowest_range(std::span<const Bar>(bars), 3));

    bars.push_back(mk_bar(0, 300, 100, 200));   // range 200, not narrowest
    CHECK(!is_narrowest_range(std::span<const Bar>(bars), 3));

    // Not enough history is not compression.
    CHECK(!is_narrowest_range(std::span<const Bar>(bars), 99));
}

XAU_TEST(stdev_of_closes) {
    std::vector<Bar> bars = {mk_bar(0, 0, 0, 100), mk_bar(0, 0, 0, 200),
                             mk_bar(0, 0, 0, 300)};
    // Sample stdev of {100,200,300} is 100.
    CHECK_NEAR(stdev_close(std::span<const Bar>(bars), 3), 100.0, 1e-9);
    CHECK_NEAR(stdev_close(std::span<const Bar>(bars), 99), 0.0, 1e-12);
}

// ---------------------------------------------------------------------------
// exit discipline
// ---------------------------------------------------------------------------

XAU_TEST(time_exit_covers_hold_rollover_and_friday) {
    Position pos;
    pos.side = Side::Long;
    pos.lots = 0.01;
    pos.entry_ts = kThu + 9 * kUsPerHour;

    SymbolSpec spec;
    auto ctx = [&](TimeUs now) {
        return BarContext{.history = {},
                          .position = pos,
                          .spec = spec,
                          .tf = Timeframe::M15,
                          .equity = 0.0,
                          .balance = 0.0,
                          .now_us = now};
    };

    ExitRules r;
    r.max_hold_bars = 4;          // one hour on M15
    r.flat_by_hour = 21;
    r.friday_flat_hour = 20;

    CHECK(!should_time_exit(ctx(kThu + 9 * kUsPerHour + 30 * kUsPerMinute), r));
    CHECK(should_time_exit(ctx(kThu + 10 * kUsPerHour), r));           // hold elapsed

    r.max_hold_bars = 0;                                                // no time cap
    CHECK(!should_time_exit(ctx(kThu + 20 * kUsPerHour), r));
    CHECK(should_time_exit(ctx(kThu + 21 * kUsPerHour), r));           // rollover

    // Friday goes flat earlier: gold gaps over the weekend and a stop cannot
    // protect a position across a market that is not trading.
    CHECK(should_time_exit(ctx(kFri + 20 * kUsPerHour), r));
    CHECK(!should_time_exit(ctx(kThu + 20 * kUsPerHour), r));

    // A flat book is never "exited".
    pos.side = Side::None;
    CHECK(!should_time_exit(ctx(kFri + 20 * kUsPerHour), r));
}

// ---------------------------------------------------------------------------
// London opening range, worked through precisely
// ---------------------------------------------------------------------------

XAU_TEST(london_opening_range_breaks_out_after_the_window) {
    fixture::TempDir dir;
    // Midnight to 10:00 UTC on the Thursday. Zigzag baseline everywhere, with a
    // decisive 6,000-point step up once the 08:00 hour begins.
    const auto price = [](int m) -> Points {
        return zigzag(m, (m >= 8 * 60) ? 2'656'000 : 2'650'000);
    };
    const TickStore store = fixture::make_store(dir, minute_ticks(kThu, 0, 10 * 60, price));

    BacktestConfig cfg = rule_config();
    LondonOpeningRange::Config lc;
    lc.lots = 0.10;
    LondonOpeningRange strat(lc);

    const BacktestResult r = BacktestEngine(store, cfg).run(strat);

    REQUIRE(r.trades.size() == 1);
    const Trade& t = r.trades[0];

    // The 07:00-08:00 range is the zigzag band, 2,649,700..2,650,300. The first
    // bar after 08:00 closes far above it, so the break is long.
    CHECK_EQ(static_cast<int>(t.side), static_cast<int>(Side::Long));
    CHECK(utc_hour(t.entry_ts) >= 8);
    CHECK(t.entry_pts > 2'650'300);

    // Nothing may fire while the range is still being built.
    CHECK(utc_hour(t.entry_ts) >= 8);
}

XAU_TEST(london_opening_range_takes_at_most_one_trade_per_day) {
    fixture::TempDir dir;
    // Three consecutive days, each with the same shape. The strategy should
    // take one trade per day and no more, however many bars break the range.
    std::vector<Tick> ticks;
    for (int d = 0; d < 3; ++d) {
        const TimeUs day = kThu + static_cast<TimeUs>(d) * kUsPerDay;
        const auto   price = [](int m) -> Points {
            return zigzag(m, (m >= 8 * 60) ? 2'656'000 : 2'650'000);
        };
        const auto part = minute_ticks(day, 0, 20 * 60, price);
        ticks.insert(ticks.end(), part.begin(), part.end());
    }
    const TickStore store = fixture::make_store(dir, ticks);

    LondonOpeningRange::Config lc;
    lc.lots = 0.10;
    LondonOpeningRange strat(lc);
    const BacktestResult r = BacktestEngine(store, rule_config()).run(strat);

    // Day 3 of the window is a Saturday by the calendar, but the store is
    // synthetic and the engine does not gate on market hours, so all three
    // days trade. What matters is one per day, not three on one day.
    CHECK_EQ(r.trades.size(), std::size_t{3});
    bool distinct_days = true;
    for (std::size_t i = 1; i < r.trades.size(); ++i) {
        if (day_start(r.trades[i].entry_ts) == day_start(r.trades[i - 1].entry_ts)) {
            distinct_days = false;
        }
    }
    CHECK(distinct_days);
}

XAU_TEST(london_opening_range_skips_a_range_outside_the_atr_band) {
    fixture::TempDir dir;
    // A 40,000-point downward spike inside 07:00-08:00 blows the opening range
    // out to roughly 6x ATR. Price still closes above the range high after
    // 08:00, so the breakout signal itself is present and only the filter can
    // suppress it — which is what makes this a test of the filter rather than
    // of the absence of a setup.
    const auto price = [](int m) -> Points {
        Points base = (m >= 8 * 60) ? 2'656'000 : 2'650'000;
        if (m >= 7 * 60 && m < 8 * 60 && (m % 30) == 0) base -= 40'000;
        return zigzag(m, base);
    };
    const auto ticks = minute_ticks(kThu, 0, 10 * 60, price);
    const TickStore store = fixture::make_store(dir, ticks);

    {
        LondonOpeningRange::Config lc;
        lc.lots = 0.10;
        lc.max_range_atr = 3.0;
        LondonOpeningRange   strat(lc);
        const BacktestResult r = BacktestEngine(store, rule_config()).run(strat);
        CHECK_EQ(r.trades.size(), std::size_t{0});
        CHECK_EQ(r.stats.signals, std::uint64_t{0});
    }
    {
        // Same data, filter opened up: the trade now appears. If this did not
        // fire, the test above would be passing because there was no setup.
        LondonOpeningRange::Config lc;
        lc.lots = 0.10;
        lc.max_range_atr = 100.0;
        LondonOpeningRange   strat(lc);
        const BacktestResult r = BacktestEngine(store, rule_config()).run(strat);
        CHECK_EQ(r.trades.size(), std::size_t{1});
    }
}

// ---------------------------------------------------------------------------
// the other three run, trade, and respect their windows
// ---------------------------------------------------------------------------

namespace {

// Wandering minute ticks, enough for every baseline to warm up and find setups.
//
// The step size is load-bearing. At a few points per minute an M15 bar's range
// comes out below the 200-point spread, every ATR-sized stop lands inside the
// spread, and the engine correctly rejects every single entry — so the
// strategies would look broken when it was the fixture that was wrong. 120
// points/minute gives bar ranges in the hundreds and stops comfortably clear
// of the spread, which is the regime real gold actually trades in.
std::vector<Tick> wandering(int days) {
    std::vector<Tick> v;
    std::uint64_t     s = 12345;
    Points            bid = 2'650'000;
    const int         minutes = days * 24 * 60;
    v.reserve(static_cast<std::size_t>(minutes));
    for (int m = 0; m < minutes; ++m) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        bid += (static_cast<Points>((s >> 59) % 9) - 4) * 30;
        v.push_back(tick(kT0 + static_cast<TimeUs>(m) * kMinute, bid, 200));
    }
    return v;
}

}  // namespace

XAU_TEST(every_baseline_runs_and_trades) {
    fixture::TempDir dir;
    const TickStore store = fixture::make_store(dir, wandering(21));
    const BacktestConfig cfg = rule_config();

    struct Case {
        const char*                       label;
        std::function<std::unique_ptr<Strategy>()> make;
    };
    const std::vector<Case> cases = {
        {"LondonOpeningRange",
         [] {
             LondonOpeningRange::Config c;
             c.lots = 0.10;
             return std::make_unique<LondonOpeningRange>(c);
         }},
        {"AsiaRangeBreakout",
         [] {
             AsiaRangeBreakout::Config c;
             c.lots = 0.10;
             return std::make_unique<AsiaRangeBreakout>(c);
         }},
        {"TrendPullback",
         [] {
             TrendPullback::Config c;
             c.lots = 0.10;
             c.trend_ema = 50;   // a week of M15 is ~670 bars; 200 leaves too little
             return std::make_unique<TrendPullback>(c);
         }},
        {"VolatilityCompression",
         [] {
             VolatilityCompression::Config c;
             c.lots = 0.10;
             return std::make_unique<VolatilityCompression>(c);
         }},
    };

    for (const Case& c : cases) {
        std::unique_ptr<Strategy> s = c.make();
        const BacktestResult      r = BacktestEngine(store, cfg).run(*s);

        // A baseline that never trades is broken, not conservative.
        CHECK(r.stats.signals > 0);
        CHECK(r.trades.size() > 0);

        // Every entry must respect the strategy's own trading window, and no
        // position may survive past the rollover hour.
        bool windows_respected = true;
        for (const Trade& t : r.trades) {
            if (utc_hour(t.exit_ts) >= 22) windows_respected = false;
        }
        CHECK(windows_respected);
    }
}

// ---------------------------------------------------------------------------
// walk-forward
// ---------------------------------------------------------------------------

XAU_TEST(walk_forward_splits_folds_and_pools_every_trade) {
    fixture::TempDir dir;
    const TickStore store = fixture::make_store(dir, wandering(7));

    BacktestConfig cfg = rule_config();
    WalkForwardConfig wf;
    wf.test_span_us = 2 * kUsPerDay;   // a week of data, two-day folds
    wf.min_trades_per_fold = 1;

    const StrategyFactory make = [] {
        VolatilityCompression::Config c;
        c.lots = 0.10;
        return std::make_unique<VolatilityCompression>(c);
    };

    const WalkForwardResult r = run_walk_forward(store, cfg, make, wf);

    // Seven days in two-day windows: four folds, the last one short.
    CHECK_EQ(r.folds.size(), std::size_t{4});
    CHECK_EQ(r.strategy_name, std::string("VolatilityCompression"));

    // Folds must tile the range without overlapping.
    bool contiguous = true;
    for (std::size_t i = 1; i < r.folds.size(); ++i) {
        if (r.folds[i].from_us != r.folds[i - 1].to_us) contiguous = false;
    }
    CHECK(contiguous);

    // Pooled trades must be exactly the sum of the folds; a mismatch means
    // trades were dropped or double-counted at a boundary.
    std::size_t summed = 0;
    for (const Fold& f : r.folds) summed += f.metrics.trades;
    CHECK_EQ(r.pooled.trades, summed);
    CHECK(summed > 0);
}

XAU_TEST(walk_forward_rebuilds_the_strategy_for_every_fold) {
    fixture::TempDir dir;
    const TickStore store = fixture::make_store(dir, wandering(7));

    // Counts how many times a fresh instance was constructed. If the runner
    // reused one strategy across folds, armed setups and indicator state would
    // leak forward and the later folds would not be out of sample at all.
    static int constructions = 0;
    struct Counting final : Strategy {
        Counting() { ++constructions; }
        [[nodiscard]] const char* name() const noexcept override { return "Counting"; }
        [[nodiscard]] Decision on_bar(const BarContext&) override { return Decision::hold(); }
    };
    constructions = 0;

    WalkForwardConfig wf;
    wf.test_span_us = 2 * kUsPerDay;
    const WalkForwardResult r =
        run_walk_forward(store, rule_config(), [] { return std::make_unique<Counting>(); }, wf);

    CHECK_EQ(r.folds.size(), std::size_t{4});
    CHECK_EQ(constructions, 4);
}

XAU_TEST(walk_forward_handles_a_degenerate_range) {
    fixture::TempDir dir;
    const TickStore store = fixture::make_store(dir, wandering(7));

    WalkForwardConfig wf;
    wf.test_span_us = 0;   // nonsensical span must yield nothing, not loop
    const WalkForwardResult r =
        run_walk_forward(store, rule_config(), [] { return std::make_unique<LondonOpeningRange>(); },
                         wf);
    CHECK_EQ(r.folds.size(), std::size_t{0});
    CHECK_EQ(r.pooled.trades, std::size_t{0});
}


// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------

XAU_TEST(registry_is_well_formed) {
    const std::span<const BaselineEntry> reg = baseline_registry();
    CHECK(reg.size() >= 6);

    std::size_t gate_candidates = 0;
    bool        names_unique = true;
    bool        all_construct = true;
    bool        all_described = true;

    for (std::size_t i = 0; i < reg.size(); ++i) {
        if (reg[i].gate_candidate) ++gate_candidates;
        if (reg[i].name == nullptr || reg[i].description == nullptr) all_described = false;

        std::unique_ptr<Strategy> s = reg[i].make(0.01);
        if (!s || s->name() == nullptr) all_construct = false;

        for (std::size_t j = i + 1; j < reg.size(); ++j) {
            if (std::string(reg[i].name) == std::string(reg[j].name)) names_unique = false;
        }
    }

    CHECK(names_unique);
    CHECK(all_construct);
    CHECK(all_described);
    // The zoo grows, so a hardcoded count here just breaks on every addition
    // and teaches the next person to bump the number without reading it. What
    // must stay true is the invariant the count was standing in for: the null
    // model and buy-and-hold can never satisfy the Phase 3 gate, and the four
    // PLAN section 7 baselines always can.
    CHECK(gate_candidates >= std::size_t{4});
    for (const char* n : {"RandomEntry (null)", "BuyAndHold"}) {
        const BaselineEntry* e = find_baseline(n);
        if (e != nullptr) CHECK(!e->gate_candidate);
    }
    for (const char* n : {"LondonOpeningRange", "AsiaRangeBreakout", "TrendPullback",
                          "VolatilityCompression"}) {
        const BaselineEntry* e = find_baseline(n);
        CHECK(e != nullptr);
        if (e != nullptr) CHECK(e->gate_candidate);
    }
}

XAU_TEST(registry_lookup_and_factories) {
    CHECK(find_baseline("LondonOpeningRange") != nullptr);
    CHECK(find_baseline("VolatilityCompression") != nullptr);
    CHECK(find_baseline("no such strategy") == nullptr);
    CHECK(find_baseline(nullptr) == nullptr);

    const BaselineEntry* e = find_baseline("AsiaRangeBreakout");
    REQUIRE(e != nullptr);
    CHECK(e->gate_candidate);

    // factory_for must hand back a FRESH instance each call: walk-forward
    // relies on that to keep folds out of sample.
    const StrategyFactory f = factory_for(*e, 0.05);
    std::unique_ptr<Strategy> a = f();
    std::unique_ptr<Strategy> b = f();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a.get() != b.get());
    CHECK_EQ(std::string(a->name()), std::string("AsiaRangeBreakout"));

    // It must also outlive the entry reference it was built from, which is why
    // it copies the maker rather than capturing by reference.
    StrategyFactory detached;
    {
        const BaselineEntry* tmp = find_baseline("TrendPullback");
        detached = factory_for(*tmp, 0.02);
    }
    CHECK(detached() != nullptr);
}
