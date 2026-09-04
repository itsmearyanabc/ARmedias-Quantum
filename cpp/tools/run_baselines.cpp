// Phase 3 gate report: run every rule baseline on walk-forward, at normal and
// at doubled costs.
//
// The gate the plan sets is "at least one shows PF > 1.05 after 2x costs on
// walk-forward". If none do, the instruction is to stop and revisit the
// timeframe rather than reach for a bigger model — the problem would be the
// cost/timeframe combination, and no amount of gradient boosting fixes that.
//
// This tool refuses to pretend synthetic ticks can answer that question. The
// generator is a driftless random walk by construction, so a run against it
// measures the cost model, not an edge.
//
//   run_baselines [dir] [symbol] [--fold-days N] [--lots X] [--require-gate]

#include "xau/engine.hpp"
#include "xau/rules.hpp"
#include "xau/session.hpp"
#include "xau/tick_store.hpp"
#include "xau/walk_forward.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace xau;

namespace {

constexpr double kGatePf = 1.05;

// Samples tick flags. The synthetic generator stamps TF_SYNTHETIC on every
// tick it writes, which is the only reliable way to tell a store apart from
// real history after the fact.
bool looks_synthetic(const TickStore& s) {
    for (const TickFile& f : s.files()) {
        const std::span<const Tick> t = f.ticks();
        if (t.empty()) continue;
        const std::size_t n = std::min<std::size_t>(t.size(), 1000);
        for (std::size_t i = 0; i < n; ++i) {
            if (t[i].flags & TF_SYNTHETIC) return true;
        }
        return false;
    }
    return false;
}

std::string ymd(TimeUs us) {
    const std::time_t tt = static_cast<std::time_t>(us / 1'000'000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

BacktestConfig base_config() {
    BacktestConfig c;
    c.spec = SymbolSpec::xauusd_default();
    c.tf = Timeframe::M15;
    c.initial_balance = 10'000.0;
    // Costs a retail gold account actually sees. Spread always comes from the
    // tick; these are what is added on top.
    c.costs.slip_base_pts = 15.0;
    c.costs.slip_vol_coef = 0.05;
    c.costs.latency_us = 150'000;
    c.costs.commission_per_lot_round_usd = 7.0;
    return c;
}

struct Baseline {
    const char*   name;
    StrategyFactory make;
};

std::vector<Baseline> baselines(double lots) {
    return {
        {"LondonOpeningRange",
         [lots] {
             LondonOpeningRange::Config c;
             c.lots = lots;
             return std::make_unique<LondonOpeningRange>(c);
         }},
        {"AsiaRangeBreakout",
         [lots] {
             AsiaRangeBreakout::Config c;
             c.lots = lots;
             return std::make_unique<AsiaRangeBreakout>(c);
         }},
        {"TrendPullback",
         [lots] {
             TrendPullback::Config c;
             c.lots = lots;
             return std::make_unique<TrendPullback>(c);
         }},
        {"VolatilityCompression",
         [lots] {
             VolatilityCompression::Config c;
             c.lots = lots;
             return std::make_unique<VolatilityCompression>(c);
         }},
    };
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = "data/ticks/XAUUSD";
    std::string symbol = "XAUUSD";
    int         fold_days = 90;
    double      lots = 0.01;
    bool        require_gate = false;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fold-days" && i + 1 < argc) {
            fold_days = std::atoi(argv[++i]);
        } else if (a == "--lots" && i + 1 < argc) {
            lots = std::atof(argv[++i]);
        } else if (a == "--require-gate") {
            require_gate = true;
        } else if (!a.empty() && a[0] != '-') {
            if (positional == 0) dir = a;
            else if (positional == 1) symbol = a;
            ++positional;
        }
    }

    try {
        const TickStore store = TickStore::open(dir, symbol);
        const bool      synthetic = looks_synthetic(store);

        std::printf("store    %s (%s)\n", dir.c_str(), symbol.c_str());
        std::printf("span     %s .. %s   %llu ticks\n", ymd(store.first_ts()).c_str(),
                    ymd(store.last_ts()).c_str(),
                    static_cast<unsigned long long>(store.total_ticks()));
        std::printf("folds    %d-day walk-forward, non-overlapping\n\n", fold_days);

        if (synthetic) {
            std::printf(
                "  ##############################################################\n"
                "  #  SYNTHETIC DATA. The gate below is NOT meaningful.         #\n"
                "  #                                                            #\n"
                "  #  These ticks come from a driftless random walk built as    #\n"
                "  #  the null hypothesis for the Phase 4 leakage tests. There  #\n"
                "  #  is no edge in them to find, by construction. What follows #\n"
                "  #  measures the cost model and that the strategies run.      #\n"
                "  #                                                            #\n"
                "  #  Ingest real Dukascopy history before reading anything     #\n"
                "  #  into these numbers.                                       #\n"
                "  ##############################################################\n\n");
        }

        WalkForwardConfig wf;
        wf.test_span_us = static_cast<TimeUs>(fold_days) * kUsPerDay;
        wf.min_trades_per_fold = 1;

        const BacktestConfig cfg = base_config();
        const BacktestConfig cfg2x = [&] {
            BacktestConfig c = cfg;
            c.costs = c.costs.stressed(2.0);
            return c;
        }();

        std::printf("%-22s %6s %7s %11s %7s %7s %7s | %7s %11s\n", "strategy", "folds",
                    "trades", "net USD", "PF", "medPF", "fold+%", "PF@2x", "net@2x");
        std::printf("%s\n", std::string(100, '-').c_str());

        bool   gate_pass = false;
        double best_pf_2x = 0.0;

        for (const Baseline& b : baselines(lots)) {
            const WalkForwardResult r1 = run_walk_forward(store, cfg, b.make, wf);
            const WalkForwardResult r2 = run_walk_forward(store, cfg2x, b.make, wf);

            std::printf("%-22s %6zu %7zu %11.2f %7.3f %7.3f %6.0f%% | %7.3f %11.2f\n", b.name,
                        r1.folds.size(), r1.pooled.trades, r1.pooled.net_profit,
                        r1.pooled.profit_factor, r1.median_profit_factor,
                        r1.frac_folds_profitable * 100.0, r2.pooled.profit_factor,
                        r2.pooled.net_profit);

            best_pf_2x = std::max(best_pf_2x, r2.pooled.profit_factor);
            if (r2.pooled.profit_factor > kGatePf && r2.pooled.trades > 0) gate_pass = true;
        }

        std::printf("\ngate     PF > %.2f after 2x costs : %s (best %.3f)\n", kGatePf,
                    gate_pass ? "PASS" : "FAIL", best_pf_2x);

        if (synthetic) {
            std::printf(
                "\nverdict  NOT EVALUATED - synthetic data cannot answer this.\n"
                "         A pass here would mean the harness is broken, not that\n"
                "         an edge exists.\n");
            return 0;
        }
        if (!gate_pass) {
            std::printf(
                "\nverdict  The plan says stop here rather than proceed to Phase 4.\n"
                "         If nothing clears the bar after 2x costs, the problem is the\n"
                "         cost/timeframe combination, not the model, and a bigger model\n"
                "         will not fix it. Revisit the horizon first.\n");
        }
        return (require_gate && !gate_pass) ? 1 : 0;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "run_baselines: %s\n", e.what());
        std::fprintf(stderr,
                     "\nNo store yet? Generate synthetic ticks, or ingest real history:\n"
                     "  python -m xau_ingest.synth --months 12 --out data/ticks/XAUUSD\n"
                     "  python -m xau_ingest.dukascopy --start 2015-01 --end 2024-12 ...\n");
        return 2;
    }
}
