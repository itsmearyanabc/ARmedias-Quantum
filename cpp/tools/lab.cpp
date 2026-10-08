// The strategy lab: every registered strategy, the same data, the same costs,
// one ranking -- and the evidence that keeps a ranking of hundreds honest.
//
// Ranking hundreds of strategies on one history and taking the top one is the
// textbook way to find a fluke: among enough zero-edge strategies, one always
// looks brilliant. So the lab charges for the search. A ledger remembers every
// strategy ever ranked on this data, edited versions included (a new source
// hash is a new trial), and the deflated Sharpe benchmarks each strategy
// against the best that many zero-edge tries would produce by luck. A
// champion is named only if it clears every gate; "none" is the usual, and
// correct, answer.
//
//   lab [dir] [symbol] [--tf M15] [--blocks 10] [--lots 0.10] [--from D] [--to D]
//       [--swap-long R] [--swap-short R] [--prior-trials 73] [--min-trades 100]
//       [--only NAME,NAME] [--jobs N] [--out lab] [--champion-file PATH]
//
//   lab --live JOURNAL.csv [--live MORE.csv ...] [--symbol XAUUSD] [--out lab] [--champion-file PATH]
//       [--min-live 40] [--retire-below 0.10] [--promote-above 0.90]
//
// --live reads the trade journal the MT5 EA writes and updates, per strategy,
// the probability that it has a real edge (xau/bayes.hpp). A strategy that is
// probably a loser is retired; one proven over enough live trades is promoted.
// The champion file is what XauBridgeEA trades in "@champion" mode.

#include "xau/bar.hpp"
#include "xau/bayes.hpp"
#include "xau/engine.hpp"
#include "xau/registry.hpp"
#include "xau/session.hpp"
#include "xau/tick_store.hpp"
#include "xau/validation.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace xau;
namespace fs = std::filesystem;

namespace {

// The project's gates (run_baselines Phase 3, validate Phase 6), not new ones.
constexpr double kGateDsr = 0.95;
constexpr double kGatePbo = 0.30;
constexpr double kGateStressPf = 1.05;   // at 2x spread and 2x slippage
constexpr int    kMinTradesForStats = 20;

std::string utc_now_iso() {
    const std::time_t tt = std::time(nullptr);
    std::tm           tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Temporary file, then rename. The EA may be reading the champion file at the
// moment it is replaced, and Windows refuses to replace an open file, so the
// rename is retried for a few seconds before giving up loudly.
bool write_atomic(const fs::path& path, const std::string& text) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        f.flush();
        if (!f) return false;
    }
    for (int i = 0; i < 50; ++i) {
        fs::rename(tmp, path, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::fprintf(stderr, "could not replace %s: %s\n", path.string().c_str(), ec.message().c_str());
    return false;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string              cur;
    std::istringstream       in(s);
    while (std::getline(in, cur, sep)) out.push_back(cur);
    return out;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

// --- the champion file -------------------------------------------------------

struct Champion {
    std::string name;      // empty: nobody
    std::string tf;
    std::string source;    // "lab" or "live"
    std::string reason;
    std::string evidence;  // e.g. "dsr=0.97" or "p_edge=0.93 n=52"
};

// CRLF: the EA reads it with MQL5's FileReadString, which ends a text line
// at \r\n.
std::string champion_text(const Champion& c) {
    std::ostringstream s;
    s << "xau_champion 1\r\n"
      << "name=" << c.name << "\r\n"
      << "tf=" << c.tf << "\r\n"
      << "source=" << c.source << "\r\n"
      << "evidence=" << c.evidence << "\r\n"
      << "reason=" << c.reason << "\r\n"
      << "updated_utc=" << utc_now_iso() << "\r\n";
    return s.str();
}

bool read_champion(const fs::path& p, Champion& c) {
    std::ifstream f(p);
    std::string   line;
    if (!f || !std::getline(f, line) || trim(line) != "xau_champion 1") return false;
    while (std::getline(f, line)) {
        line = trim(line);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "name") c.name = v;
        else if (k == "tf") c.tf = v;
        else if (k == "source") c.source = v;
        else if (k == "reason") c.reason = v;
        else if (k == "evidence") c.evidence = v;
    }
    return true;
}

void publish_champion(const Champion& c, const fs::path& out_file, const std::string& extra) {
    const std::string text = champion_text(c);
    bool              ok = write_atomic(out_file, text);
    if (!extra.empty()) ok = write_atomic(fs::path(extra), text) && ok;
    std::printf("\nchampion  %s%s\n", c.name.empty() ? "NONE" : c.name.c_str(),
                c.name.empty() ? "" : (" on " + c.tf).c_str());
    if (!c.reason.empty()) std::printf("  why     %s\n", c.reason.c_str());
    std::printf("  written %s%s%s\n", out_file.string().c_str(), extra.empty() ? "" : " and ",
                extra.c_str());
    if (!ok) std::fprintf(stderr, "WARNING: the champion file was not fully written\n");
}

// --- the trial ledger ----------------------------------------------------------
//
// Every custom strategy version ever ranked on this symbol's data, by name and
// source hash. Deleting a strategy does not delete its trial: the search
// happened, and the benchmark must keep charging for it.

struct Ledger {
    fs::path                                   path;
    std::set<std::pair<std::string, std::string>> seen;
    std::vector<std::string>                   lines;

    void load() {
        std::ifstream f(path);
        std::string   line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line.rfind("name,", 0) == 0) continue;
            const auto c = split(line, ',');
            if (c.size() < 2) continue;
            if (seen.insert({c[0], c[1]}).second) lines.push_back(line);
        }
    }
    bool add(const std::string& name, const std::string& sha) {
        if (!seen.insert({name, sha}).second) return false;
        lines.push_back(name + "," + sha + "," + utc_now_iso());
        return true;
    }
    bool save() const {
        std::string text = "name,source_sha256,first_ranked_utc\n";
        for (const std::string& l : lines) text += l + "\n";
        return write_atomic(path, text);
    }
};

