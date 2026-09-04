#include "xau/registry.hpp"

#include "xau/baselines.hpp"
#include "xau/rules.hpp"

#include <cstring>
#include <vector>

namespace xau {
namespace {

const std::vector<BaselineEntry>& entries() {
    static const std::vector<BaselineEntry> v = {
        {"LondonOpeningRange",
         "Range over the London open hour, trade the break. ATR band cuts both "
         "tails: too narrow is noise, too wide means the move already happened.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             LondonOpeningRange::Config c;
             c.lots = lots;
             return std::make_unique<LondonOpeningRange>(c);
         }},
        {"AsiaRangeBreakout",
         "Gold coils through thin Asia and often resolves when London arrives. "
         "The range window wraps midnight.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             AsiaRangeBreakout::Config c;
             c.lots = lots;
             return std::make_unique<AsiaRangeBreakout>(c);
         }},
        {"TrendPullback",
         "With the slow trend, entered on the resumption rather than the "
         "pullback, so it is not a knife-catch.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             TrendPullback::Config c;
             c.lots = lots;
             return std::make_unique<TrendPullback>(c);
         }},
        {"VolatilityCompression",
         "NR7 marks compression, the trade is the expansion. Setups age out.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             VolatilityCompression::Config c;
             c.lots = lots;
             return std::make_unique<VolatilityCompression>(c);
         }},
        {"RandomEntry (null)",
         "The cost-only baseline. Random side, fixed hold, no stops. Every real "
         "strategy has to beat this by a wide margin; on driftless data it loses "
         "exactly the round-turn cost.",
         false,
         [](double lots) -> std::unique_ptr<Strategy> {
             RandomEntry::Config c;
             c.lots = lots;
             c.entry_prob = 0.05;
             c.hold_bars = 8;
             return std::make_unique<RandomEntry>(c);
         }},
        {"BuyAndHold",
         "Enters once and never exits. Not a strategy — the reconciliation "
         "target that proves the engine's accounting.",
         false,
         [](double lots) -> std::unique_ptr<Strategy> {
             return std::make_unique<BuyAndHold>(lots, Side::Long);
         }},
    };
    return v;
}

}  // namespace

std::span<const BaselineEntry> baseline_registry() { return entries(); }

const BaselineEntry* find_baseline(const char* name) {
    if (name == nullptr) return nullptr;
    for (const BaselineEntry& e : entries()) {
        if (std::strcmp(e.name, name) == 0) return &e;
    }
    return nullptr;
}

}  // namespace xau
