// Walk-forward evaluation.
//
// The plan is emphatic that a single aggregate number is not evidence: report
// the distribution across folds. One good Sharpe over ten years can be one
// lucky quarter carrying nine mediocre ones, and only the per-fold spread shows
// that.
//
// Rule baselines have nothing to fit, so "walk-forward" here means consecutive
// out-of-sample windows rather than train/test pairs. The shape is the same one
// Phase 5 will use when there IS something to fit, and the strategy is rebuilt
// per fold so no state — armed setups, indicator seeds, day counters — can leak
// across a boundary.
#pragma once

#include "xau/engine.hpp"
#include "xau/session.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xau {

struct WalkForwardConfig {
    TimeUs      test_span_us = 90 * kUsPerDay;   // three-month windows
    TimeUs      step_us = 0;                     // 0 = non-overlapping
    std::size_t min_trades_per_fold = 1;         // below this, excluded from the distribution
};

struct Fold {
    TimeUs        from_us = 0;
    TimeUs        to_us = 0;
    Metrics       metrics;
    BacktestStats stats;
};

struct WalkForwardResult {
    std::string       strategy_name;
    std::vector<Fold> folds;

    // Metrics over every trade from every fold, with balance rebuilt as one
    // path. The drawdown here is trade-level, not the tick-level figure a
    // single run reports — it cannot see intra-trade excursion.
    Metrics pooled;

    std::size_t folds_counted = 0;          // folds meeting min_trades_per_fold
    std::size_t folds_without_losses = 0;   // profit factor undefined for these
    double      median_profit_factor = 0.0;
    double      worst_profit_factor = 0.0;
    double      frac_folds_profitable = 0.0;
    double      wall_seconds = 0.0;

    [[nodiscard]] std::string fold_table() const;
    [[nodiscard]] std::string summary() const;
};

[[nodiscard]] WalkForwardResult run_walk_forward(const TickStore& store,
                                                 const BacktestConfig& cfg,
                                                 const StrategyFactory& make,
                                                 const WalkForwardConfig& wf);

}  // namespace xau
