// Round B hypotheses (docs/RESEARCH-B.md): mechanical correctness only.
//
// Whether any of these has an edge is for real history to say. What these
// pin is that each rule does what its pre-registration says, never looks
// forward, and that the holdout warm-up cannot leak a trade into the window
// it is meant to exclude.

#include "fixture.hpp"
#include "harness.hpp"

#include "xau/engine.hpp"
#include "xau/session.hpp"
#include "xau/zoo.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

using namespace xau;
using fixture::kT0;
using fixture::tick;

namespace {

constexpr TimeUs kHour = kUsPerHour;

// One tick an hour while gold trades, for `days` days from kT0, priced by the
// caller as a function of the hour index. A +/-300 point alternation keeps ATR
// away from zero.
std::vector<Tick> hourly(int days, const std::function<Points(int)>& base) {
    std::vector<Tick> v;
    for (int i = 0; i < days * 24; ++i) {
        const TimeUs ts = kT0 + static_cast<TimeUs>(i) * kHour;
        if (!market_open(ts)) continue;
        v.push_back(tick(ts, base(i) + ((i % 2) ? 300 : -300), 200));
    }
    return v;
}

BacktestConfig cfg_for(Timeframe tf) {
    BacktestConfig c;
    c.spec = SymbolSpec::xauusd_default();
    c.spec.stops_level_pts = 0;
    c.tf = tf;
    c.apply_swap = false;
    c.costs.slip_base_pts = 0.0;
    c.costs.slip_vol_coef = 0.0;
    c.costs.latency_us = 0;
    c.costs.commission_per_lot_round_usd = 0.0;
    return c;
}

// Up 50 points an hour for `peak` days, then down at the same rate.
std::function<Points(int)> tent(int peak_days) {
    return [peak_days](int i) -> Points {
        const int peak = peak_days * 24;
        const int k = i <= peak ? i : 2 * peak - i;
        return 2'000'000 + 50 * static_cast<Points>(k);
    };
}

// Trades taken on `a` and on `b`, which agree up to `cut` and differ after,
// must agree on every trade ENTERED before `cut`. A rule that peeks forward
// shows up here as an early trade that moves when only the future changed.
void check_causal(const std::function<std::unique_ptr<Strategy>()>& make, Timeframe tf,
                  const std::vector<Tick>& a, const std::vector<Tick>& b, TimeUs cut) {
    fixture::TempDir da, db;
    const TickStore  sa = fixture::make_store(da, a);
    const TickStore  sb = fixture::make_store(db, b);
    auto             s1 = make();
    auto             s2 = make();
    const auto       ra = BacktestEngine(sa, cfg_for(tf)).run(*s1);
    const auto       rb = BacktestEngine(sb, cfg_for(tf)).run(*s2);

    std::vector<const Trade*> ea, eb;
    for (const Trade& t : ra.trades) if (t.entry_ts < cut) ea.push_back(&t);
    for (const Trade& t : rb.trades) if (t.entry_ts < cut) eb.push_back(&t);
    REQUIRE(!ea.empty());
    REQUIRE(ea.size() == eb.size());
    for (std::size_t i = 0; i < ea.size(); ++i) {
        CHECK_EQ(ea[i]->entry_ts, eb[i]->entry_ts);
        CHECK_EQ(static_cast<int>(ea[i]->side), static_cast<int>(eb[i]->side));
        CHECK_EQ(ea[i]->entry_pts, eb[i]->entry_pts);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 14. Time-series momentum
// ---------------------------------------------------------------------------

XAU_TEST(tsmom_waits_a_year_then_follows_the_year) {
    fixture::TempDir dir;
    const TickStore  store = fixture::make_store(dir, hourly(900, tent(450)));
    TimeSeriesMomentum strat;
    const auto r = BacktestEngine(store, cfg_for(Timeframe::D1)).run(strat);

    REQUIRE(!r.trades.empty());
    // Nothing until a full year of closes exists to compare against.
    CHECK(r.trades.front().entry_ts >= kT0 + 365 * kUsPerDay);
    CHECK_EQ(static_cast<int>(r.trades.front().side), static_cast<int>(Side::Long));

    // The tent peaks at day 450. The trailing year turns negative once the
    // decline has undone the rise it is compared with: 450 - x < 85 + x, so
    // x > 182.5 days. No short may come before that, no long after it.
    const TimeUs flip = kT0 + static_cast<TimeUs>((450 + 182.5) * 86400.0) * 1'000'000;
    bool saw_short = false, order_ok = true;
    for (const Trade& t : r.trades) {
        if (t.side == Side::Short) {
            saw_short = true;
            if (t.entry_ts < flip) order_ok = false;
        } else if (t.entry_ts > flip + 29 * kUsPerDay) {
            order_ok = false;   // a review is at most four weeks away
        }
    }
    CHECK(saw_short);
    CHECK(order_ok);
}

XAU_TEST(tsmom_is_causal) {
    const auto a = hourly(700, tent(450));
    auto       b = hourly(700, tent(450));
    const TimeUs cut = kT0 + 500 * kUsPerDay;
    for (Tick& t : b) if (t.ts_us >= cut) t.bid_pts -= 400'000;   // a crash after the cut
    check_causal([] { return std::make_unique<TimeSeriesMomentum>(); }, Timeframe::D1, a, b,
                 cut);
}

// ---------------------------------------------------------------------------
// the holdout warm-up
// ---------------------------------------------------------------------------

XAU_TEST(trade_from_warms_the_strategy_but_takes_no_earlier_trade) {
    fixture::TempDir dir;
    const TickStore  store = fixture::make_store(dir, hourly(600, tent(600)));

    BacktestConfig cfg = cfg_for(Timeframe::D1);
    cfg.trade_from_us = kT0 + 450 * kUsPerDay;
    TimeSeriesMomentum strat;
    const auto r = BacktestEngine(store, cfg).run(strat);

    REQUIRE(!r.trades.empty());
    for (const Trade& t : r.trades) CHECK(t.entry_ts >= cfg.trade_from_us);
    CHECK(r.stats.entries_before_trade_from > 0);
    // Warm on arrival: the first trade is the first bar after the boundary,
    // not a year later as a cold start would make it.
    CHECK(r.trades.front().entry_ts < cfg.trade_from_us + 2 * kUsPerDay);
    for (const EquityPoint& e : r.equity) CHECK(e.ts_us >= cfg.trade_from_us);
}

// ---------------------------------------------------------------------------
// 15. Donchian
// ---------------------------------------------------------------------------

XAU_TEST(donchian_enters_on_the_break_and_exits_on_the_opposite_channel) {
    fixture::TempDir dir;
    // Flat for 120 days, a 20,000-point step up, flat for 20 days, then a
    // 40,000-point step down through everything.
    const auto price = [](int i) -> Points {
        const int d = i / 24;
        if (d < 120) return 2'000'000;
        if (d < 140) return 2'020'000;
        return 1'980'000;
    };
    const TickStore store = fixture::make_store(dir, hourly(200, price));
    DonchianTrend   strat;
    const auto r = BacktestEngine(store, cfg_for(Timeframe::D1)).run(strat);

    REQUIRE(!r.trades.empty());
    const Trade& t = r.trades.front();
    CHECK_EQ(static_cast<int>(t.side), static_cast<int>(Side::Long));
    CHECK(t.entry_ts >= kT0 + 120 * kUsPerDay);
    CHECK(t.entry_ts <= kT0 + 123 * kUsPerDay);
    // The step down is twice the stop distance, so the stop takes it at once;
    // either way it must be out by the first bar after the drop.
    CHECK(t.exit_ts <= kT0 + 142 * kUsPerDay);
    // Nothing before the 77-day channel exists.
    for (const Trade& x : r.trades) CHECK(x.entry_ts >= kT0 + 77 * kUsPerDay);
}

XAU_TEST(donchian_is_causal) {
    const auto price = [](int i) -> Points {
        return 2'000'000 + 40 * static_cast<Points>(i % (24 * 160));   // ramps and resets
    };
    const auto a = hourly(500, price);
    auto       b = hourly(500, price);
    const TimeUs cut = kT0 + 300 * kUsPerDay;
    for (Tick& t : b) if (t.ts_us >= cut) t.bid_pts += 300'000;
    check_causal([] { return std::make_unique<DonchianTrend>(); }, Timeframe::D1, a, b, cut);
}

// ---------------------------------------------------------------------------
// 16. Asia drift
// ---------------------------------------------------------------------------

XAU_TEST(asia_drift_holds_23_to_07_on_weeknights_and_pays_no_swap) {
    fixture::TempDir dir;
    const TickStore  store =
        fixture::make_store(dir, hourly(28, [](int) -> Points { return 2'000'000; }));

    BacktestConfig cfg = cfg_for(Timeframe::H1);
    cfg.apply_swap = true;
    cfg.spec.swap_long_annual = -0.05;
    cfg.spec.swap_long_pts = -50.0;
    AsiaDrift  strat;
    const auto r = BacktestEngine(store, cfg).run(strat);

    // 28 days from a Wednesday: four weeks of Sun..Thu nights is 20, less the
    // first night, which the 24-bar ATR is still warming through.
    CHECK(r.trades.size() >= std::size_t{18});
    CHECK(r.trades.size() <= std::size_t{20});
    for (const Trade& t : r.trades) {
        if (t.exit_reason == ExitReason::EndOfData) continue;   // the last night
        CHECK_EQ(static_cast<int>(t.side), static_cast<int>(Side::Long));
        CHECK(utc_hour(t.entry_ts) == 23 || utc_hour(t.entry_ts) == 0);
        CHECK_EQ(utc_hour(t.exit_ts), 7);
        CHECK(utc_weekday(t.exit_ts) <= 5);
        CHECK(t.exit_ts - t.entry_ts <= 8 * kHour);
    }
    CHECK_EQ(r.stats.swap_charges, std::uint64_t{0});
    CHECK_NEAR(r.metrics.total_swap, 0.0, 1e-12);
}

XAU_TEST(asia_drift_enters_once_a_night_even_after_a_stop) {
    fixture::TempDir dir;
    // Quiet, then a 30,000-point drop at 23:30 on one night: far through a
    // 3-ATR stop. Before the 00:00 bar the strategy is flat again, and must
    // NOT take a second trade that night.
    const auto price = [](int i) -> Points {
        return i >= 7 * 24 + 23 ? 1'970'000 : 2'000'000;
    };
    std::vector<Tick> ticks = hourly(10, price);
    ticks.push_back(tick(kT0 + (7 * 24 + 23) * kHour + 30 * fixture::kMinute, 1'970'000, 200));
    std::sort(ticks.begin(), ticks.end(),
              [](const Tick& x, const Tick& y) { return x.ts_us < y.ts_us; });
    const TickStore store = fixture::make_store(dir, ticks);

    AsiaDrift  strat;
    const auto r = BacktestEngine(store, cfg_for(Timeframe::H1)).run(strat);

    int that_night = 0;
    const TimeUs from = kT0 + (7 * 24 + 22) * kHour;
    for (const Trade& t : r.trades) {
        if (t.entry_ts >= from && t.entry_ts < from + 9 * kHour) ++that_night;
    }
    CHECK_EQ(that_night, 1);
}

XAU_TEST(asia_drift_does_nothing_on_bars_longer_than_an_hour) {
    fixture::TempDir dir;
    const TickStore  store =
        fixture::make_store(dir, hourly(28, [](int) -> Points { return 2'000'000; }));
    AsiaDrift  strat;
    const auto r = BacktestEngine(store, cfg_for(Timeframe::H4)).run(strat);
    CHECK_EQ(r.trades.size(), std::size_t{0});
}
