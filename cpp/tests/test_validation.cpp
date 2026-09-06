// Phase 6 statistics, checked against values that can be derived by hand.
//
// These functions decide whether a strategy is believed. If deflated_sharpe
// has a sign error we will believe the wrong thing with great confidence, and
// nothing downstream would catch it -- a wrong probability still looks like a
// probability.

#include "harness.hpp"

#include "xau/validation.hpp"

#include <cmath>
#include <algorithm>
#include <random>
#include <utility>
#include <vector>

using namespace xau;

XAU_TEST(moments_on_a_known_series) {
    // Symmetric: skew must vanish. Variance of {-2,-1,0,1,2} is 2 exactly.
    const std::vector<double> xs = {-2.0, -1.0, 0.0, 1.0, 2.0};
    const Moments             m = moments_of(xs);
    CHECK_NEAR(m.mean, 0.0, 1e-12);
    CHECK_NEAR(m.stdev, std::sqrt(2.0), 1e-12);
    CHECK_NEAR(m.skew, 0.0, 1e-12);
    // Kurtosis of a discrete uniform on 5 points: m4/m2^2 = 3.4/4 = 1.7
    CHECK_NEAR(m.kurtosis, 1.7, 1e-12);
}

XAU_TEST(sharpe_is_mean_over_stdev) {
    const std::vector<double> xs = {1.0, 2.0, 3.0, 4.0, 5.0};
    const Moments             m = moments_of(xs);
    CHECK_NEAR(sharpe_ratio(xs), m.mean / m.stdev, 1e-12);
}

XAU_TEST(sharpe_of_a_flat_series_is_zero_not_infinity) {
    // Zero variance would divide by zero. A constant return series has no
    // risk-adjusted anything, and the honest answer is 0 rather than a NaN
    // that propagates into every later comparison.
    const std::vector<double> flat(20, 0.004);
    CHECK_NEAR(sharpe_ratio(flat), 0.0, 1e-12);
}

XAU_TEST(expected_max_sharpe_grows_with_the_number_of_trials) {
    // The core claim of the deflated Sharpe: search harder, and the best
    // result you find is higher even when nothing has any edge at all.
    const double v = 0.25;   // variance of trial Sharpes
    const double s10 = expected_max_sharpe(v, 10);
    const double s100 = expected_max_sharpe(v, 100);
    const double s1000 = expected_max_sharpe(v, 1000);
    CHECK(s10 > 0.0);
    CHECK(s100 > s10);
    CHECK(s1000 > s100);

    // A single trial cannot be a maximum of anything.
    CHECK_NEAR(expected_max_sharpe(v, 1), 0.0, 1e-12);
}

XAU_TEST(deflated_sharpe_is_half_when_observed_equals_benchmark) {
    // At the benchmark exactly, the probability of exceeding it is 0.5. This
    // pins the centre of the distribution and would catch a sign error.
    const double p = deflated_sharpe(0.5, 0.5, 200, 0.0, 3.0);
    CHECK_NEAR(p, 0.5, 1e-9);
}

XAU_TEST(deflated_sharpe_rises_with_the_observed_ratio) {
    const double lo = deflated_sharpe(0.10, 0.05, 500, 0.0, 3.0);
    const double hi = deflated_sharpe(0.40, 0.05, 500, 0.0, 3.0);
    CHECK(hi > lo);
    CHECK(lo > 0.0);
    CHECK(hi < 1.0);
}

XAU_TEST(deflated_sharpe_punishes_fat_tails_and_negative_skew) {
    const double normal = deflated_sharpe(0.30, 0.05, 500, 0.0, 3.0);
    const double fat = deflated_sharpe(0.30, 0.05, 500, 0.0, 9.0);
    const double skewed = deflated_sharpe(0.30, 0.05, 500, -1.5, 3.0);

    // The same Sharpe measured on uglier returns is weaker evidence.
    CHECK(fat < normal);
    CHECK(skewed < normal);
}

XAU_TEST(deflated_sharpe_rises_with_sample_length) {
    const double shortish = deflated_sharpe(0.20, 0.05, 50, 0.0, 3.0);
    const double longer = deflated_sharpe(0.20, 0.05, 5000, 0.0, 3.0);
    CHECK(longer > shortish);
}

