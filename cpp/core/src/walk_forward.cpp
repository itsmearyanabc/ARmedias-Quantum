#include "xau/walk_forward.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace xau {
namespace {

std::string ymd(TimeUs us) {
    const std::time_t t = static_cast<std::time_t>(us / 1'000'000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

}  // namespace

WalkForwardResult run_walk_forward(const TickStore& store, const BacktestConfig& cfg,
                                   const StrategyFactory& make, const WalkForwardConfig& wf) {
    const auto wall_start = std::chrono::steady_clock::now();

    WalkForwardResult r;
    const TimeUs from = cfg.from_us ? cfg.from_us : store.first_ts();
    const TimeUs to = cfg.to_us ? cfg.to_us : store.last_ts() + 1;
    const TimeUs step = wf.step_us ? wf.step_us : wf.test_span_us;
    if (step <= 0 || wf.test_span_us <= 0 || to <= from) return r;

    std::vector<Trade> all_trades;

    for (TimeUs s = from; s < to; s += step) {
        const TimeUs e = std::min(s + wf.test_span_us, to);
        if (e <= s) break;

        BacktestConfig fold_cfg = cfg;
        fold_cfg.from_us = s;
        fold_cfg.to_us = e;

        std::unique_ptr<Strategy> strat = make();
        if (!strat) break;

        const BacktestResult br = BacktestEngine(store, fold_cfg).run(*strat);
        if (r.strategy_name.empty()) r.strategy_name = br.strategy_name;

        Fold f;
        f.from_us = s;
        f.to_us = e;
        f.metrics = br.metrics;
        f.stats = br.stats;
        r.folds.push_back(f);

        all_trades.insert(all_trades.end(), br.trades.begin(), br.trades.end());
    }

    // Pooled metrics over the concatenated trades, with balance rebuilt as a
    // single continuous path so drawdown spans fold boundaries.
    std::vector<EquityPoint> equity;
    equity.reserve(all_trades.size() + 1);
    double balance = cfg.initial_balance;
    equity.push_back(EquityPoint{all_trades.empty() ? 0 : all_trades.front().entry_ts, balance,
                                 balance});
    for (const Trade& t : all_trades) {
        balance += t.net_usd;
        equity.push_back(EquityPoint{t.exit_ts, balance, balance});
    }
    r.pooled = Metrics::compute(all_trades, equity, cfg.initial_balance, cfg.tf);

    // Distribution across folds.
    std::vector<double> pfs;
    std::size_t         profitable = 0;
    for (const Fold& f : r.folds) {
        if (f.metrics.trades < wf.min_trades_per_fold) continue;
        ++r.folds_counted;
        if (f.metrics.net_profit > 0.0) ++profitable;
        // Metrics reports profit_factor 0 when there were no losing trades at
        // all, which would drag a median down rather than up. Count those
        // separately instead of pretending they are the worst folds.
        if (f.metrics.gross_loss > 0.0) {
            pfs.push_back(f.metrics.profit_factor);
        } else if (f.metrics.trades > 0) {
            ++r.folds_without_losses;
        }
    }
    if (!pfs.empty()) {
        std::sort(pfs.begin(), pfs.end());
        r.median_profit_factor = pfs[pfs.size() / 2];
        r.worst_profit_factor = pfs.front();
    }
    if (r.folds_counted > 0) {
        r.frac_folds_profitable =
            static_cast<double>(profitable) / static_cast<double>(r.folds_counted);
    }

    r.wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    return r;
}

std::string WalkForwardResult::fold_table() const {
    std::string out =
        "  from        to          trades   net USD      PF     maxDD%\n"
        "  ----------------------------------------------------------------\n";
    char line[192];
    for (const Fold& f : folds) {
        std::snprintf(line, sizeof(line), "  %-11s %-11s %6zu  %9.2f  %6.3f  %8.2f\n",
                      ymd(f.from_us).c_str(), ymd(f.to_us).c_str(), f.metrics.trades,
                      f.metrics.net_profit, f.metrics.profit_factor,
                      f.metrics.max_drawdown_pct);
        out += line;
    }
    return out;
}

std::string WalkForwardResult::summary() const {
    char buf[900];
    std::snprintf(buf, sizeof(buf),
                  "%s\n"
                  "  folds            %zu  (%zu counted, %zu with no losing trade)\n"
                  "  pooled trades    %zu\n"
                  "  pooled net       %.2f USD   PF %.3f   expectancy %.4f\n"
                  "  pooled maxDD     %.2f%%  (trade-level)\n"
                  "  fold PF          median %.3f   worst %.3f\n"
                  "  folds profitable %.0f%%\n"
                  "  wall             %.2f s",
                  strategy_name.c_str(), folds.size(), folds_counted, folds_without_losses,
                  pooled.trades, pooled.net_profit, pooled.profit_factor,
                  pooled.expectancy_usd, pooled.max_drawdown_pct, median_profit_factor,
                  worst_profit_factor, frac_folds_profitable * 100.0, wall_seconds);
    return buf;
}

}  // namespace xau
