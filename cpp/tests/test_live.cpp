// Live/backtest parity.
//
// The property under test: fed the same ticks with the same position, the
// strategy sees the same bars, at the same nominal times, with the same history
// length, whether it is driven by the backtest engine or by LiveSession. If
// that holds, a deterministic strategy decides the same things in both, and the
// backtest describes the program that actually trades.
//
// Ticks come from an integer LCG rather than a standard-library distribution.
// Those sequences are implementation-defined and differ between MSVC and
// libstdc++, and a parity test that is itself not portable proves nothing.

#include "fixture.hpp"
#include "harness.hpp"

#include "xau/engine.hpp"
#include "xau/live.hpp"
#include "xau/session.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace xau;

namespace {

struct Seen {
    TimeUs      now_us = 0;
    std::size_t history_size = 0;
    Bar         bar{};
};

// Never trades, so the position stays flat on both sides and the comparison is
// about bars alone, with no fill model in the loop.
class Recorder final : public Strategy {
public:
    Recorder(std::vector<Seen>& out, std::size_t warmup) : out_(out), warmup_(warmup) {}
    [[nodiscard]] const char* name() const noexcept override { return "Recorder"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override { return warmup_; }
    [[nodiscard]] Decision    on_bar(const BarContext& c) override {
        out_.push_back(Seen{c.now_us, c.history.size(), c.bar()});
        return Decision::hold();
    }

private:
    std::vector<Seen>& out_;
    std::size_t        warmup_;
};

// Wed 2020-01-01 through Thu 2020-01-09, with gold's weekend close from Friday
// 22:00 to Sunday 22:00 UTC removed. Irregular spacing, a random walk and a
// varying spread, all from integer arithmetic.
std::vector<Tick> week_of_ticks() {
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    const auto    next = [&state]() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state >> 33);
    };

    const TimeUs fri_close = fixture::kT0 + 2 * kUsPerDay + 22 * kUsPerHour;
    const TimeUs sun_open = fixture::kT0 + 4 * kUsPerDay + 22 * kUsPerHour;
    const TimeUs end = fixture::kT0 + 8 * kUsPerDay + 20 * kUsPerHour;

    std::vector<Tick> out;
    TimeUs            t = fixture::kT0;
    Points            px = 1'520'000;
    while (t < end) {
        if (t >= fri_close && t < sun_open) {
            t = sun_open;
            continue;
        }
        const auto step = static_cast<Points>(next() % 601) - 300;
        px += step;
        const auto spread = static_cast<std::uint16_t>(150 + next() % 300);
        out.push_back(fixture::tick(t, px, spread));
        t += static_cast<TimeUs>(1 + next() % 20) * 1'000'000LL;
    }
    return out;
}

std::vector<Seen> through_engine(const TickStore& store, Timeframe tf, std::size_t warmup,
                                 std::size_t max_history) {
    std::vector<Seen> seen;
    Recorder          rec(seen, warmup);
    BacktestConfig    cfg;
    cfg.spec = SymbolSpec::xauusd_default();
    cfg.tf = tf;
    cfg.initial_balance = 10'000.0;
    cfg.max_history_bars = max_history;
    (void)BacktestEngine(store, cfg).run(rec);
    return seen;
}

std::vector<Seen> through_live(const std::vector<Tick>& ticks, Timeframe tf, std::size_t warmup,
                               std::size_t max_history) {
    std::vector<Seen> seen;
    Recorder          rec(seen, warmup);
    LiveSession       live(tf, rec, max_history);
    live.start(SymbolSpec::xauusd_default());
    const Position flat{};
    for (const Tick& t : ticks) (void)live.on_tick(t, flat, 10'000.0, 10'000.0);
    return seen;
}

bool same_bar(const Bar& a, const Bar& b) {
    return a.open_time_us == b.open_time_us && a.open == b.open && a.high == b.high &&
           a.low == b.low && a.close == b.close && a.ticks == b.ticks &&
           a.spread_mean_pts == b.spread_mean_pts && a.spread_max_pts == b.spread_max_pts;
}

// Returns the index of the first mismatch, or -1. An index is far more useful
// than a bare false when this fails: it says WHICH bar diverged.
long first_divergence(const std::vector<Seen>& a, const std::vector<Seen>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i].now_us != b[i].now_us || a[i].history_size != b[i].history_size ||
            !same_bar(a[i].bar, b[i].bar)) {
            return static_cast<long>(i);
        }
    }
    return a.size() == b.size() ? -1 : static_cast<long>(n);
}

}  // namespace

