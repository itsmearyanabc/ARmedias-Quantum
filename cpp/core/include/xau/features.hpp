// Feature engine.
//
// The one rule this file exists to enforce: a feature row for bar i is built
// from bars [0, i] only. Never i+1. The entire validation apparatus downstream
// -- purged CV, CPCV, deflated Sharpe -- is worthless if this rule leaks, and a
// leak here does not look like a bug. It looks like a great strategy.
//
// So the API takes a prefix span rather than a full history plus an index. You
// cannot read a bar you were not handed, which makes the safe thing the only
// thing available rather than merely the documented thing.
//
// Everything is scale-free: returns are in ATR units, spreads are fractions of
// ATR, positions are fractions of a range. Gold ran from 1,100 to 2,790 over
// the decade in the store, so a feature denominated in raw points means
// something different in 2015 than in 2024, and a model trained across both
// would spend its capacity learning the price level instead of the signal.

#ifndef XAU_FEATURES_HPP
#define XAU_FEATURES_HPP

#include "xau/bar.hpp"
#include "xau/session.hpp"
#include "xau/types.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace xau {

// Column order is the contract between C++ and the Python trainer. Append
// only; never reorder or delete, or a model exported yesterday silently reads
// the wrong column today.
enum class Feat : std::size_t {
    // -- price ------------------------------------------------------------
    Ret1Atr,          // 1-bar return in ATR units
    Ret4Atr,
    Ret16Atr,
    EmaDistAtr,       // (close - EMA20) / ATR
    RangePosition,    // where close sits in the last 20 bars' range, 0..1
    RangeExpansion,   // this bar's range / mean range of the last 20
    BodyFraction,     // |close-open| / range, a doji-vs-marubozu measure

    // -- microstructure ---------------------------------------------------
    SpreadAtr,        // mean spread / ATR: what the trade costs relative to the move
    SpreadMaxRatio,   // max spread / mean spread within the bar, a stress signal
    TickIntensity,    // tick count vs its own 20-bar mean

    // -- cross-timeframe --------------------------------------------------
    // Higher-timeframe context, derived from the same bar array by strided
    // aggregation, so there is one source of truth and no second bar builder
    // to fall out of sync.
    HtfRetAtr,        // 4x-timeframe return in ATR units
    HtfAlign,         // +1/-1/0: does the HTF direction agree with the LTF

    // -- session ----------------------------------------------------------
    // Cyclic, because hour 23 is adjacent to hour 0 and a raw integer hour
    // teaches a tree that midnight is maximally far from 23:00.
    HourSin,
    HourCos,
    SessionId,        // Session enum as a number; trees split it fine
    MinutesIntoSession,
    IsRollover,       // the 21:00-22:00 UTC spread blowout

    COUNT
};

inline constexpr std::size_t kFeatureCount = static_cast<std::size_t>(Feat::COUNT);

using FeatureRow = std::array<double, kFeatureCount>;

[[nodiscard]] std::string_view feature_name(Feat f) noexcept;

// How many bars of history must exist before a row is meaningful. Rows before
// this are still emitted -- dropping them here would desynchronise the feature
// matrix from the bar array -- but `valid` is false and they must not train.
inline constexpr std::size_t kFeatureWarmup = 64;

struct FeatureMatrix {
    std::vector<FeatureRow> rows;
    std::vector<TimeUs>     bar_close_us;   // when this row became knowable
    std::vector<char>       valid;          // false during warmup

    [[nodiscard]] std::size_t size() const noexcept { return rows.size(); }
};

// Build the row for the LAST bar of `history`. The span is the causality
// guarantee: there is no way to reach a later bar from here.
[[nodiscard]] FeatureRow compute_row(std::span<const Bar> history, Timeframe tf) noexcept;

// Build every row for a bar array. Equivalent to calling compute_row on each
// growing prefix, and tested against exactly that.
[[nodiscard]] FeatureMatrix compute_features(std::span<const Bar> bars, Timeframe tf);

}  // namespace xau

#endif  // XAU_FEATURES_HPP
