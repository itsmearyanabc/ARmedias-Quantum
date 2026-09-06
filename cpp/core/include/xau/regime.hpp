// Market regime classification.
//
// The D1 sweep turned up something specific: InsideBarBreak's MEDIAN fold has
// a profit factor of 1.130 while its POOLED figure is 0.923. The typical
// quarter makes money and a few of them give it all back. Averaging over that
// answers the wrong question -- "how does this strategy do on gold" -- when the
// useful one is "under which conditions does this strategy do well, and can we
// tell we are in those conditions before the trade rather than after".
//
// Two axes, because they are the two that change what a strategy should do:
//
//   VOLATILITY  is the market moving enough to pay for the spread? This is the
//               axis Phase 3 showed dominates cost, and cost is what has killed
//               every strategy so far.
//   STRUCTURE   is price trending or reverting? Momentum and mean reversion are
//               opposite bets, and each is right in a different world.
//
// Both are computed from a CAUSAL prefix -- classify(history) can only see bars
// that have closed. A regime label that peeks at the future would make the
// selector look brilliant and be worthless, in exactly the way section 8 warns
// about.

#ifndef XAU_REGIME_HPP
#define XAU_REGIME_HPP

#include "xau/bar.hpp"
#include "xau/types.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace xau {

enum class Volatility : std::uint8_t { Quiet, Normal, Wild };
enum class Structure : std::uint8_t { Ranging, Trending };

struct Regime {
    Volatility vol = Volatility::Normal;
    Structure  structure = Structure::Ranging;

    // A dense index, for use as an array subscript in per-regime tables.
    [[nodiscard]] constexpr std::size_t index() const noexcept {
        return static_cast<std::size_t>(vol) * 2 + static_cast<std::size_t>(structure);
    }
    static constexpr std::size_t kCount = 6;
};

[[nodiscard]] std::string_view regime_name(Regime r) noexcept;
[[nodiscard]] std::string_view regime_name_at(std::size_t index) noexcept;

struct RegimeConfig {
    std::size_t atr_period = 14;
    // The window the current ATR is judged against. Absolute volatility is
    // useless across a decade in which gold doubled; what matters is whether
    // today is busy RELATIVE to its own recent past.
    std::size_t vol_lookback = 100;
    double      quiet_below = 0.85;   // ATR / median ATR
    double      wild_above = 1.25;

    // Structure: how far the fast and slow means have separated, in ATR. A
    // wide separation means price is going somewhere; a narrow one means it is
    // oscillating around a level.
    std::size_t fast_ma = 20;
    std::size_t slow_ma = 60;
    double      trend_atr = 0.75;
};

// Classify the regime as of the LAST bar of `history`.
[[nodiscard]] Regime classify(std::span<const Bar> history, const RegimeConfig& cfg) noexcept;

// Convenience: classify with the defaults.
[[nodiscard]] inline Regime classify(std::span<const Bar> history) noexcept {
    return classify(history, RegimeConfig{});
}

}  // namespace xau

#endif  // XAU_REGIME_HPP