XAU_TEST(cpcv_path_count_matches_the_formula) {
    // C(6,2) = 15 splits, each contributing 2 of 6 groups -> 15*2/6 = 5 paths.
    CHECK_EQ(cpcv_path_count(6, 2), std::size_t{5});
    // C(10,2) = 45 -> 45*2/10 = 9
    CHECK_EQ(cpcv_path_count(10, 2), std::size_t{9});
    CHECK_EQ(cpcv_path_count(4, 1), std::size_t{1});
    CHECK_EQ(cpcv_path_count(5, 0), std::size_t{0});
}

XAU_TEST(cpcv_enumerates_every_combination_once) {
    const auto combos = cpcv_test_combinations(5, 2);
    CHECK_EQ(combos.size(), std::size_t{10});   // C(5,2)
    for (const auto& c : combos) CHECK_EQ(c.size(), std::size_t{2});

    // No duplicates, and each is sorted ascending.
    for (const auto& c : combos) CHECK(c[0] < c[1]);
    std::vector<std::pair<std::size_t, std::size_t>> seen;
    for (const auto& c : combos) seen.emplace_back(c[0], c[1]);
    std::sort(seen.begin(), seen.end());
    CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

XAU_TEST(pbo_is_low_when_one_strategy_is_genuinely_best) {
    // Strategy 0 wins in every block. Selecting it in sample should keep
    // winning out of sample, so overfitting probability must be near zero.
    std::vector<std::vector<double>> perf = {
        {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
        {1.0, 1.0, 1.0, 1.0, 1.0, 1.0},
        {0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0},
    };
    const PboResult r = probability_of_backtest_overfitting(perf);
    CHECK(r.splits > 0);
    CHECK_NEAR(r.pbo, 0.0, 1e-12);
}

XAU_TEST(pbo_is_high_when_the_winner_is_whoever_got_lucky) {
    // Two strategies that are exact mirror images: whichever looks better in
    // one half is necessarily worse in the other. This is the pathological
    // case the statistic exists to detect, and it must report it.
    std::vector<std::vector<double>> perf = {
        {3.0, -3.0, 3.0, -3.0, 3.0, -3.0},
        {-3.0, 3.0, -3.0, 3.0, -3.0, 3.0},
    };
    const PboResult r = probability_of_backtest_overfitting(perf);
    CHECK(r.splits > 0);
    CHECK(r.pbo > 0.5);
}

XAU_TEST(pbo_rejects_malformed_input) {
    // Odd block counts cannot be split in half; too few strategies or blocks
    // make the statistic meaningless. Returning the pessimistic 1.0 is the
    // safe direction -- a silent 0.0 would read as "definitely not overfit".
    CHECK_NEAR(probability_of_backtest_overfitting({{1.0, 2.0}}).pbo, 1.0, 1e-12);
    std::vector<std::vector<double>> odd = {{1, 2, 3, 4, 5}, {5, 4, 3, 2, 1}};
    CHECK_NEAR(probability_of_backtest_overfitting(odd).pbo, 1.0, 1e-12);
}

XAU_TEST(deflated_sharpe_rejects_a_lucky_maximum_from_pure_noise) {
    // End to end: draw 200 zero-edge strategies, take the best, and check the
    // machinery calls it what it is. If this passes with a high probability,
    // the whole Phase 6 gate is worthless.
    std::mt19937_64                  rng(7);
    std::normal_distribution<double> n01(0.0, 1.0);

    const std::size_t   trials = 200, obs = 250;
    std::vector<double> srs;
    srs.reserve(trials);
    for (std::size_t t = 0; t < trials; ++t) {
        std::vector<double> r(obs);
        for (double& x : r) x = n01(rng);
        srs.push_back(sharpe_ratio(r));
    }

    const Moments sm = moments_of(srs);
    const double  best = *std::max_element(srs.begin(), srs.end());
    const double  bench = expected_max_sharpe(sm.stdev * sm.stdev, trials);

    // The luckiest of 200 nulls should not clear the bar that accounts for
    // having drawn 200 times.
    const double p = deflated_sharpe(best, bench, obs, 0.0, 3.0);
    CHECK(p < 0.95);
}
