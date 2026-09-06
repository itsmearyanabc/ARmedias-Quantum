// Which strategy works in which market, measured.
//
// The D1 sweep found medians above 1.0 sitting underneath pooled figures below
// it -- the typical fold pays and a few folds take it all back. This tool asks
// whether those bad folds share a market condition, because if they do, the
// answer is not a better signal. It is declining to trade in that condition.
//
// Every trade is attributed to the regime that was classifiable AT ITS ENTRY,
// from bars that had already closed. Labelling a trade with the regime it
// turned out to be in would produce a beautiful table and a useless one.
//
//   regime_report [dir] [symbol] [--tf D1] [--lots X] [--min-trades N]

#include "xau/bar.hpp"
#include "xau/engine.hpp"
#include "xau/regime.hpp"
#include "xau/registry.hpp"
#include "xau/tick_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

using namespace xau;

namespace {

struct Cell {
    int    trades = 0;
    double gross_win = 0.0;
    double gross_loss = 0.0;   // positive magnitude
    double net = 0.0;

    [[nodiscard]] double profit_factor() const noexcept {
        return gross_loss > 0.0 ? gross_win / gross_loss : (gross_win > 0.0 ? 99.0 : 0.0);
    }
    [[nodiscard]] double expectancy() const noexcept {
        return trades > 0 ? net / trades : 0.0;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string dir = "data/ticks/real/XAUUSD";
    std::string symbol = "XAUUSD";
    Timeframe   tf = Timeframe::D1;
    double      lots = 0.01;
    int         min_trades = 30;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--lots" && i + 1 < argc) {
            lots = std::atof(argv[++i]);
        } else if (a == "--min-trades" && i + 1 < argc) {
            min_trades = std::atoi(argv[++i]);
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

    try {
        const TickStore store = TickStore::open(dir, symbol);
        std::printf("store    %s (%s)\n", dir.c_str(), symbol.c_str());
        std::printf("bars     %s\n\n", timeframe_name(tf));

        const BarSeries series =
            BarSeries::build(store, tf, store.first_ts(), store.last_ts() + 1);
        const std::span<const Bar> bars = series.closed();

        // Pre-classify every bar once. Each classification is from the prefix
        // ending at that bar, so it is exactly what a live system would have
        // known at that moment.
        std::vector<Regime> regimes;
        std::vector<TimeUs> closes;
        regimes.reserve(bars.size());
        closes.reserve(bars.size());
        for (std::size_t i = 0; i < bars.size(); ++i) {
            regimes.push_back(classify(bars.subspan(0, i + 1)));
            closes.push_back(bars[i].close_time_us(tf));
        }

        // How much of the decade each regime accounts for, so a strategy that
        // only shines in a rare regime is visibly a narrow bet.
        std::vector<int> regime_bars(Regime::kCount, 0);
        for (const Regime& r : regimes) ++regime_bars[r.index()];
        std::printf("regime mix over %zu bars\n", bars.size());
        for (std::size_t k = 0; k < Regime::kCount; ++k) {
            std::printf("  %-14s %5d bars  %5.1f%%\n", regime_name_at(k).data(), regime_bars[k],
                        100.0 * regime_bars[k] / static_cast<double>(bars.size()));
        }
        std::printf("\n");

        BacktestConfig cfg;
        cfg.spec = SymbolSpec::for_symbol(symbol);
        cfg.tf = tf;
        cfg.initial_balance = 10'000.0;
        cfg.costs.slip_base_pts = 15.0;
        cfg.costs.slip_vol_coef = 0.05;
        cfg.costs.latency_us = 150'000;
        cfg.costs.commission_per_lot_round_usd = 7.0;

        std::printf("%-22s", "strategy");
        for (std::size_t k = 0; k < Regime::kCount; ++k) {
            std::printf(" %13s", regime_name_at(k).data());
        }
        std::printf("\n%s\n", std::string(22 + 14 * Regime::kCount, '-').c_str());

        // best[regime] = (expectancy, strategy name)
        std::vector<std::pair<double, std::string>> best(
            Regime::kCount, {-1e18, std::string("-")});

        for (const BaselineEntry& b : baseline_registry()) {
            std::unique_ptr<Strategy> strat = b.make(lots);
            const BacktestResult      br = BacktestEngine(store, cfg).run(*strat);
            if (br.trades.empty()) continue;

            std::vector<Cell> cells(Regime::kCount);
            std::size_t       cursor = 0;
            for (const Trade& t : br.trades) {
                // Last bar to have CLOSED at or before the entry.
                while (cursor + 1 < closes.size() && closes[cursor + 1] <= t.entry_ts) ++cursor;
                if (closes[cursor] > t.entry_ts) continue;

                Cell& c = cells[regimes[cursor].index()];
                ++c.trades;
                c.net += t.net_usd;
                if (t.net_usd >= 0.0) c.gross_win += t.net_usd;
                else c.gross_loss += -t.net_usd;
            }

            std::printf("%-22s", b.name);
            for (std::size_t k = 0; k < Regime::kCount; ++k) {
                const Cell& c = cells[k];
                if (c.trades == 0) {
                    std::printf(" %13s", "-");
                } else {
                    std::printf(" %6.2f/%5d", c.profit_factor(), c.trades);
                    if (c.trades >= min_trades && c.expectancy() > best[k].first) {
                        best[k] = {c.expectancy(), b.name};
                    }
                }
            }
            std::printf("\n");
        }

        std::printf("\n(profit factor / trade count, per regime)\n");
        std::printf("\nbest strategy per regime, at >= %d trades\n", min_trades);
        bool any = false;
        for (std::size_t k = 0; k < Regime::kCount; ++k) {
            if (best[k].first <= -1e17) {
                std::printf("  %-14s no strategy with enough trades\n",
                            regime_name_at(k).data());
                continue;
            }
            any = true;
            std::printf("  %-14s %-22s expectancy %+8.4f USD/trade\n", regime_name_at(k).data(),
                        best[k].second.c_str(), best[k].first);
        }

        if (any) {
            std::printf(
                "\nA positive cell here is not yet a strategy. It is a hypothesis\n"
                "that has not been penalised for the number of regimes searched --\n"
                "six regimes times twelve strategies is seventy-two draws, and the\n"
                "best of seventy-two looks good by construction. Phase 6's deflated\n"
                "Sharpe exists to charge for exactly that, and until it runs these\n"
                "numbers are a shortlist rather than a result.\n");
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