// --- ranking -------------------------------------------------------------------

struct Row {
    const BaselineEntry* e = nullptr;
    Timeframe            tf = Timeframe::M15;
    std::string          error;   // set if the strategy threw
    int                  trades = 0;
    double               net = 0.0, pf = 0.0, win = 0.0, max_dd = 0.0;
    double               sr = 0.0, skew = 0.0, kurt = 3.0;
    double               cpcv_med = 0.0, cpcv_p05 = 0.0, cpcv_pos = 0.0;
    double               dsr = 0.0, stress_pf = 0.0;
    bool                 stress_run = false;
    std::vector<double>      per_block, trade_rets;
    std::vector<std::size_t> block_count;
    bool                 pass = false;
    std::string          why;
};

double profit_factor(const std::vector<Trade>& ts) {
    double won = 0.0, lost = 0.0;
    for (const Trade& t : ts) (t.net_usd > 0.0 ? won : lost) += std::fabs(t.net_usd);
    return lost > 0.0 ? won / lost : (won > 0.0 ? 99.0 : 0.0);
}

void evaluate(Row& r, const TickStore& store, const BacktestConfig& base, std::size_t blocks,
              TimeUs t0, TimeUs span, double lots) {
    try {
        BacktestConfig cfg = base;
        cfg.tf = r.tf;
        auto strat = r.e->make(lots);
        if (!strat) throw std::runtime_error("factory returned nothing");
        const BacktestResult br = BacktestEngine(store, cfg).run(*strat);

        r.trades = static_cast<int>(br.trades.size());
        r.pf = profit_factor(br.trades);
        r.per_block.assign(blocks, 0.0);
        r.block_count.assign(blocks, 0);
        std::vector<std::vector<double>> by_block(blocks);
        double cum = 0.0, peak = 0.0;
        int    wins = 0;
        for (const Trade& t : br.trades) {
            auto k = static_cast<std::size_t>((t.entry_ts - t0) / (span > 0 ? span : 1));
            if (k >= blocks) k = blocks - 1;
            r.per_block[k] += t.net_usd;
            by_block[k].push_back(t.net_usd);
            r.net += t.net_usd;
            if (t.net_usd > 0.0) ++wins;
            cum += t.net_usd;
            peak = std::max(peak, cum);
            r.max_dd = std::max(r.max_dd, peak - cum);
        }
        r.win = r.trades > 0 ? static_cast<double>(wins) / static_cast<double>(r.trades) : 0.0;
        for (std::size_t b = 0; b < blocks; ++b) {
            r.block_count[b] = by_block[b].size();
            for (double v : by_block[b]) r.trade_rets.push_back(v);
        }
        if (r.trades < kMinTradesForStats) return;
        r.sr = sharpe_ratio(r.trade_rets);
        const Moments m = moments_of(r.trade_rets);
        r.skew = m.skew;
        r.kurt = m.kurtosis;

        // CPCV: the Sharpe over every way of choosing half the blocks, so a
        // single lucky path cannot pass for a stable edge.
        const auto          combos = cpcv_test_combinations(blocks, blocks / 2);
        std::vector<double> path_sr;
        for (const auto& test : combos) {
            std::vector<double> sample;
            std::size_t         ti = 0;
            for (std::size_t b = 0; b < blocks; ++b) {
                const bool is_test = std::find(test.begin(), test.end(), b) != test.end();
                if (is_test)
                    for (std::size_t j = 0; j < r.block_count[b]; ++j) sample.push_back(r.trade_rets[ti + j]);
                ti += r.block_count[b];
            }
            if (sample.size() >= 10) path_sr.push_back(sharpe_ratio(sample));
        }
        if (path_sr.size() >= 10) {
            std::sort(path_sr.begin(), path_sr.end());
            const auto n = static_cast<double>(path_sr.size());
            r.cpcv_med = path_sr[path_sr.size() / 2];
            r.cpcv_p05 = path_sr[static_cast<std::size_t>(n * 0.05)];
            r.cpcv_pos = static_cast<double>(std::count_if(path_sr.begin(), path_sr.end(),
                                                           [](double x) { return x > 0.0; })) /
                         n;
        }
    } catch (const std::exception& ex) {
        r.error = ex.what();
    } catch (...) {
        r.error = "unknown exception";
    }
}

