// Conformance: what every registered strategy must satisfy, built-in or
// dropped into cpp/strategies/custom/ by hand or by an AI.
//
// These are the properties a backtest silently depends on and a generated
// strategy most often breaks. None of them says a strategy is GOOD -- the lab
// answers that. They say its backtest MEANS something:
//
//   deterministic  the same ticks give the same trades, run after run. A
//                  strategy keeping state in a static, or seeding from the
//                  clock, gives each walk-forward fold a different strategy.
//   causal         trades that closed before time T are the same whether or
//                  not the data goes on past T. A strategy whose past depends
//                  on its future is trading on information it will not have.
//   well-formed    no exception, a name that matches its registry entry, and,
//                  for a custom one, the bar length and source hash the lab
//                  and the trial ledger rely on.

#include "fixture.hpp"
#include "harness.hpp"

#include "xau/engine.hpp"
#include "xau/registry.hpp"
#include "xau/session.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {

// Ninety days of quotes every 5 s: a random walk whose drift switches every
// few days between up, down and flat, so trend and reversion rules both find
// something to do.
std::vector<xau::Tick> conformance_ticks() {
    std::vector<xau::Tick> v;
    const xau::TimeUs      t0 = 1'704'067'200'000'000LL;   // 2024-01-01, a Monday
    xau::Points            bid = 2'000'000;
    std::uint64_t          s = 12345;
    int                    drift = 0;
    for (xau::TimeUs t = t0; t < t0 + 90 * xau::kUsPerDay; t += 5'000'000) {
        if (!xau::market_open(t)) continue;
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        if ((s >> 40) % 50'000 == 0) drift = static_cast<int>((s >> 20) % 3) - 1;
        bid += static_cast<xau::Points>((s >> 59) % 31) - 15 + drift;
        const int h = xau::utc_hour(t);
        v.push_back(fixture::tick(t, bid, static_cast<std::uint16_t>((h >= 7 && h < 17) ? 220 : 380)));
    }
    return v;
}

bool same_trade(const xau::Trade& a, const xau::Trade& b) {
    return a.entry_ts == b.entry_ts && a.exit_ts == b.exit_ts && a.side == b.side &&
           a.lots == b.lots && a.entry_pts == b.entry_pts && a.exit_pts == b.exit_pts &&
           a.exit_reason == b.exit_reason;
}

std::vector<xau::Trade> closed_before(const std::vector<xau::Trade>& ts, xau::TimeUs t) {
    std::vector<xau::Trade> out;
    for (const xau::Trade& x : ts)
        if (x.exit_reason != xau::ExitReason::EndOfData && x.exit_ts < t) out.push_back(x);
    return out;
}

bool is_hex64(const char* s) {
    if (s == nullptr || std::strlen(s) != 64) return false;
    for (const char* p = s; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}

}  // namespace

XAU_TEST(every_registered_strategy_is_deterministic_causal_and_well_formed) {
    const auto       ticks = conformance_ticks();
    fixture::TempDir dir;
    const auto       store = fixture::make_store(dir, ticks);
    const xau::TimeUs cut = ticks[ticks.size() / 2].ts_us;

    std::size_t strategies = 0, traded = 0, custom = 0;
    for (const xau::BaselineEntry& e : xau::baseline_registry()) {
        ++strategies;
        const xau::Timeframe tf = e.tf != xau::Timeframe::COUNT ? e.tf : xau::Timeframe::M15;
        xau::BacktestConfig  cfg;
        cfg.spec = xau::SymbolSpec::xauusd_default();
        cfg.tf = tf;
        cfg.apply_swap = false;

        const auto run = [&](xau::TimeUs to_us, std::vector<xau::Trade>& out) -> bool {
            try {
                auto s = e.make(0.10);
                if (!s) return false;
                xau::BacktestConfig c = cfg;
                c.to_us = to_us;
                out = xau::BacktestEngine(store, c).run(*s).trades;
                return true;
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "    %s threw: %s\n", e.name, ex.what());
                return false;
            } catch (...) {
                std::fprintf(stderr, "    %s threw a non-exception\n", e.name);
                return false;
            }
        };

        std::vector<xau::Trade> a, b, half;
        const bool ok = run(0, a) && run(0, b) && run(cut, half);
        if (!ok) {
            CHECK(false);
            continue;
        }
        if (!a.empty()) ++traded;

        // Deterministic: the second run, on a fresh instance after the first
        // has run in this process, trades exactly the same.
        bool same = a.size() == b.size();
        for (std::size_t i = 0; same && i < a.size(); ++i) same = same_trade(a[i], b[i]);
        if (!same) std::fprintf(stderr, "    %s: two runs on the same ticks disagree\n", e.name);
        CHECK(same);

        // Causal: what closed before the cut does not depend on what follows it.
        const auto pa = closed_before(a, cut), ph = closed_before(half, cut);
        bool causal = pa.size() == ph.size();
        for (std::size_t i = 0; causal && i < pa.size(); ++i) causal = same_trade(pa[i], ph[i]);
        if (!causal)
            std::fprintf(stderr, "    %s: trades before the cut change when later data is added\n",
                         e.name);
        CHECK(causal);

        if (e.custom) {
            ++custom;
            // The lab and the MT5 bridge find it by this name; the journal
            // records what name() says. They must be one name.
            auto s = e.make(0.10);
            CHECK(s && std::strcmp(s->name(), e.name) == 0);
            CHECK(e.tf != xau::Timeframe::COUNT);
            CHECK(e.source != nullptr && std::strlen(e.source) > 4);
            CHECK(is_hex64(e.source_sha256));
            CHECK(e.description != nullptr && std::strlen(e.description) > 0);
        }
    }
    std::fprintf(stderr, "    %zu strategies (%zu custom), %zu traded on the test ticks\n",
                 strategies, custom, traded);
    // Checking strategies that never trade proves nothing about the ones that do.
    CHECK(traded >= strategies / 2);
}
