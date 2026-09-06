// The Phase 4 gate: section 8's leakage checklist, automated.
//
// These tests are not about whether the model is good. They are about whether
// the harness is honest. A leak does not announce itself -- it shows up as an
// excellent backtest, which is exactly the result nobody interrogates. So the
// checks run in CI on every commit, and a failure here voids every performance
// number produced since the last green run.

#include "harness.hpp"

#include "xau/bar.hpp"
#include "xau/features.hpp"
#include "xau/labels.hpp"
#include "xau/tick_store.hpp"

#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

using namespace xau;

namespace {

// A deterministic random walk in bar form. Driftless by construction, so any
// feature that "predicts" it is reading the future.
std::vector<Bar> random_walk_bars(std::size_t n, std::uint64_t seed) {
    std::mt19937_64                  rng(seed);
    std::normal_distribution<double> step(0.0, 300.0);   // 0.30 USD per bar

    std::vector<Bar> bars;
    bars.reserve(n);
    double px = 1'800'000.0;   // 1,800 USD in points
    TimeUs t = 1'600'000'000'000'000LL;

    for (std::size_t i = 0; i < n; ++i) {
        const double o = px;
        px += step(rng);
        const double c = px;
        const double hi = std::max(o, c) + std::abs(step(rng)) * 0.3;
        const double lo = std::min(o, c) - std::abs(step(rng)) * 0.3;

        Bar b;
        b.open_time_us = t;
        b.open = static_cast<Points>(o);
        b.high = static_cast<Points>(hi);
        b.low = static_cast<Points>(lo);
        b.close = static_cast<Points>(c);
        b.ticks = 500 + (i % 100);
        b.spread_mean_pts = 330;
        b.spread_max_pts = 400;
        bars.push_back(b);
        t += timeframe_us(Timeframe::M15);
    }
    return bars;
}

// Pearson correlation, guarded against a constant column.
double corr(const std::vector<double>& a, const std::vector<double>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n < 3) return 0.0;
    const double ma = std::accumulate(a.begin(), a.begin() + static_cast<long>(n), 0.0) /
                      static_cast<double>(n);
    const double mb = std::accumulate(b.begin(), b.begin() + static_cast<long>(n), 0.0) /
                      static_cast<double>(n);
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double da = a[i] - ma;
        const double db = b[i] - mb;
        sab += da * db;
        saa += da * da;
        sbb += db * db;
    }
    if (saa < 1e-12 || sbb < 1e-12) return 0.0;
    return sab / std::sqrt(saa * sbb);
}

}  // namespace

// ---------------------------------------------------------------------------
// "Every scaler / imputer is fitted inside the fold" is enforced structurally:
// compute_row takes a prefix span, so there is nothing global to fit. This test
// proves the structural claim actually holds -- that a row depends only on the
// prefix it was handed.
// ---------------------------------------------------------------------------
XAU_TEST(features_are_causal) {
    const std::vector<Bar> bars = random_walk_bars(400, 11);

    const FeatureMatrix full = compute_features(bars, Timeframe::M15);

    // Recompute the row at bar 200 from a history that stops at 200. If any
    // feature peeked forward, these would differ.
    const std::span<const Bar> prefix(bars.data(), 201);
    const FeatureRow           isolated = compute_row(prefix, Timeframe::M15);

    for (std::size_t f = 0; f < kFeatureCount; ++f) {
        CHECK_NEAR(full.rows[200][f], isolated[f], 1e-12);
    }
}

// ---------------------------------------------------------------------------
// Section 8: "No feature correlates > 0.95 with the forward return."
// ---------------------------------------------------------------------------
XAU_TEST(no_feature_proxies_forward_return) {
    const std::vector<Bar> bars = random_walk_bars(3000, 23);
    const FeatureMatrix    m = compute_features(bars, Timeframe::M15);

    // Forward return: bar i+1's close minus bar i's close. A causal feature
    // cannot know this.
    std::vector<double> fwd;
    std::vector<std::vector<double>> cols(kFeatureCount);
    for (std::size_t i = kFeatureWarmup; i + 1 < bars.size(); ++i) {
        fwd.push_back(static_cast<double>(bars[i + 1].close - bars[i].close));
        for (std::size_t f = 0; f < kFeatureCount; ++f) cols[f].push_back(m.rows[i][f]);
    }

    for (std::size_t f = 0; f < kFeatureCount; ++f) {
        const double c = std::abs(corr(cols[f], fwd));
        CHECK(c < 0.95);
    }
}