std::string csv_safe(std::string s) {
    for (char& ch : s)
        if (ch == ',' || ch == '\n' || ch == '\r') ch = ' ';
    return s;
}

int rank_mode(int argc, char** argv) {
    std::string dir = "data/ticks/real/XAUUSD", symbol = "XAUUSD", out = "lab", champion_extra;
    Timeframe   default_tf = Timeframe::M15;
    double      lots = 0.10;
    std::size_t blocks = 10, prior_trials = 73;
    int         min_trades = 100;
    unsigned    jobs = std::max(1u, std::thread::hardware_concurrency());
    std::set<std::string> only;
    Financing   fin;
    TimeUs      from_us = 0, to_us = 0;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool        more = i + 1 < argc;
        if (a == "--lots" && more) lots = std::atof(argv[++i]);
        else if (a == "--blocks" && more) blocks = static_cast<std::size_t>(std::atoi(argv[++i]));
        else if (a == "--prior-trials" && more) prior_trials = static_cast<std::size_t>(std::atoi(argv[++i]));
        else if (a == "--min-trades" && more) min_trades = std::atoi(argv[++i]);
        else if (a == "--jobs" && more) jobs = static_cast<unsigned>(std::max(1, std::atoi(argv[++i])));
        else if (a == "--out" && more) out = argv[++i];
        else if (a == "--champion-file" && more) champion_extra = argv[++i];
        else if (a == "--swap-long" && more) fin.long_annual = std::atof(argv[++i]);
        else if (a == "--swap-short" && more) fin.short_annual = std::atof(argv[++i]);
        else if (a == "--only" && more) {
            for (const std::string& n : split(argv[++i], ',')) only.insert(trim(n));
        } else if (a == "--tf" && more) {
            if (!parse_timeframe(argv[++i], default_tf)) {
                std::fprintf(stderr, "unknown timeframe: %s\n", argv[i]);
                return 2;
            }
        } else if ((a == "--from" || a == "--to") && more) {
            if (!parse_utc_date(argv[++i], a == "--from" ? from_us : to_us)) {
                std::fprintf(stderr, "bad date for %s: %s (want YYYY-MM-DD)\n", a.c_str(), argv[i]);
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
    if (blocks < 4) blocks = 4;
    if (blocks % 2 != 0) ++blocks;

    const TickStore store = TickStore::open(dir, symbol);
    const bool      synthetic = looks_synthetic(store);
    const TimeUs    t0 = from_us ? from_us : store.first_ts();
    const TimeUs    t1 = to_us ? to_us : store.last_ts();
    if (t1 <= t0) {
        std::fprintf(stderr, "empty window\n");
        return 2;
    }
    const TimeUs span = (t1 - t0) / static_cast<TimeUs>(blocks);

    BacktestConfig base;
    base.spec = SymbolSpec::for_symbol(symbol);
    base.initial_balance = 10'000.0;
    base.costs.slip_base_pts = 15.0;
    base.costs.slip_vol_coef = 0.05;
    base.costs.latency_us = 150'000;
    base.costs.commission_per_lot_round_usd = 7.0;
    base.from_us = from_us;
    base.to_us = to_us;
    apply_financing(base.spec, fin, 1.0);

    std::vector<Row> rows;
    for (const BaselineEntry& e : baseline_registry()) {
        if (!only.empty() && only.count(e.name) == 0) continue;
        if (!e.gate_candidate) continue;   // the null model and buy-and-hold are controls
        Row r;
        r.e = &e;
        r.tf = e.tf != Timeframe::COUNT ? e.tf : default_tf;
        rows.push_back(std::move(r));
    }
    if (rows.empty()) {
        std::fprintf(stderr, "no strategies selected\n");
        return 2;
    }
    for (const std::string& n : only)
        if (find_baseline(n.c_str()) == nullptr) std::fprintf(stderr, "WARNING: no strategy named %s\n", n.c_str());

    std::printf("store    %s (%s)%s\n", dir.c_str(), symbol.c_str(), synthetic ? "  SYNTHETIC" : "");
    std::printf("ranking  %zu strategies on %u threads, %zu blocks\n", rows.size(), jobs, blocks);

    // Every strategy is independent: its own instance, its own engine, the
    // store read-only. A strategy that throws is reported, not fatal.
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned j = 0; j < std::min<unsigned>(jobs, static_cast<unsigned>(rows.size())); ++j) {
        pool.emplace_back([&] {
            for (std::size_t i = next++; i < rows.size(); i = next++)
                evaluate(rows[i], store, base, blocks, t0, span, lots);
        });
    }
    for (std::thread& t : pool) t.join();

    // --- the search, charged for --------------------------------------------
    fs::create_directories(out);
    Ledger ledger;
    ledger.path = fs::path(out) / ("trials_" + symbol + ".csv");
    ledger.load();
    std::size_t added = 0;
    for (const Row& r : rows)
        if (r.e->custom && r.e->source_sha256 != nullptr && ledger.add(r.e->name, r.e->source_sha256)) ++added;
    if (!ledger.save()) std::fprintf(stderr, "WARNING: the trial ledger could not be saved\n");
    const std::size_t n_trials = std::max<std::size_t>(prior_trials + ledger.lines.size(), rows.size());

    std::vector<double> srs;
    for (const Row& r : rows)
        if (r.error.empty() && r.trades >= kMinTradesForStats) srs.push_back(r.sr);
    const double sr_var = srs.size() >= 2 ? moments_of(srs).stdev * moments_of(srs).stdev : 0.0;
    const double bench = expected_max_sharpe(sr_var, n_trials);

    std::vector<std::vector<double>> perf;
    for (const Row& r : rows)
        if (r.error.empty() && r.trades >= kMinTradesForStats) perf.push_back(r.per_block);
    const PboResult pbo = perf.size() >= 2 ? probability_of_backtest_overfitting(perf) : PboResult{};
    const bool      pbo_ok = perf.size() >= 2 && pbo.pbo < kGatePbo;

    for (Row& r : rows) {
        if (!r.error.empty()) {
            r.why = "threw: " + r.error;
            continue;
        }
        if (r.trades >= kMinTradesForStats)
            r.dsr = deflated_sharpe(r.sr, bench, r.trade_rets.size(), r.skew, r.kurt);
        if (r.trades < min_trades) r.why = "too few trades (" + std::to_string(r.trades) + ")";
        else if (!(r.net > 0.0)) r.why = "loses after costs";
        else if (!(r.dsr > kGateDsr)) r.why = "DSR below 0.95: not separable from luck";
        else r.pass = true;
    }

    // The stress test only for what is left: at 2x spread and 2x slippage it
    // must still make more than it loses.
    for (Row& r : rows) {
        if (!r.pass) continue;
        BacktestConfig c = base;
        c.tf = r.tf;
        c.costs.spread_mult = 2.0;
        c.costs.slippage_mult = 2.0;
        try {
            auto s = r.e->make(lots);
            r.stress_pf = profit_factor(BacktestEngine(store, c).run(*s).trades);
            r.stress_run = true;
        } catch (...) {
            r.stress_pf = 0.0;
        }
        if (!(r.stress_pf >= kGateStressPf)) {
            r.pass = false;
            r.why = "fails at doubled costs";
        }
    }

    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.pass != b.pass) return a.pass;
        if (a.dsr != b.dsr) return a.dsr > b.dsr;
        return a.sr > b.sr;
    });

    // --- report --------------------------------------------------------------
    std::printf("\n%-4s %-24s %-4s %6s %10s %6s %8s %7s %7s %6s  %s\n", "rank", "strategy", "tf", "trades",
                "net USD", "PF", "SR/trd", "CPCV+", "DSR", "gate", "why");
    std::printf("%s\n", std::string(110, '-').c_str());
    std::ostringstream csv;
    csv << "rank,name,custom,tf,trades,net_usd,pf,win_rate,max_dd_usd,sr_per_trade,cpcv_median_sr,"
           "cpcv_p05_sr,cpcv_positive_share,dsr,stress_pf,gate,why\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        std::printf("%-4zu %-24.24s %-4s %6d %10.2f %6.2f %8.4f %6.0f%% %7.4f %6s  %s\n", i + 1, r.e->name,
                    timeframe_name(r.tf), r.trades, r.net, r.pf, r.sr, r.cpcv_pos * 100.0, r.dsr,
                    r.pass ? "PASS" : "-", r.why.c_str());
        csv << i + 1 << ',' << csv_safe(r.e->name) << ',' << (r.e->custom ? 1 : 0) << ','
            << timeframe_name(r.tf) << ',' << r.trades << ',' << r.net << ',' << r.pf << ',' << r.win << ','
            << r.max_dd << ',' << r.sr << ',' << r.cpcv_med << ',' << r.cpcv_p05 << ',' << r.cpcv_pos << ','
            << r.dsr << ',' << (r.stress_run ? std::to_string(r.stress_pf) : "") << ','
            << (r.pass ? "PASS" : "FAIL") << ',' << csv_safe(r.why) << '\n';
    }
    const fs::path board = fs::path(out) / ("leaderboard_" + symbol + ".csv");
    if (!write_atomic(board, csv.str())) std::fprintf(stderr, "WARNING: leaderboard not written\n");

    std::printf("\ntrials   %zu charged (%zu before the lab + %zu custom versions in %s; %zu new)\n", n_trials,
                prior_trials, ledger.lines.size(), ledger.path.string().c_str(), added);
    std::printf("luck     the best of %zu zero-edge strategies would show SR/trade %.4f\n", n_trials, bench);
    std::printf("PBO      %.3f over %zu splits (gate < %.2f: %s)\n", pbo.pbo, pbo.splits, kGatePbo,
                pbo_ok ? "PASS" : "FAIL");
    std::printf("board    %s\n", board.string().c_str());

    Champion c;
    c.source = "lab";
    const Row* best = (!rows.empty() && rows.front().pass) ? &rows.front() : nullptr;
    if (synthetic) {
        c.reason = "synthetic data: nothing on it is evidence";
    } else if (best == nullptr) {
        c.reason = "no strategy passed every gate";
    } else if (!pbo_ok) {
        c.reason = "PBO " + std::to_string(pbo.pbo) + ": the in-sample winner is unreliable out of sample";
    } else {
        c.name = best->e->name;
        c.tf = timeframe_name(best->tf);
        char ev[96];
        std::snprintf(ev, sizeof(ev), "dsr=%.4f trades=%d stress_pf=%.3f pbo=%.3f", best->dsr, best->trades,
                      best->stress_pf, pbo.pbo);
        c.evidence = ev;
        c.reason = "passed every gate; trade it on DEMO first";
    }
    publish_champion(c, fs::path(out) / ("champion_" + symbol + ".txt"), champion_extra);
    return 0;
}

