#include "xau/risk.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace xau {

namespace {

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    return v[mid];
}

}  // namespace

PassResult simulate_challenge(std::span<const double> trade_returns, const PropRules& rules,
                              double risk_scale, double trades_per_day, std::size_t paths,
                              std::uint64_t seed) {
    PassResult out;
    if (trade_returns.empty() || paths == 0 || trades_per_day <= 0.0) return out;

    std::mt19937_64                          rng(seed);
    std::uniform_int_distribution<std::size_t> pick(0, trade_returns.size() - 1);

    std::size_t         passed = 0, dd = 0, daily = 0, expired = 0;
    std::vector<double> days_to_pass;
    std::vector<double> finals;
    finals.reserve(paths);

    const int    max_days = rules.max_days > 0 ? rules.max_days : 3650;
    const double target = 1.0 + rules.profit_target_frac;

    for (std::size_t p = 0; p < paths; ++p) {
        double equity = 1.0;
        double peak = 1.0;
        bool   done = false;

        for (int day = 1; day <= max_days && !done; ++day) {
            const double day_start_equity = equity;

            // Poisson-ish trade count: the number of trades in a day varies,
            // and holding it fixed understates how bad a cluster of losses can
            // be. That clustering is precisely what breaks a daily-loss rule.
            std::poisson_distribution<int> n_trades(trades_per_day);
            const int                      n = n_trades(rng);

            for (int t = 0; t < n; ++t) {
                // Bootstrap with replacement from the realised distribution,
                // which keeps the skew and the fat tail that a normal drops.
                const double r = trade_returns[pick(rng)] * risk_scale;
                equity *= (1.0 + r);
                peak = std::max(peak, equity);

                // Barrier order matters and is deliberate: ruin is checked
                // before the target on the same trade. A path that would have
                // touched both has busted -- the firm closes the account, it
                // does not congratulate you for the round trip.
                const double dd_floor =
                    rules.trailing_drawdown ? peak * (1.0 - rules.max_drawdown_frac)
                                            : (1.0 - rules.max_drawdown_frac);
                if (equity <= dd_floor) {
                    ++dd;
                    done = true;
                    break;
                }
                if (equity <= day_start_equity * (1.0 - rules.daily_loss_frac)) {
                    ++daily;
                    done = true;
                    break;
                }
                if (equity >= target && day >= rules.min_trading_days) {
                    ++passed;
                    days_to_pass.push_back(static_cast<double>(day));
                    done = true;
                    break;
                }
            }
        }
        if (!done) ++expired;
        finals.push_back(equity);
    }

    const double n = static_cast<double>(paths);
    out.passed = static_cast<double>(passed) / n;
    out.busted_drawdown = static_cast<double>(dd) / n;
    out.busted_daily = static_cast<double>(daily) / n;
    out.expired = static_cast<double>(expired) / n;
    out.median_days_to_pass = median_of(days_to_pass);
    out.median_final_equity = median_of(finals);
    return out;
}

RiskSolution solve_risk_scale(std::span<const double> trade_returns, const PropRules& rules,
                              double trades_per_day, double min_scale, double max_scale,
                              std::size_t steps, std::size_t paths, std::uint64_t seed) {
    RiskSolution sol;
    if (steps < 2 || max_scale <= min_scale) return sol;

    for (std::size_t i = 0; i < steps; ++i) {
        const double f = static_cast<double>(i) / static_cast<double>(steps - 1);
        const double scale = min_scale + f * (max_scale - min_scale);
        // Same seed at every scale: the paths differ only by the risk, so the
        // comparison is not contaminated by which random draws each got.
        const PassResult r =
            simulate_challenge(trade_returns, rules, scale, trades_per_day, paths, seed);
        sol.curve.emplace_back(scale, r.passed);
        if (r.passed > sol.best.passed) {
            sol.best = r;
            sol.best_scale = scale;
        }
    }
    sol.any_viable = sol.best.passed >= 0.50;
    return sol;
}

double size_by_risk(double equity, Points stop_dist_pts, double atr_pts, const SymbolSpec& spec,
                    const SizingConfig& cfg) noexcept {
    if (equity <= 0.0 || stop_dist_pts <= 0) return 0.0;
    if (!spec.stop_distance_ok(stop_dist_pts)) return 0.0;

    const double usd_at_risk = equity * cfg.risk_per_trade;
    const double usd_per_point = spec.usd_per_point_per_lot();
    if (usd_per_point <= 0.0) return 0.0;

    double lots = usd_at_risk / (static_cast<double>(stop_dist_pts) * usd_per_point);

    // Volatility targeting. The stop is already in ATR units for every strategy
    // here, so risking a fixed fraction per trade ALREADY normalises most of
    // the volatility. This handles the residual: when ATR itself is far from
    // its own norm, the whole opportunity set is different, not just the stop.
    if (cfg.vol_target && cfg.target_atr_pts > 0.0 && atr_pts > 0.0) {
        lots *= cfg.target_atr_pts / atr_pts;
    }

    lots = std::min(lots, cfg.max_lots);
    lots = spec.round_lots(lots);
    return lots < cfg.min_lots ? 0.0 : lots;
}

}  // namespace xau