XAU_TEST(live_session_sees_exactly_the_bars_the_engine_sees) {
    fixture::TempDir           dir;
    const std::vector<Tick> ticks = week_of_ticks();
    const TickStore         store = fixture::make_store(dir, ticks);

    const auto engine = through_engine(store, Timeframe::H1, 5, 0);
    const auto live = through_live(ticks, Timeframe::H1, 5, 0);

    CHECK(engine.size() > 100);   // a real run, not two empty vectors agreeing
    CHECK_EQ(engine.size(), live.size());
    CHECK_EQ(first_divergence(engine, live), -1L);
}

XAU_TEST(parity_holds_across_history_trimming) {
    // max_history_bars of 20 trims at 40 bars, many times over this run. The
    // trim has to land AFTER the strategy sees the bar on both sides, or
    // history_size diverges on exactly the bars where it fires.
    fixture::TempDir           dir;
    const std::vector<Tick> ticks = week_of_ticks();
    const TickStore         store = fixture::make_store(dir, ticks);

    const auto engine = through_engine(store, Timeframe::H1, 5, 20);
    const auto live = through_live(ticks, Timeframe::H1, 5, 20);

    CHECK_EQ(first_divergence(engine, live), -1L);

    // And trimming really did fire, or this test proved nothing about it.
    bool saw_trim = false;
    for (std::size_t i = 1; i < live.size(); ++i) {
        if (live[i].history_size < live[i - 1].history_size) saw_trim = true;
    }
    CHECK(saw_trim);
}

XAU_TEST(parity_holds_on_m15_with_a_long_warmup) {
    fixture::TempDir           dir;
    const std::vector<Tick> ticks = week_of_ticks();
    const TickStore         store = fixture::make_store(dir, ticks);

    // max_history must exceed warmup. At 50 against a warmup of 64, every
    // trim drops history below the gate and the strategy goes silent until it
    // regrows -- see max_history_below_warmup_is_rejected.
    const auto engine = through_engine(store, Timeframe::M15, 64, 100);
    const auto live = through_live(ticks, Timeframe::M15, 64, 100);

    CHECK(engine.size() > 200);
    CHECK_EQ(first_divergence(engine, live), -1L);
    // Warmup is respected identically: the first call comes after bar 64.
    CHECK(!live.empty());
    if (!live.empty()) CHECK(live.front().history_size == 65);
}

XAU_TEST(the_bar_before_a_weekend_is_stamped_at_its_boundary) {
    // Friday's 21:00 bar does not close until Sunday's first tick arrives, but
    // it is reported as closing at Friday 22:00. The strategy is told the time
    // the bar ENDED, not the time the market resumed.
    const std::vector<Tick> ticks = week_of_ticks();

    const auto live = through_live(ticks, Timeframe::H1, 5, 0);

    const TimeUs fri_close = fixture::kT0 + 2 * kUsPerDay + 22 * kUsPerHour;
    const TimeUs sun_bar_close = fixture::kT0 + 4 * kUsPerDay + 23 * kUsPerHour;

    bool found = false;
    for (std::size_t i = 0; i + 1 < live.size(); ++i) {
        if (live[i].now_us == fri_close) {
            found = true;
            // The very next bar the strategy sees is Sunday's first hour: the
            // weekend produced no bars at all rather than empty ones.
            CHECK_EQ(live[i + 1].now_us, sun_bar_close);
        }
    }
    CHECK(found);
}

XAU_TEST(max_history_below_warmup_is_rejected) {
    // A cap at or below warmup would silence the strategy after every trim.
    // Both paths must refuse it -- and refuse it identically, since they share
    // the check.
    std::vector<Seen> seen;
    Recorder          rec(seen, 64);

    CHECK_THROWS(LiveSession(Timeframe::M15, rec, 50));
    CHECK_THROWS(LiveSession(Timeframe::M15, rec, 64));   // equal is still silent

    fixture::TempDir        dir;
    const std::vector<Tick> ticks = week_of_ticks();
    const TickStore         store = fixture::make_store(dir, ticks);
    CHECK_THROWS(through_engine(store, Timeframe::M15, 64, 50));

    // Zero means never trim, and one bar above warmup is the smallest safe cap.
    LiveSession ok_never(Timeframe::M15, rec, 0);
    LiveSession ok_edge(Timeframe::M15, rec, 65);
    CHECK(!ok_never.started());
    CHECK(!ok_edge.started());
}

XAU_TEST(an_unstarted_session_decides_nothing) {
    // A session that was never told its instrument must not call the strategy.
    std::vector<Seen>       seen;
    Recorder                rec(seen, 0);
    LiveSession             live(Timeframe::H1, rec, 0);
    const std::vector<Tick> ticks = week_of_ticks();
    const Position          flat{};
    for (const Tick& t : ticks) {
        const Decision d = live.on_tick(t, flat, 10'000.0, 10'000.0);
        CHECK(d.kind == Decision::Kind::Hold);
    }
    CHECK(seen.empty());
    CHECK(!live.started());
}