// --- live evolution --------------------------------------------------------------

struct LiveTrade {
    std::string strategy;
    double      ret = 0.0;   // net / balance before the trade
    long long   close_ms = 0;
};

int live_mode(int argc, char** argv) {
    std::vector<std::string> journals;
    std::string              symbol = "XAUUSD", out = "lab", champion_extra;
    std::size_t min_live = 40, min_retire = 20;
    double      retire_below = 0.10, promote_above = 0.90;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool        more = i + 1 < argc;
        if (a == "--live" && more) journals.push_back(argv[++i]);
        else if (a == "--symbol" && more) symbol = argv[++i];
        else if (a == "--out" && more) out = argv[++i];
        else if (a == "--champion-file" && more) champion_extra = argv[++i];
        else if (a == "--min-live" && more) min_live = static_cast<std::size_t>(std::atoi(argv[++i]));
        else if (a == "--retire-below" && more) retire_below = std::atof(argv[++i]);
        else if (a == "--promote-above" && more) promote_above = std::atof(argv[++i]);
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    // One row per closed position: close_utc_ms,position,strategy,magic,side,lots,net,balance_before
    std::map<std::string, std::vector<LiveTrade>> by;
    std::set<std::string>                         positions;
    std::size_t                                   bad = 0, dup = 0;
    for (const std::string& journal : journals) {
        std::ifstream f(journal);
        if (!f) {
            std::fprintf(stderr, "cannot read journal %s\n", journal.c_str());
            return 2;
        }
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line.rfind("close_utc_ms", 0) == 0) continue;
            const auto c = split(line, ',');
            try {
                if (c.size() < 8) throw std::runtime_error("short");
                // The EA appends and never replays, but a copied or merged
                // journal can repeat lines. One position counts once.
                if (!positions.insert(journal + "|" + c[1] + "|" + c[3]).second) {
                    ++dup;
                    continue;
                }
                LiveTrade t;
                t.close_ms = std::stoll(c[0]);
                t.strategy = c[2];
                const double net = std::stod(c[6]), bal = std::stod(c[7]);
                if (!(bal > 0.0)) throw std::runtime_error("balance");
                t.ret = net / bal;
                by[t.strategy].push_back(t);
            } catch (...) {
                ++bad;
            }
        }
    }

    // The variance prior from every trade in the journal: one strategy's few
    // trades say little about scale, all of them together say more.
    std::vector<double> all;
    for (const auto& [name, v] : by)
        for (const LiveTrade& t : v) all.push_back(t.ret);
    EdgePrior prior;
    if (all.size() >= 10) {
        const Moments m = moments_of(all);
        if (m.stdev > 0.0) prior.var = m.stdev * m.stdev;
    }

    struct Live {
        std::string   name;
        EdgePosterior post;
        double        net_ret = 0.0;
        std::string   status;
    };
    std::vector<Live> rows;
    for (const auto& [name, v] : by) {
        std::vector<double> r;
        Live                l;
        l.name = name;
        for (const LiveTrade& t : v) {
            r.push_back(t.ret);
            l.net_ret += t.ret;
        }
        l.post = edge_posterior(r, prior);
        if (l.post.n >= min_retire && l.post.p_edge < retire_below) l.status = "RETIRE";
        else if (l.post.n >= min_live && l.post.p_edge >= promote_above) l.status = "PROVEN";
        else l.status = "collecting";
        rows.push_back(l);
    }
    std::vector<EdgePosterior> posts;
    for (const Live& l : rows) posts.push_back(l.post);
    const std::vector<double> pbest = prob_best(posts, 20'000, 20261008);

    std::printf("journals %zu: %zu trades across %zu strategies (%zu duplicates, %zu unreadable)\n",
                journals.size(), all.size(), rows.size(), dup, bad);
    std::printf("prior    skeptical: zero edge, worth %.0f trades; per-trade sd %.4f%%\n\n", prior.strength,
                std::sqrt(prior.var) * 100.0);
    std::printf("%-24s %6s %10s %12s %9s %9s  %s\n", "strategy", "trades", "sum ret", "E[ret]/trade",
                "P(edge)", "P(best)", "status");
    std::printf("%s\n", std::string(88, '-').c_str());
    std::ostringstream csv;
    csv << "name,trades,sum_ret,posterior_mean_ret,p_edge,p_best,status\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Live& l = rows[i];
        std::printf("%-24.24s %6zu %9.3f%% %11.4f%% %9.3f %9.3f  %s\n", l.name.c_str(), l.post.n,
                    l.net_ret * 100.0, l.post.mean * 100.0, l.post.p_edge, pbest[i], l.status.c_str());
        csv << csv_safe(l.name) << ',' << l.post.n << ',' << l.net_ret << ',' << l.post.mean << ','
            << l.post.p_edge << ',' << pbest[i] << ',' << l.status << '\n';
    }
    write_atomic(fs::path(out) / ("live_" + symbol + ".csv"), csv.str());

    // --- the champion, evolved -----------------------------------------------
    const fs::path cfile = fs::path(out) / ("champion_" + symbol + ".txt");
    Champion       cur;
    const bool     have_cur = read_champion(champion_extra.empty() ? cfile : fs::path(champion_extra), cur) ||
                          read_champion(cfile, cur);

    // Promotion: proven over enough live trades, and the likeliest to be best
    // among those proven. Live evidence outranks the backtest's.
    const Live* promote = nullptr;
    double      promote_p = -1.0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].status != "PROVEN") continue;
        if (find_baseline(rows[i].name.c_str()) == nullptr) continue;   // not in this build
        if (pbest[i] > promote_p) {
            promote_p = pbest[i];
            promote = &rows[i];
        }
    }
    if (promote != nullptr) {
        Champion c;
        c.name = promote->name;
        const BaselineEntry* e = find_baseline(promote->name.c_str());
        // The bar length it traded live is the one it is proven on: keep the
        // current champion's when it is the same strategy, else its own.
        c.tf = (have_cur && cur.name == c.name && !cur.tf.empty())
                   ? cur.tf
                   : (e->tf != Timeframe::COUNT ? timeframe_name(e->tf) : "M15");
        c.source = "live";
        char ev[96];
        std::snprintf(ev, sizeof(ev), "p_edge=%.4f p_best=%.4f n=%zu", promote->post.p_edge, promote_p,
                      promote->post.n);
        c.evidence = ev;
        c.reason = "proven on live trades";
        if (have_cur && cur.name == c.name) {
            std::printf("\nchampion  %s stays (%s)\n", c.name.c_str(), ev);
        }
        publish_champion(c, cfile, champion_extra);
        return 0;
    }
    for (const Live& l : rows) {
        if (have_cur && l.name == cur.name && l.status == "RETIRE") {
            Champion c;
            c.source = "live";
            char why[160];
            std::snprintf(why, sizeof(why), "%s retired: P(edge) %.3f after %zu live trades", l.name.c_str(),
                          l.post.p_edge, l.post.n);
            c.reason = why;
            publish_champion(c, cfile, champion_extra);
            return 0;
        }
    }
    std::printf("\nchampion  unchanged (%s): no strategy proven, the current one not retired\n",
                have_cur ? (cur.name.empty() ? "NONE" : cur.name.c_str()) : "no champion file yet");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--live") return live_mode(argc, argv);
        return rank_mode(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
