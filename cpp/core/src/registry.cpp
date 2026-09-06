#include "xau/registry.hpp"

#include "xau/baselines.hpp"
#include "xau/rules.hpp"
#include "xau/zoo.hpp"

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
        {"BollingerReversion",
         "Fade a stretch beyond 2 SD back toward the mean. The opposite bet to every breakout rule here, which is why it earns a slot.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             BollingerReversion::Config c;
             c.lots = lots;
             return std::make_unique<BollingerReversion>(c);
         }},
        {"Rsi2Extreme",
         "Connors RSI-2 below 5 or above 95. Keys on the run of closes rather than the distance travelled.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             Rsi2Extreme::Config c;
             c.lots = lots;
             return std::make_unique<Rsi2Extreme>(c);
         }},
        {"MomentumContinuation",
         "Buy strength, sell weakness. The null that any claim of trend on gold has to beat.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             MomentumContinuation::Config c;
             c.lots = lots;
             return std::make_unique<MomentumContinuation>(c);
         }},
        {"SessionDrift",
         "Direction of the first bars after the NY open, held into the session. No indicator: pure time of day.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             SessionDrift::Config c;
             c.lots = lots;
             return std::make_unique<SessionDrift>(c);
         }},
        {"LiquidityFade",
         "A spread spike with no range expansion is liquidity leaving, not news arriving. Reads the book, not the chart.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             LiquidityFade::Config c;
             c.lots = lots;
             return std::make_unique<LiquidityFade>(c);
         }},
        {"InsideBarBreak",
         "Break of the mother bar after a one-bar compression. Setups age out in three bars.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             InsideBarBreak::Config c;
             c.lots = lots;
             return std::make_unique<InsideBarBreak>(c);
         }},
        {"WeekendGapFade",
         "Gold gaps over the weekend and usually fills. Fires once a week at most, so it is reported on thin data.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             WeekendGapFade::Config c;
             c.lots = lots;
             return std::make_unique<WeekendGapFade>(c);
         }},
        {"AdaptiveTrend",
         "EMA cross gated on expanding volatility, so it sits out the chop that bleeds most trend systems.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             AdaptiveTrend::Config c;
             c.lots = lots;
             return std::make_unique<AdaptiveTrend>(c);
         }},
        {"Rsi2InRange",
         "RSI-2 extremes, but only where a mean-reversion bet has a reason to "
         "work: ranging markets and high-volatility overshoots. It sits out "
         "normal trends, which is 41% of the decade and where it bleeds worst.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             Rsi2Extreme::Config c;
             c.lots = lots;
             return std::make_unique<RegimeGated>(
                 std::make_unique<Rsi2Extreme>(c),
                 RegimeGated::mask_of({0, 2, 4, 5}),   // ranges + wild/trend
                 "Rsi2InRange");
         }},
        {"Rsi2RangesOnly",
         "The honesty check on Rsi2InRange. Mean reversion only in RANGING "
         "markets -- a rule stated from theory, with no cell chosen by looking "
         "at the table. If this holds up, the effect is real; if only the "
         "hand-picked version works, the hand-picking was the effect.",
         true,
         [](double lots) -> std::unique_ptr<Strategy> {
             Rsi2Extreme::Config c;
             c.lots = lots;
             return std::make_unique<RegimeGated>(
                 std::make_unique<Rsi2Extreme>(c),
                 RegimeGated::mask_of({0, 2, 4}),   // ranging only, a priori
                 "Rsi2RangesOnly");
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
