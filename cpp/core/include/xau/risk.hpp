// Risk sizing as a constraint problem, not a preference.
//
// A prop-firm challenge is not "trade well and you pass". It is a race between
// two absorbing barriers: reach the profit target before you touch the drawdown
// limit, within a deadline. That is a first-passage problem, and it has a
// property most people get backwards:
//
//   P(pass) is NOT monotonic in risk.
//
// Risk too little and the deadline arrives before the target. Risk too much and
// the drawdown barrier arrives before either. There is an interior optimum, and
// on any realistic edge it sits far below what feels aggressive. Doubling your
// risk past that point does not double your expected outcome -- it LOWERS your
// probability of passing while raising your probability of ruin. Both at once.
//
// So the risk fraction is solved for, not chosen. And when the edge is weak or
// negative, the honest answer the solver returns is that no fraction passes,
// which is information worth more than a number.
//
// Everything here is driven by the EMPIRICAL trade distribution -- bootstrapped
// from trades the engine actually produced -- rather than by an assumed normal.
// Trading returns are skewed and fat-tailed, and a normal approximation is
// optimistic in exactly the tail that decides whether you bust.

#ifndef XAU_RISK_HPP
#define XAU_RISK_HPP

#include "xau/symbol_spec.hpp"
#include "xau/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xau {

// The firm's rules. Defaults are FTMO-shaped; every firm differs, and getting
// one of these wrong invalidates the whole calculation, so they are explicit
// rather than assumed.
struct PropRules {
    double profit_target_frac = 0.10;   // +10% to pass
    double max_drawdown_frac = 0.10;    // -10% from peak (or from initial: see below)
    double daily_loss_frac = 0.05;      // -5% in any one day
    bool   trailing_drawdown = false;   // true: from equity peak; false: from initial
    int    max_days = 30;               // deadline; 0 = unlimited
    int    min_trading_days = 4;        // some firms require a minimum
};

struct PassResult {
    double passed = 0.0;          // fraction of paths that hit the target
    double busted_drawdown = 0.0;
    double busted_daily = 0.0;
    double expired = 0.0;         // ran out of days
    double median_days_to_pass = 0.0;
    double median_final_equity = 0.0;
};

// Monte Carlo the challenge by bootstrapping from a set of realised per-trade
// returns, expressed as a FRACTION OF EQUITY AT ENTRY at the reference risk.
//
// trades_per_day scales the empirical trade rate into the simulation clock.
// risk_scale multiplies every trade's return, which is what "risking more"
// actually does to a fixed strategy.
[[nodiscard]] PassResult simulate_challenge(std::span<const double> trade_returns,
                                            const PropRules& rules, double risk_scale,
                                            double trades_per_day, std::size_t paths,
                                            std::uint64_t seed);

struct RiskSolution {
    double     best_scale = 0.0;
    PassResult best;
    // P(pass) across the scanned grid, so the shape is visible rather than just
    // the peak. A flat or falling curve is itself the answer.
    std::vector<std::pair<double, double>> curve;   // (scale, P(pass))
    bool       any_viable = false;                  // did anything clear 50%?
};

// Scan risk scales and return the one maximising P(pass).
[[nodiscard]] RiskSolution solve_risk_scale(std::span<const double> trade_returns,
                                            const PropRules& rules, double trades_per_day,
                                            double min_scale, double max_scale,
                                            std::size_t steps, std::size_t paths,
                                            std::uint64_t seed);

// --- position sizing -------------------------------------------------------

struct SizingConfig {
    // Fraction of equity to put at risk on a single trade, i.e. what is lost if
    // the stop is hit exactly.
    double risk_per_trade = 0.005;   // 0.5%

    // Volatility targeting: scale size so that each trade's expected
    // contribution to portfolio volatility is roughly constant. Without this a
    // fixed lot size takes several times more real risk in a wild week than a
    // quiet one, which is how a strategy with a good average has a fatal month.
    bool   vol_target = true;
    double target_atr_pts = 0.0;   // 0 = use the running mean as the target

    double max_lots = 1.0;
    double min_lots = 0.01;
};

// Lots to trade, given the stop distance and current volatility. Returns 0 when
// the stop is too tight for the broker or the size rounds below the minimum --
// both are reasons to skip the trade rather than to fudge it.
[[nodiscard]] double size_by_risk(double equity, Points stop_dist_pts, double atr_pts,
                                  const SymbolSpec& spec, const SizingConfig& cfg) noexcept;

}  // namespace xau

#endif  // XAU_RISK_HPP