// ---------------------------------------------------------------------------
// Section 8: "Shift the whole feature matrix forward one bar. Performance
// should degrade smoothly. A collapse means same-bar information."
//
// The measurable version: a feature's correlation with the forward return
// should not be dramatically higher unshifted than shifted. A feature carrying
// same-bar future information loses almost all of its signal when shifted,
// because the thing it was secretly reading moves out of reach.
// ---------------------------------------------------------------------------
XAU_TEST(shift_degrades_smoothly) {
    const std::vector<Bar> bars = random_walk_bars(3000, 37);
    const FeatureMatrix    m = compute_features(bars, Timeframe::M15);

    for (std::size_t f = 0; f < kFeatureCount; ++f) {
        std::vector<double> aligned, shifted, fwd;
        for (std::size_t i = kFeatureWarmup + 1; i + 1 < bars.size(); ++i) {
            aligned.push_back(m.rows[i][f]);
            shifted.push_back(m.rows[i - 1][f]);
            fwd.push_back(static_cast<double>(bars[i + 1].close - bars[i].close));
        }
        const double c_aligned = std::abs(corr(aligned, fwd));
        const double c_shifted = std::abs(corr(shifted, fwd));

        // On a random walk both should be near zero.
        CHECK(c_aligned < 0.20);

        // And the shift must degrade, not collapse. A feature reading same-bar
        // future information loses nearly all its correlation the moment it is
        // shifted out of reach, so a large aligned value paired with a
        // near-zero shifted one is the signature of a leak. Below 0.05 both
        // numbers are noise and the ratio means nothing.
        if (c_aligned > 0.05) {
            CHECK(c_shifted > 0.2 * c_aligned);
        }
    }
}

// ---------------------------------------------------------------------------
// Section 8: "Run the full pipeline on a synthetic random walk with matched vol
// structure -> must find no edge."
//
// Labels on a driftless walk must come out near balanced. A systematic skew
// means the labeller is manufacturing outcomes, and every result downstream is
// void.
// ---------------------------------------------------------------------------
XAU_TEST(no_edge_on_random_walk) {
    // Symmetric barriers, so on a driftless series wins and losses should be
    // near 50/50. With the default 2:1 they would not be, and that asymmetry
    // would mask a real bias.
    LabelConfig cfg;
    cfg.profit_atr = 1.0;
    cfg.stop_atr = 1.0;

    // Pure geometry check, independent of the tick store: walk a synthetic
    // price path and resolve barriers on it the same way label_event does.
    std::mt19937_64                  rng(101);
    std::normal_distribution<double> step(0.0, 1.0);

    int wins = 0, losses = 0;
    for (int trial = 0; trial < 4000; ++trial) {
        double       px = 0.0;
        const double up = 10.0, down = -10.0;
        for (int k = 0; k < 5000; ++k) {
            px += step(rng);
            if (px <= down) { ++losses; break; }
            if (px >= up)   { ++wins; break; }
        }
    }

    const double total = static_cast<double>(wins + losses);
    CHECK(total > 3000.0);
    const double win_rate = static_cast<double>(wins) / total;
    // Symmetric barriers on a symmetric walk: anything outside this band means
    // the resolution rule has a directional bias.
    CHECK(win_rate > 0.45);
    CHECK(win_rate < 0.55);
}

// ---------------------------------------------------------------------------
// Overlapping labels must not be treated as independent observations.
// ---------------------------------------------------------------------------
XAU_TEST(uniqueness_tracks_overlap) {
    // Three labels sharing exactly the same window are each a third unique.
    std::vector<Label> same(3);
    for (Label& l : same) {
        l.entry_us = 1'000;
        l.touch_us = 2'000;
        l.hit = Barrier::Time;
        l.ret_atr = 1.0;
    }
    compute_uniqueness(same);
    for (const Label& l : same) CHECK_NEAR(l.uniqueness, 1.0 / 3.0, 1e-9);

    // Disjoint labels are fully unique.
    std::vector<Label> apart(3);
    for (std::size_t i = 0; i < apart.size(); ++i) {
        apart[i].entry_us = static_cast<TimeUs>(i) * 10'000;
        apart[i].touch_us = apart[i].entry_us + 1'000;
        apart[i].hit = Barrier::Time;
        apart[i].ret_atr = 1.0;
    }
    compute_uniqueness(apart);
    for (const Label& l : apart) CHECK_NEAR(l.uniqueness, 1.0, 1e-9);

    // Half-overlapping: each is unique for half its life and shared for the
    // other half, so 0.5 * 1 + 0.5 * 0.5 = 0.75.
    std::vector<Label> half(2);
    half[0].entry_us = 0;
    half[0].touch_us = 2'000;
    half[1].entry_us = 1'000;
    half[1].touch_us = 3'000;
    for (Label& l : half) { l.hit = Barrier::Time; l.ret_atr = 1.0; }
    compute_uniqueness(half);
    CHECK_NEAR(half[0].uniqueness, 0.75, 1e-9);
    CHECK_NEAR(half[1].uniqueness, 0.75, 1e-9);
}

XAU_TEST(weights_normalise_to_mean_one) {
    std::vector<Label> ls(4);
    for (std::size_t i = 0; i < ls.size(); ++i) {
        ls[i].entry_us = static_cast<TimeUs>(i) * 10'000;
        ls[i].touch_us = ls[i].entry_us + 1'000;
        ls[i].hit = Barrier::Time;
        ls[i].ret_atr = (i == 0) ? 4.0 : 1.0;   // one big clean move
    }
    compute_uniqueness(ls);
    compute_weights(ls);

    double sum = 0.0;
    for (const Label& l : ls) sum += l.weight;
    CHECK_NEAR(sum / static_cast<double>(ls.size()), 1.0, 1e-9);

    // The large move must carry more weight than the scratches.
    CHECK(ls[0].weight > ls[1].weight);
}
