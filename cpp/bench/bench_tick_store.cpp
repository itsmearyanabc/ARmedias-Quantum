// Phase 0 gate: the tick reader must sustain >= 20M ticks/s on one core.
//
// Exits non-zero if it does not, so CI enforces the target rather than us
// remembering to look at it. See docs/PLAN.md section 3.
//
//   bench_tick_store [dir] [symbol] [min_ticks_per_sec]

#include "xau/tick_store.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <string>

using namespace xau;
using Clock = std::chrono::steady_clock;

namespace {

struct Scan {
    std::uint64_t ticks     = 0;
    std::int64_t  mid_accum = 0;   // wraps; we only need the optimiser to keep it
    Points        min_bid   = std::numeric_limits<Points>::max();
    Points        max_bid   = std::numeric_limits<Points>::min();
    std::uint64_t wide      = 0;   // spread over $0.50
};

// The hot loop. Flat walk over a contiguous span, no indirection, no virtuals.
Scan scan_all(const TickStore& store) {
    Scan s;
    store.for_each_chunk(store.first_ts(), store.last_ts() + 1,
                         [&s](std::span<const Tick> chunk) {
                             for (const Tick& t : chunk) {
                                 s.mid_accum += t.mid_half_pts();
                                 if (t.bid_pts < s.min_bid) s.min_bid = t.bid_pts;
                                 if (t.bid_pts > s.max_bid) s.max_bid = t.bid_pts;
                                 s.wide += (t.spread_pts > 500) ? 1u : 0u;
                             }
                             s.ticks += chunk.size();
                             return true;
                         });
    return s;
}

double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

void report(const char* label, const Scan& s, double secs) {
    const double ticks = static_cast<double>(s.ticks);
    const double tps   = secs > 0.0 ? ticks / secs : 0.0;
    const double gbps  = tps * static_cast<double>(sizeof(Tick)) / 1e9;
    std::printf("  %-6s %10.3f s   %8.2f M ticks/s   %6.2f GB/s\n", label, secs, tps / 1e6,
                gbps);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir    = (argc > 1) ? argv[1] : "data/ticks/XAUUSD";
    const std::string symbol = (argc > 2) ? argv[2] : "XAUUSD";
    const double      floor_tps = (argc > 3) ? std::atof(argv[3]) : 20e6;

    try {
        const TickStore store = TickStore::open(dir, symbol);

        std::printf("store   %s  (%s)\n", dir.c_str(), symbol.c_str());
        std::printf("files   %zu months, %llu ticks, %.2f GB on disk\n", store.file_count(),
                    static_cast<unsigned long long>(store.total_ticks()),
                    static_cast<double>(store.total_ticks()) *
                        static_cast<double>(sizeof(Tick)) / 1e9);
        std::printf("span    %lld .. %lld (us)\n\n",
                    static_cast<long long>(store.first_ts()),
                    static_cast<long long>(store.last_ts()));

        // Cold: page faults from disk. This is what a fresh backtest pays.
        auto       t0   = Clock::now();
        const Scan cold = scan_all(store);
        const double cold_s = seconds_since(t0);

        // Warm: page cache hot. This is the compute throughput the gate targets.
        t0 = Clock::now();
        const Scan warm = scan_all(store);
        const double warm_s = seconds_since(t0);

        report("cold", cold, cold_s);
        report("warm", warm, warm_s);

        // Keep the accumulator observable so the loop cannot be optimised away.
        std::printf("\nchecksum %lld   bid range %d .. %d pts   wide spreads %llu\n",
                    static_cast<long long>(warm.mid_accum), warm.min_bid, warm.max_bid,
                    static_cast<unsigned long long>(warm.wide));

        const double warm_tps = warm_s > 0.0 ? static_cast<double>(warm.ticks) / warm_s : 0.0;
        std::printf("\ngate    >= %.1f M ticks/s : %s (%.2f M)\n", floor_tps / 1e6,
                    warm_tps >= floor_tps ? "PASS" : "FAIL", warm_tps / 1e6);

        return warm_tps >= floor_tps ? 0 : 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "bench_tick_store: %s\n", e.what());
        std::fprintf(stderr,
                     "\nNo store yet? Generate synthetic ticks first:\n"
                     "  python -m xau_ingest.synth --months 12 --out data/ticks/XAUUSD\n");
        return 2;
    }
}
