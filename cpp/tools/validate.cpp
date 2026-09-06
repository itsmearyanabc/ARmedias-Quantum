// Phase 6 gate: does the candidate survive being charged for the search?
//
// Rsi2InRange clears PF 1.05 after doubled costs on walk-forward. It was also
// found after trying fourteen strategies across four timeframes and six
// regimes. Those two sentences are both true, and the second is the reason the
// first is not yet evidence.
//
// This tool prices the search. It runs every registered strategy over the same
// blocks of history, computes the Sharpe distribution, deflates the best one
// for the number of trials and the shape of its returns, and measures how often
// picking the in-sample winner would have picked wrongly.
//
//   validate [dir] [symbol] [--tf D1] [--blocks 10] [--lots X]

#include "xau/bar.hpp"
#include "xau/engine.hpp"
#include "xau/registry.hpp"
#include "xau/tick_store.hpp"
#include "xau/validation.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

using namespace xau;

int main(int argc, char** argv) {
    std::string dir = "data/ticks/real/XAUUSD";
    std::string symbol = "XAUUSD";
    Timeframe   tf = Timeframe::D1;
    double      lots = 0.01;
    std::size_t blocks = 10;   // even, and C(10,5)=252 splits
    // The honest trial count is not the number of strategies this run happens
    // to report. It is every configuration ever evaluated on this data: 14
    // strategies across 4 timeframes, plus the regime variants. Counting only
    // the survivors is precisely the bias the deflated Sharpe exists to remove,
    // so undercounting here quietly reintroduces it.
    std::size_t trials_override = 0;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--lots" && i + 1 < argc) {
            lots = std::atof(argv[++i]);
        } else if (a == "--trials" && i + 1 < argc) {
            trials_override = static_cast<std::size_t>(std::atoi(argv[++i]));
        } else if (a == "--blocks" && i + 1 < argc) {
            blocks = static_cast<std::size_t>(std::atoi(argv[++i]));
        } else if (a == "--tf" && i + 1 < argc) {
            const std::string want = argv[++i];
            for (int k = 0; k < static_cast<int>(Timeframe::COUNT); ++k) {
                const auto cand = static_cast<Timeframe>(k);
                if (want == timeframe_name(cand)) tf = cand;
            }
        } else if (!a.empty() && a[0] != '-') {
            if (positional == 0) dir = a;
            else if (positional == 1) symbol = a;
            ++positional;
        }
    }
    if (blocks % 2 != 0) ++blocks;   // CSCV needs to halve them

    try {
        const TickStore store = TickStore::open(dir, symbol);
        const TimeUs    t0 = store.first_ts();
        const TimeUs    t1 = store.last_ts();
        const TimeUs    span = (t1 - t0) / static_cast<TimeUs>(blocks);

        std::printf("store    %s (%s)\n", dir.c_str(), symbol.c_str());
        std::printf("bars     %s, %zu blocks\n\n", timeframe_name(tf), blocks);

        BacktestConfig cfg;
        cfg.spec = SymbolSpec::for_symbol(symbol);
        cfg.tf = tf;
        cfg.initial_balance = 10'000.0;
        cfg.costs.slip_base_pts = 15.0;
        cfg.costs.slip_vol_coef = 0.05;
        cfg.costs.latency_us = 150'000;
        cfg.costs.commission_per_lot_round_usd = 7.0;

        struct Row {
            std::string         name;
            std::vector<double> per_block;    // net USD in each block
            std::vector<double> trade_rets;   // per-trade net, for moments
            double              sharpe = 0.0;
            int                 trades = 0;
            double              net = 0.0;
        };
        std::vector<Row> rows;

        for (const BaselineEntry& b : baseline_registry()) {
            std::unique_ptr<Strategy> strat = b.make(lots);
            const BacktestResult      br = BacktestEngine(store, cfg).run(*strat);
            if (br.trades.size() < 20) continue;   // nothing to say about a handful

            Row r;
            r.name = b.name;
            r.per_block.assign(blocks, 0.0);
            r.trades = static_cast<int>(br.trades.size());
            for (const Trade& t : br.trades) {
                auto k = static_cast<std::size_t>((t.entry_ts - t0) / (span > 0 ? span : 1));
                if (k >= blocks) k = blocks - 1;
                r.per_block[k] += t.net_usd;
                r.trade_rets.push_back(t.net_usd);
                r.net += t.net_usd;
            }
            r.sharpe = sharpe_ratio(r.trade_rets);
            rows.push_back(std::move(r));
        }

        if (rows.size() < 2) {
            std::fprintf(stderr, "need at least two strategies with trades\n");
            return 1;
        }

        // --- trial statistics ------------------------------------------------
        std::vector<double> srs;
        srs.reserve(rows.size());
        for (const Row& r : rows) srs.push_back(r.sharpe);
        const Moments sm = moments_of(srs);

        std::printf("%-22s %7s %10s %9s %9s\n", "strategy", "trades", "net USD", "SR/trade",
                    "skew");
        std::printf("%s\n", std::string(62, '-').c_str());
        for (const Row& r : rows) {
            const Moments tm = moments_of(r.trade_rets);
            std::printf("%-22s %7d %10.2f %9.4f %9.2f\n", r.name.c_str(), r.trades, r.net,
                        r.sharpe, tm.skew);
        }

        const auto best_it = std::max_element(
            rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.sharpe < b.sharpe; });
        const Row&    best = *best_it;
        const Moments bm = moments_of(best.trade_rets);

        // --- deflated Sharpe -------------------------------------------------
        // n_trials is deliberately the number of strategies TRIED, not the
        // number reported. Counting only the survivors is how a search gets to
        // look like a single lucky guess.
        const std::size_t n_trials = trials_override > 0 ? trials_override : rows.size();
        const double      bench = expected_max_sharpe(sm.stdev * sm.stdev, n_trials);
        const double      dsr =
            deflated_sharpe(best.sharpe, bench, best.trade_rets.size(), bm.skew, bm.kurtosis);

        std::printf("\nbest by Sharpe   %s\n", best.name.c_str());
        std::printf("  SR/trade       %.4f  over %d trades\n", best.sharpe, best.trades);
        std::printf("  skew %.2f  kurtosis %.2f  (normal is 0.00 / 3.00)\n", bm.skew,
                    bm.kurtosis);
        std::printf("  trials         %zu   SR spread across them %.4f\n", n_trials, sm.stdev);
        std::printf("  benchmark      %.4f  <- the Sharpe %zu zero-edge strategies would\n",
                    bench, n_trials);
        std::printf("                         produce by luck alone\n");
        std::printf("  DSR            %.4f\n", dsr);

        // --- PBO -------------------------------------------------------------
        std::vector<std::vector<double>> perf;
        perf.reserve(rows.size());
        for (const Row& r : rows) perf.push_back(r.per_block);
        const PboResult pbo = probability_of_backtest_overfitting(perf);

        std::printf("\nPBO over %zu symmetric splits\n", pbo.splits);
        std::printf("  PBO            %.3f  <- how often the in-sample winner lands\n", pbo.pbo);
        std::printf("                         below median out of sample\n");
        if (!pbo.oos_of_is_best.empty()) {
            std::vector<double> v = pbo.oos_of_is_best;
            std::sort(v.begin(), v.end());
            std::printf("  OOS of winner  median %+.2f USD   worst %+.2f   best %+.2f\n",
                        v[v.size() / 2], v.front(), v.back());
        }

        // --- gate ------------------------------------------------------------
        const bool dsr_ok = dsr > 0.95;
        const bool pbo_ok = pbo.pbo < 0.30;
        std::printf("\nPhase 6 gate\n");
        std::printf("  DSR > 0.95     %-5s (%.4f)\n", dsr_ok ? "PASS" : "FAIL", dsr);
        std::printf("  PBO < 0.30     %-5s (%.3f)\n", pbo_ok ? "PASS" : "FAIL", pbo.pbo);
        std::printf("  overall        %s\n", (dsr_ok && pbo_ok) ? "PASS" : "FAIL");

        if (!dsr_ok || !pbo_ok) {
            std::printf(
                "\nA failure here does not mean the strategy loses money. It means\n"
                "the evidence does not yet separate it from the best of %zu tries,\n"
                "which is a statement about how much has been searched rather than\n"
                "about the market. More data or fewer trials both move it; picking\n"
                "a different winner does not.\n",
                n_trials);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
