// Phase 6: the statistics that decide whether a backtest result is real.
//
// We now have a strategy that clears PF 1.05 after doubled costs on
// walk-forward. So did, at some point, almost every strategy that has ever lost
// somebody money. The difference between a discovery and a coincidence is not
// visible in the equity curve; it is a question about how many things were
// tried before this one looked good, and that question has an actual answer.
//
// Three tools, each answering a different way of being wrong:
//
//   CPCV   One backtest path is one sample. Combinatorial purged CV resamples
//          the same data into hundreds of paths, turning a single Sharpe into a
//          DISTRIBUTION -- which is the only form in which a Sharpe means
//          anything. This is the piece the C++ engine makes affordable.
//
//   DSR    A Sharpe of 1.0 found on the first try and a Sharpe of 1.0 found on
//          the fiftieth are not the same evidence. The Deflated Sharpe Ratio
//          discounts for the number of trials, the length of the sample, and
//          the skew and kurtosis of the returns -- gold's returns are neither
//          normal nor symmetric, and a plain Sharpe silently assumes both.
//
//   PBO    Probability of Backtest Overfitting. Split the history many ways,
//          pick the best strategy in each in-sample half, then look at where it
//          ranks out of sample. If the winner is routinely below median
//          afterwards, the selection procedure itself is broken -- and that is
//          a fact about the procedure, not about any one strategy.
//
// Bailey & Lopez de Prado, "The Deflated Sharpe Ratio" (2014) and "The
// Probability of Backtest Overfitting" (2015).

#ifndef XAU_VALIDATION_HPP
#define XAU_VALIDATION_HPP

#include "xau/types.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace xau {

// --- moments ---------------------------------------------------------------

struct Moments {
    double mean = 0.0;
    double stdev = 0.0;
    double skew = 0.0;
    double kurtosis = 3.0;   // NORMAL kurtosis, not excess
    std::size_t n = 0;
};

[[nodiscard]] Moments moments_of(std::span<const double> xs) noexcept;

// Sharpe of a return series, in the units the returns are in. Annualise by
// multiplying by sqrt(periods per year) -- deliberately not done here, because
// a per-trade series and a per-day series need different factors and guessing
// wrong is a silent 4x error.
[[nodiscard]] double sharpe_ratio(std::span<const double> returns) noexcept;

// --- deflated Sharpe -------------------------------------------------------

// The Sharpe you would expect from the LUCKIEST of n_trials independent
// strategies that all truly have zero edge. Anything below this is what
// searching hard looks like, not what an edge looks like.
//
// trial_sr_variance is the variance of the Sharpe ratios across the trials.
[[nodiscard]] double expected_max_sharpe(double trial_sr_variance, std::size_t n_trials) noexcept;

// Probability that the observed Sharpe exceeds the benchmark, given the
// sample's own non-normality. Returns a probability in [0, 1]; the Phase 6
// gate wants it above 0.95 for a claim, and above 0.5 to be worth continuing.
[[nodiscard]] double deflated_sharpe(double observed_sr, double benchmark_sr, std::size_t n_obs,
                                     double skew, double kurtosis) noexcept;

// --- probability of backtest overfitting -----------------------------------

// perf[strategy][block] = that strategy's performance in that block of time.
// Blocks are contiguous, equal-length slices of the history; CSCV forms every
// way of splitting them in half, so `blocks` should be even and small enough
// that C(blocks, blocks/2) stays sane -- 10 to 16 is the usual range.
//
// Returns the fraction of splits in which the strategy that looked best in
// sample landed below the median out of sample.
struct PboResult {
    double      pbo = 1.0;
    std::size_t splits = 0;
    // Out-of-sample performance of the in-sample winner, one per split. A
    // median below zero means the selection is actively harmful.
    std::vector<double> oos_of_is_best;
};

[[nodiscard]] PboResult probability_of_backtest_overfitting(
    const std::vector<std::vector<double>>& perf);

// --- combinatorial purged CV -----------------------------------------------

// Split [0, n_obs) into `groups` contiguous groups and return every way of
// choosing `test_groups` of them as the test set. Each entry is the sorted
// list of group indices used for testing; the caller purges and embargoes.
[[nodiscard]] std::vector<std::vector<std::size_t>> cpcv_test_combinations(
    std::size_t groups, std::size_t test_groups);

// Number of distinct backtest paths CPCV yields, which is what makes the
// resulting Sharpe distribution meaningful rather than decorative.
[[nodiscard]] std::size_t cpcv_path_count(std::size_t groups, std::size_t test_groups) noexcept;

}  // namespace xau

#endif  // XAU_VALIDATION_HPP
