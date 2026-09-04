// Event-driven, tick-resolution backtest engine.
//
// Walks the tick stream and resolves stops and targets in true sequence, so the
// engine never has to guess which of a stop and a target was hit first inside a
// bar. Guessing that biases results, and it biases them in the flattering
// direction.
//
// One position at a time in Phase 1. Concurrent positions arrive with the risk
// layer in Phase 7, where they belong alongside the exposure limits that make
// them safe.
#pragma once

#include "xau/bar.hpp"
#include "xau/costs.hpp"
#include "xau/order.hpp"
#include "xau/strategy.hpp"
#include "xau/symbol_spec.hpp"
#include "xau/tick_store.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace xau {

struct RiskConfig {
    enum class Mode : std::uint8_t { FixedLots, FixedFractional };

    Mode   mode = Mode::FixedLots;
    double fixed_lots = 0.01;
    double risk_fraction = 0.005;   // 0.5% of equity per trade
    double max_lots = 5.0;
};

// Lots for a trade risking `risk_fraction` of equity over `sl_dist_pts`.
// Returns 0 when the result is below the broker's minimum volume — the caller
// must treat that as "no trade", never as "round it up".
[[nodiscard]] double size_position(const RiskConfig& risk, const SymbolSpec& spec,
                                   double equity, Points sl_dist_pts) noexcept;

struct BacktestConfig {
    SymbolSpec spec{};
    CostModel  costs{};
    RiskConfig risk{};
    Timeframe  tf = Timeframe::M15;

    double initial_balance = 10'000.0;
    TimeUs from_us = 0;
    TimeUs to_us = 0;            // 0 = to the end of the store
    bool   close_at_end = true;

    // Swap is charged when this hour rolls over. The broker's rollover is
    // midnight *server* time, which is UTC+2/+3 and shifts with DST; the
    // correct value comes from config/symbol_spec.json. 21:00 UTC is the
    // right ballpark and is only load-bearing for positions held overnight.
    int  swap_hour_utc = 21;
    bool apply_swap = true;

    // Cap on retained closed bars. 0 keeps everything, which is what a strategy
    // with long lookbacks needs; a small number bounds memory on a decade run.
    std::size_t max_history_bars = 0;
};

struct BacktestStats {
    std::uint64_t ticks = 0;
    std::uint64_t bars = 0;
    std::uint64_t signals = 0;
    std::uint64_t rejected_stop_too_close = 0;
    std::uint64_t rejected_stop_inside_spread = 0;
    std::uint64_t rejected_volume = 0;
    std::uint64_t rejected_in_position = 0;
    std::uint64_t swap_charges = 0;
    double        wall_seconds = 0.0;

    [[nodiscard]] std::uint64_t rejected_total() const noexcept {
        return rejected_stop_too_close + rejected_stop_inside_spread + rejected_volume +
               rejected_in_position;
    }
};

struct Metrics {
    std::size_t trades = 0;
    std::size_t wins = 0;
    std::size_t losses = 0;
    double win_rate = 0.0;

    double gross_profit = 0.0;   // sum of winners
    double gross_loss = 0.0;     // sum of losers, positive
    double net_profit = 0.0;
    double profit_factor = 0.0;  // 0 when there are no losses
    double expectancy_usd = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;

    double total_commission = 0.0;
    double total_swap = 0.0;

    double max_drawdown_usd = 0.0;
    double max_drawdown_pct = 0.0;
    double return_pct = 0.0;
    double sharpe = 0.0;   // annualised from per-bar equity returns

    static Metrics compute(const std::vector<Trade>& trades,
                           const std::vector<EquityPoint>& equity, double initial_balance,
                           Timeframe tf);

    [[nodiscard]] std::string to_string() const;
};

struct BacktestResult {
    std::vector<Trade>       trades;
    std::vector<EquityPoint> equity;
    BacktestStats            stats;
    Metrics                  metrics;

    double initial_balance = 0.0;
    double final_balance = 0.0;
    std::string strategy_name;

    [[nodiscard]] std::string summary() const;
};

class BacktestEngine {
public:
    BacktestEngine(const TickStore& store, BacktestConfig cfg);

    [[nodiscard]] BacktestResult run(Strategy& strategy);

    [[nodiscard]] const BacktestConfig& config() const noexcept { return cfg_; }

private:
    const TickStore& store_;
    BacktestConfig   cfg_;
};

}  // namespace xau
