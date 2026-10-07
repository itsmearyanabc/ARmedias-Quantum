// One pre-registered strategy, one window, the controls that keep a trend
// result honest. See docs/RESEARCH-B.md.
//
// run_baselines answers "does anything clear the gate"; validate answers "does
// the best of everything survive the search". This answers the question both
// skip on an instrument that doubled: is the P&L the SIGNAL, or is it just
// having been long gold? It splits every figure by side and sets the long
// side against the drift that simply holding gold for the same length of time
// would have earned.
//
//   evaluate [dir] [symbol] --strategy NAME [--tf D1] [--lots X]
//            [--from YYYY-MM-DD] [--to YYYY-MM-DD] [--trade-from YYYY-MM-DD]
//            [--swap-long R] [--swap-short R]
//
// --trade-from is the holdout switch: bars from --from warm the strategy, and
// no position opens before --trade-from.

#include "xau/baselines.hpp"
#include "xau/engine.hpp"
#include "xau/registry.hpp"
#include "xau/session.hpp"
#include "xau/tick_store.hpp"
#include "xau/validation.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <vector>

using namespace xau;

namespace {

std::string ymd(TimeUs us) {
    const std::time_t tt = static_cast<std::time_t>(us / 1'000'000);
    std::tm           tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

struct Leg {
    std::size_t n = 0;
    double      gross = 0.0, swap = 0.0, net = 0.0, won = 0.0, lost = 0.0;
    double      days = 0.0;   // calendar days in the market

    void add(const Trade& t) {
        ++n;
        gross += t.gross_usd;
        swap += t.swap_usd;
        net += t.net_usd;
        (t.net_usd > 0.0 ? won : lost) += t.net_usd > 0.0 ? t.net_usd : -t.net_usd;
        days += static_cast<double>(t.duration_us()) / static_cast<double>(kUsPerDay);
    }
    [[nodiscard]] double pf() const { return lost > 0.0 ? won / lost : 0.0; }
};

void print_leg(const char* label, const Leg& l) {
    std::printf("  %-6s %5zu trades  gross %10.2f  swap %9.2f  net %10.2f  PF %6.3f  "
                "%7.0f days\n",
                label, l.n, l.gross, l.swap, l.net, l.pf(), l.days);
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = "data/ticks/XAUUSD";
    std::string symbol = "XAUUSD";
    std::string strategy;
    Timeframe   tf = Timeframe::D1;
    double      lots = 0.01;
    Financing   fin;
    TimeUs      from_us = 0, to_us = 0, trade_from_us = 0;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--strategy" && i + 1 < argc) {
            strategy = argv[++i];
        } else if (a == "--lots" && i + 1 < argc) {
            lots = std::atof(argv[++i]);
        } else if (a == "--swap-long" && i + 1 < argc) {
            fin.long_annual = std::atof(argv[++i]);
        } else if (a == "--swap-short" && i + 1 < argc) {
            fin.short_annual = std::atof(argv[++i]);
        } else if (a == "--tf" && i + 1 < argc) {
            const std::string want = argv[++i];
            bool              found = false;
            for (int k = 0; k < static_cast<int>(Timeframe::COUNT); ++k) {
                if (want == timeframe_name(static_cast<Timeframe>(k))) {
                    tf = static_cast<Timeframe>(k);
                    found = true;
                }
            }
            if (!found) {
                std::fprintf(stderr, "unknown timeframe: %s\n", want.c_str());
                return 2;
            }
        } else if ((a == "--from" || a == "--to" || a == "--trade-from") && i + 1 < argc) {
            TimeUs& dst = a == "--from" ? from_us : (a == "--to" ? to_us : trade_from_us);
            if (!parse_utc_date(argv[++i], dst)) {
                std::fprintf(stderr, "bad date for %s: %s (want YYYY-MM-DD)\n", a.c_str(),
                             argv[i]);
                return 2;
            }
        } else if (!a.empty() && a[0] != '-') {
            if (positional == 0) dir = a;
            else if (positional == 1) symbol = a;
            ++positional;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    const BaselineEntry* entry = find_baseline(strategy.c_str());
    if (entry == nullptr) {
        std::fprintf(stderr, "--strategy must name a registered strategy, got '%s'\n",
                     strategy.c_str());
        return 2;
    }

    try {
        const TickStore store = TickStore::open(dir, symbol);
        const TimeUs    start = trade_from_us ? trade_from_us : (from_us ? from_us : store.first_ts());
        const TimeUs    end = to_us ? to_us : store.last_ts() + 1;
        if (end <= start || (from_us && trade_from_us && trade_from_us < from_us)) {
            std::fprintf(stderr, "empty or inverted window\n");
            return 2;
        }

        std::printf("store     %s (%s)\n", dir.c_str(), symbol.c_str());
        std::printf("strategy  %s on %s, %.2f lots\n", entry->name, timeframe_name(tf), lots);
        std::printf("trading   %s .. %s (end exclusive)", ymd(start).c_str(), ymd(end).c_str());
        if (trade_from_us) {
            std::printf(", warmed from %s",
                        ymd(from_us ? from_us : store.first_ts()).c_str());
        }
        std::printf("\nswap      long %+.2f%%  short %+.2f%% a year on notional\n\n",
                    fin.long_annual * 100.0, fin.short_annual * 100.0);

        BacktestConfig base;
        base.spec = SymbolSpec::for_symbol(symbol);
        base.tf = tf;
        base.initial_balance = 10'000.0;
        base.from_us = from_us;
        base.to_us = to_us;
        base.trade_from_us = trade_from_us;
        // The same retail costs run_baselines and validate charge.
        base.costs.slip_base_pts = 15.0;
        base.costs.slip_vol_coef = 0.05;
        base.costs.latency_us = 150'000;
        base.costs.commission_per_lot_round_usd = 7.0;

        // Buy-and-hold over exactly the trading window: the drift any long
        // position collected just by existing.
        BacktestConfig bh_cfg = base;
        bh_cfg.from_us = start;
        bh_cfg.trade_from_us = 0;
        apply_financing(bh_cfg.spec, fin, 1.0);
        BuyAndHold     bh(lots, Side::Long);
        const auto     bh_r = BacktestEngine(store, bh_cfg).run(bh);
        double         bh_gross = 0.0, bh_net = 0.0, bh_days = 0.0;
        for (const Trade& t : bh_r.trades) {
            bh_gross += t.gross_usd;
            bh_net += t.net_usd;
            bh_days += static_cast<double>(t.duration_us()) / static_cast<double>(kUsPerDay);
        }
        const double drift_per_day = bh_days > 0.0 ? bh_gross / bh_days : 0.0;
        std::printf("buy&hold  gross %10.2f  net %10.2f over %.0f days  (drift %.4f USD/day)\n\n",
                    bh_gross, bh_net, bh_days, drift_per_day);

        for (const double mult : {1.0, 2.0}) {
            BacktestConfig cfg = base;
            if (mult != 1.0) cfg.costs = cfg.costs.stressed(mult);
            apply_financing(cfg.spec, fin, mult);

            std::unique_ptr<Strategy> s = entry->make(lots);
            const BacktestResult      r = BacktestEngine(store, cfg).run(*s);

            Leg                 all, lng, sht;
            std::vector<double> rets;
            for (const Trade& t : r.trades) {
                all.add(t);
                (t.side == Side::Long ? lng : sht).add(t);
                rets.push_back(t.net_usd);
            }

            std::printf("costs x%.0f  (spread, slippage and financing)\n", mult);
            print_leg("all", all);
            print_leg("long", lng);
            print_leg("short", sht);
            const double drift = drift_per_day * lng.days;
            std::printf("  long gross beyond drift %10.2f  (drift share %.2f)\n", lng.gross - drift,
                        drift);
            // Per trade, the same statistic validate's CPCV percentiles are in.
            std::printf("  SR/trade %.4f   expectancy %.4f USD/trade   rejected %llu   "
                        "max DD %.2f%%\n\n",
                        sharpe_ratio(rets), all.n ? all.net / static_cast<double>(all.n) : 0.0,
                        static_cast<unsigned long long>(r.stats.rejected_total()),
                        r.metrics.max_drawdown_pct);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "evaluate: %s\n", e.what());
        return 1;
    }
}
