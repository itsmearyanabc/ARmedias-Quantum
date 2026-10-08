// How sure can we be that a strategy has an edge, given the trades it has made?
//
// A strategy's true expected return per trade is unknown; its trades are noisy
// draws around it. This keeps a Bayesian belief about that expectation and
// updates it trade by trade: Normal returns with unknown mean and variance,
// and the conjugate Normal-Inverse-Gamma prior, so the belief after n trades
// is exact and needs no simulation.
//
// The prior is deliberately SKEPTICAL: centred on zero edge, worth a number of
// trades' evidence. A new strategy must earn belief with results; a backtest
// does not buy it any, because the lab's own figures show how far backtests
// across hundreds of tries flatter the best of them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xau {

struct EdgePrior {
    double mean = 0.0;          // prior expected return per trade: no edge
    double strength = 10.0;     // worth this many trades of evidence (kappa_0)
    double shape = 3.0;         // alpha_0 of the variance prior, > 1
    double var = 1e-4;          // prior guess of the per-trade return variance
};

struct EdgePosterior {
    std::size_t n = 0;          // trades seen
    double      mean = 0.0;     // posterior expected return per trade
    double      scale = 0.0;    // Student-t scale of that expectation
    double      dof = 0.0;      // Student-t degrees of freedom
    double      p_edge = 0.5;   // P(expected return > 0 | trades)
};

// Posterior after `returns` (per-trade, scale-free: e.g. net / balance).
[[nodiscard]] EdgePosterior edge_posterior(std::span<const double> returns, const EdgePrior& prior);

// Student-t cumulative distribution, P(T <= t) with `dof` degrees of freedom.
[[nodiscard]] double student_t_cdf(double t, double dof) noexcept;

// P(each strategy has the highest true expectation), by sampling every
// posterior `draws` times. Seeded: the same inputs give the same answer.
[[nodiscard]] std::vector<double> prob_best(std::span<const EdgePosterior> posts, std::size_t draws,
                                            std::uint64_t seed);

}  // namespace xau
