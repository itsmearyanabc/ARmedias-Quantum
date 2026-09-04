// Phase 1 gate: a ten-year backtest must complete in under 20 seconds.
//
// CI does not hold ten years of ticks, so the gate is expressed as a rate that
// implies it. Ten years of XAUUSD is roughly 300M ticks (docs/PLAN.md section
// 3), so 300M / 20s = 15M ticks/s. Gating on the rate makes the criterion
// independent of how much data happens to be on the machine.
//
//   bench_backtest [dir] [symbol] [min_ticks_per_sec]

#include "xau/baselines.hpp"
#include "xau/engine.hpp"
#include "xau/tick_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

using namespace xau;

namespace {

constexpr double kTicksInADecade = 300e6;

BacktestConfig realistic_config() {
    BacktestConfig cfg;
    cfg.spec = SymbolSpec::xauusd_default();
    cfg.tf = Timeframe::M15;
    cfg.initial_balance = 10'000.0;
    // Costs a retail gold account would actually see.
    cfg.costs.slip_base_pts = 15.0;
    cfg.costs.slip_vol_coef = 0.05;
    cfg.costs.latency_us = 150'000;
    cfg.costs.commission_per_lot_round_usd = 7.0;
    return cfg;
}

void report(const char* label, const BacktestResult& r) {
    const double tps = r.stats.wall_seconds > 0.0
                           ? static_cast<double>(r.stats.ticks) / r.stats.wall_seconds
                           : 0.0;
    std::printf("  %-16s %7.3f s   %8.2f M ticks/s   %6llu trades   decade %6.1f s\n", label,
                r.stats.wall_seconds, tps / 1e6,
                static_cast<unsigned long long>(r.trades.size()),
                tps > 0.0 ? kTicksInADecade / tps : 0.0);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "data/ticks/XAUUSD";
    const std::string symbol = (argc > 2) ? argv[2] : "XAUUSD";
    const double      floor_tps = (argc > 3) ? std::atof(argv[3]) : 15e6;

    try {
        const TickStore store = TickStore::open(dir, symbol);
        std::printf("store   %s  (%llu ticks over %zu months)\n\n", dir.c_str(),
                    static_cast<unsigned long long>(store.total_ticks()), store.file_count());

        const BacktestConfig cfg = realistic_config();

        // Almost no per-tick strategy work: the engine's own floor.
        BuyAndHold           bh(0.01, Side::Long);
        const BacktestResult r_bh = BacktestEngine(store, cfg).run(bh);
        report("buy and hold", r_bh);

        // Many entries and exits, so stops, fills and accounting are all
        // exercised — closer to what a real strategy costs.
        RandomEntry::Config rc;
        rc.entry_prob = 0.10;
        rc.hold_bars = 6;
        rc.lots = 0.01;
        RandomEntry          rnd(rc);
        const BacktestResult r_rnd = BacktestEngine(store, cfg).run(rnd);
        report("random entry", r_rnd);

        std::printf("\n%s\n", r_rnd.summary().c_str());

        // Gate on the busier run: the floor should hold when the engine is
        // actually doing something, not only when it is idling.
        const double tps = r_rnd.stats.wall_seconds > 0.0
                               ? static_cast<double>(r_rnd.stats.ticks) / r_rnd.stats.wall_seconds
                               : 0.0;
        const bool pass = tps >= floor_tps;
        std::printf("\ngate    >= %.1f M ticks/s : %s (%.2f M, a decade in %.1f s)\n",
                    floor_tps / 1e6, pass ? "PASS" : "FAIL", tps / 1e6,
                    tps > 0.0 ? kTicksInADecade / tps : 0.0);

        // A null model that made money would mean the cost model is not being
        // applied on this path at all.
        if (r_rnd.metrics.net_profit >= 0.0) {
            std::printf("\nWARNING: the null model did not lose money. The cost model is "
                        "not reaching the fill path.\n");
            return 1;
        }
        return pass ? 0 : 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "bench_backtest: %s\n", e.what());
        std::fprintf(stderr,
                     "\nNo store yet? Generate synthetic ticks first:\n"
                     "  python -m xau_ingest.synth --months 12 --out data/ticks/XAUUSD\n");
        return 2;
    }
}
