// Reference strategies used to validate the engine itself.
//
// Neither is meant to make money. They exist because an unvalidated backtester
// is a random number generator with good typography, and these two are how the
// Phase 1 gate checks it: one has an answer you can work out on paper, the
// other has an answer the cost model predicts exactly.
//
// The rule strategies that are actually meant to trade arrive in Phase 3.
#pragma once

#include "xau/strategy.hpp"

#include <cstdint>

namespace xau {

// Deterministic RNG, in the library rather than <random> on purpose.
//
// std::mt19937 is specified across implementations but the distributions are
// not, so std::uniform_real_distribution can hand back different numbers on
// different standard libraries. A backtest that changes when you switch
// compilers is not reproducible, and reproducibility is the whole point of the
// golden-file test.
struct SplitMix64 {
    std::uint64_t state = 0x5EED'1234'ABCD'0001ULL;

    std::uint64_t next() noexcept {
        state += 0x9E37'79B9'7F4A'7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31);
    }

    // [0, 1) from the top 53 bits, which is the exactly-representable range.
    double unit() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
};

// Enters once on the first eligible bar and never exits; the engine closes it
// at end of data. Its P&L is a subtraction you can do on paper, which makes it
// the reconciliation target for the whole fill and accounting path.
class BuyAndHold final : public Strategy {
public:
    explicit BuyAndHold(double lots = 0.01, Side side = Side::Long) noexcept
        : lots_(lots), side_(side) {}

    [[nodiscard]] const char* name() const noexcept override { return "BuyAndHold"; }
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    double lots_;
    Side   side_;
    bool   signalled_ = false;
};

// The null model: random side, fixed holding period, no stops.
//
// Over a driftless series the expected gross P&L is zero, so the expected net
// is exactly minus the round-turn cost — one spread, two slippages, one
// commission. That makes this a direct test of the cost model rather than a
// strategy. If measured expectancy does not match predicted cost, the fill
// model and the cost model disagree, and every later result is suspect.
//
// It is also the baseline every real strategy has to beat by a wide margin.
class RandomEntry final : public Strategy {
public:
    struct Config {
        double        entry_prob = 0.02;   // per eligible bar
        std::size_t   hold_bars = 8;
        double        lots = 0.01;
        std::uint64_t seed = 0x5EED'1234'ABCD'0001ULL;
    };

    // Two constructors rather than a `Config c = {}` default argument: GCC
    // rejects brace-initialising an aggregate-with-NSDMIs in a default
    // argument inside an incomplete class, where MSVC accepts it. Spelling
    // both out avoids the disagreement entirely.
    RandomEntry() noexcept { rng_.state = cfg_.seed; }
    explicit RandomEntry(const Config& c) noexcept : cfg_(c) { rng_.state = c.seed; }

    [[nodiscard]] const char* name() const noexcept override { return "RandomEntry(null)"; }
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config     cfg_{};
    SplitMix64 rng_{};
};

}  // namespace xau
