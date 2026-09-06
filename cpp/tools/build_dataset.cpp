// Phase 4 output: the training table for the Phase 5 meta-label model.
//
// One row per signal the PRIMARY strategy fired. The primary decides the side;
// this tool records what the market did next and what was knowable at the time.
// The secondary model's job is to look at the features and predict the
// meta-label -- "would taking this trade have paid?" -- so it can decline the
// ones that would not.
//
// The Phase 3 gate measured why that matters: the baselines carry about 0.06
// USD of gross edge per trade against 0.68 USD of cost. A model that keeps the
// best tenth of the signals has to find edge roughly ten times the average
// concentrated there. This table is what tells us whether it is.
//
//   build_dataset [dir] [symbol] [--strategy Name] [--tf M15] [--out file.csv]
//                 [--profit-atr X] [--stop-atr X] [--hold-hours N] [--holdout-from YYYY]

#include "xau/bar.hpp"
#include "xau/engine.hpp"
#include "xau/features.hpp"
#include "xau/labels.hpp"
#include "xau/registry.hpp"
#include "xau/tick_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <memory>
#include <string>
#include <filesystem>
#include <vector>

using namespace xau;

namespace {

int year_of(TimeUs us) {
    const std::time_t tt = static_cast<std::time_t>(us / 1'000'000);
    std::tm           tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    return tm.tm_year + 1900;
}

const char* barrier_name(Barrier b) {
    switch (b) {
        case Barrier::Profit: return "profit";
        case Barrier::Stop: return "stop";
        case Barrier::Time: return "time";
        case Barrier::None: break;
    }
    return "none";
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = "data/ticks/real/XAUUSD";
    std::string symbol = "XAUUSD";
    std::string strategy = "London";
    std::string out_path = "data/datasets/meta.csv";
    Timeframe   tf = Timeframe::M15;
    double      lots = 0.01;
    LabelConfig lcfg;
    // Section 8 requires an untouched holdout carved off on day one. Rows at or
    // after this year are written with holdout=1 so the trainer can drop them
    // without a second tool needing to know the rule.
    int holdout_from = 2024;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--strategy" && i + 1 < argc) {
            strategy = argv[++i];
        } else if (a == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (a == "--profit-atr" && i + 1 < argc) {
            lcfg.profit_atr = std::atof(argv[++i]);
        } else if (a == "--stop-atr" && i + 1 < argc) {
            lcfg.stop_atr = std::atof(argv[++i]);
        } else if (a == "--hold-hours" && i + 1 < argc) {
            lcfg.max_hold_us = static_cast<TimeUs>(std::atoi(argv[++i])) * 3'600'000'000LL;
        } else if (a == "--holdout-from" && i + 1 < argc) {
            holdout_from = std::atoi(argv[++i]);
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
        std::printf("store      %s (%s)  %llu ticks\n", dir.c_str(), symbol.c_str(),
                    static_cast<unsigned long long>(store.total_ticks()));

        // --- primary: run it once to harvest its signals ---------------------
        const BaselineEntry* entry = nullptr;
        for (const BaselineEntry& b : baseline_registry()) {
            const std::string name = b.name;
            if (name.rfind(strategy, 0) == 0) { entry = &b; break; }
        }
        if (entry == nullptr) {
            std::fprintf(stderr, "unknown strategy: %s\n", strategy.c_str());
            return 2;
        }

        BacktestConfig cfg;
        cfg.spec = SymbolSpec::xauusd_default();
        cfg.tf = tf;
        cfg.initial_balance = 10'000.0;
        cfg.costs.slip_base_pts = 15.0;
        cfg.costs.slip_vol_coef = 0.05;
        cfg.costs.latency_us = 150'000;
        cfg.costs.commission_per_lot_round_usd = 7.0;

        std::unique_ptr<Strategy> strat = entry->make(lots);
        const BacktestResult      br = BacktestEngine(store, cfg).run(*strat);
        std::printf("primary    %s -> %zu signals\n", entry->name, br.trades.size());
        if (br.trades.empty()) {
            std::fprintf(stderr, "no signals; nothing to label\n");
            return 1;
        }

        // --- bars and features, once for the whole span ----------------------
        const BarSeries series =
            BarSeries::build(store, tf, store.first_ts(), store.last_ts() + 1);
        const std::span<const Bar> bars = series.closed();
        const FeatureMatrix        fm = compute_features(bars, tf);
        std::printf("bars       %zu %s bars, %zu feature rows\n", bars.size(),
                    timeframe_name(tf), fm.size());

        // --- events: the primary's entries -----------------------------------
        std::vector<TimeUs> events;
        std::vector<Side>   sides;
        std::vector<Points> atrs;
        std::vector<std::size_t> feat_idx;

        events.reserve(br.trades.size());
        std::size_t cursor = 0;
        for (const Trade& t : br.trades) {
            // The last bar whose CLOSE is at or before the signal. Anything
            // later is not yet knowable, and taking bars[i] by position rather
            // than by close time is how an off-by-one leak gets in.
            while (cursor + 1 < fm.size() && fm.bar_close_us[cursor + 1] <= t.entry_ts) {
                ++cursor;
            }
            if (cursor >= fm.size() || !fm.valid[cursor]) continue;
            if (fm.bar_close_us[cursor] > t.entry_ts) continue;

            // ATR in points from the same causal prefix the features saw.
            const std::size_t lo = cursor >= 14 ? cursor - 14 : 0;
            double            tr = 0.0;
            std::size_t       n = 0;
            for (std::size_t k = lo + 1; k <= cursor; ++k) {
                const double hi_ = static_cast<double>(bars[k].high);
                const double lo_ = static_cast<double>(bars[k].low);
                const double pc = static_cast<double>(bars[k - 1].close);
                tr += std::max({hi_ - lo_, std::abs(hi_ - pc), std::abs(lo_ - pc)});
                ++n;
            }
            const auto atr = static_cast<Points>(n > 0 ? tr / static_cast<double>(n) : 0.0);
            if (atr <= 0) continue;

            events.push_back(t.entry_ts);
            sides.push_back(t.side);
            atrs.push_back(atr);
            feat_idx.push_back(cursor);
        }
        std::printf("events     %zu labelled from %zu trades\n", events.size(),
                    br.trades.size());

        // --- labels ----------------------------------------------------------
        const std::vector<Label> labels = label_events(store, events, sides, atrs, lcfg);

        // --- write -----------------------------------------------------------
        std::filesystem::create_directories(
            std::filesystem::path(out_path).parent_path());
        std::FILE* f = std::fopen(out_path.c_str(), "wb");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
            return 1;
        }

        // entry_us and touch_us are not decoration: purged K-fold cannot run
        // without them. Purging means dropping training rows whose LABEL
        // WINDOW overlaps the test fold, and the window is exactly
        // [entry_us, touch_us]. A dataset without them can only be split
        // naively, which leaks across every fold boundary.
        std::fprintf(f,
                     "event_us,entry_us,touch_us,year,holdout,side,barrier,"
                     "ret_atr,meta,uniqueness,weight");
        for (std::size_t k = 0; k < kFeatureCount; ++k) {
            std::fprintf(f, ",%s",
                         std::string(feature_name(static_cast<Feat>(k))).c_str());
        }
        std::fprintf(f, "\n");

        int wins = 0, stops = 0, times = 0, holdout_rows = 0;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const Label& l = labels[i];
            if (l.hit == Barrier::None) continue;
            const int yr = year_of(l.event_us);
            const int ho = yr >= holdout_from ? 1 : 0;
            holdout_rows += ho;
            if (l.hit == Barrier::Profit) ++wins;
            else if (l.hit == Barrier::Stop) ++stops;
            else ++times;

            std::fprintf(f, "%lld,%lld,%lld,%d,%d,%d,%s,%.6f,%d,%.6f,%.6f",
                         static_cast<long long>(l.event_us),
                         static_cast<long long>(l.entry_us),
                         static_cast<long long>(l.touch_us), yr, ho, sign_of(l.side),
                         barrier_name(l.hit), l.ret_atr, l.meta(), l.uniqueness, l.weight);
            const FeatureRow& row = fm.rows[feat_idx[i]];
            for (double v : row) std::fprintf(f, ",%.6f", v);
            std::fprintf(f, "\n");
        }
        std::fclose(f);

        const int total = wins + stops + times;
        std::printf("labels     %d rows: %d profit, %d stop, %d time\n", total, wins, stops,
                    times);
        if (total > 0) {
            std::printf("meta rate  %.1f%% of signals would have paid\n",
                        100.0 * static_cast<double>(wins) / static_cast<double>(total));
        }
        std::printf("holdout    %d rows from %d onward, flagged not dropped\n", holdout_rows,
                    holdout_from);
        std::printf("wrote      %s\n", out_path.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
