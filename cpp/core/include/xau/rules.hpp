// Phase 3 rule baselines — the bar every later model has to beat.
//
// These exist for a specific reason. Many gold edges that actually survive are
// session-structural rather than statistical: the London open, the Asia range,
// the overlap. It is entirely possible one of these is the final product and
// the Phase 5 model only gates it. Running the ML first would mean never
// finding that out, and never knowing how much the model actually added.
//
// The Phase 3 gate: at least one of these must show PF > 1.05 after 2x costs on
// walk-forward. If none do, the plan says stop and revisit the timeframe rather
// than reach for a bigger model.
//
// None of this can be judged on synthetic ticks. The generator is a driftless
// random walk by construction — it is the null hypothesis for the Phase 4
// leakage tests — so a run against it measures the cost model, not an edge.
#pragma once

#include "xau/indicators.hpp"
#include "xau/session.hpp"
#include "xau/strategy.hpp"

#include <cstddef>

namespace xau {

// Shared exit discipline. The plan's design is intraday, flat overnight and
// flat over weekends, and this is the strategy-side expression of it. The hard
// guards — daily loss floor, news blackout, prop drawdown — belong to the
// Phase 7 risk layer and are deliberately not here.
struct ExitRules {
    std::size_t max_hold_bars = 32;    // 8 hours on M15
    int         flat_by_hour = 21;     // out before the 21:00 UTC rollover
    int         friday_flat_hour = 20; // and earlier before the weekend gap
};

[[nodiscard]] bool should_time_exit(const BarContext& c, const ExitRules& r) noexcept;

// ---------------------------------------------------------------------------
// 1. London opening-range breakout
//
// Build the range over the London open hour, then trade a close beyond it. The
// ATR filter cuts both tails: a range far narrower than ATR is noise, and one
// far wider means the move already happened and we would be buying the top.
// ---------------------------------------------------------------------------
class LondonOpeningRange final : public Strategy {
public:
    struct Config {
        int         range_from_hour = 7;    // 07:00-08:00 UTC
        int         range_to_hour = 8;
        int         trade_until_hour = 16;  // no new entries after this
        std::size_t atr_period = 14;
        double      sl_atr = 1.0;
        double      tp_r = 2.0;             // target = tp_r x risk
        double      min_range_atr = 0.5;
        double      max_range_atr = 3.0;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    LondonOpeningRange();
    explicit LondonOpeningRange(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "LondonOpeningRange"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config cfg_;
    Atr    atr_;
    TimeUs cur_day_ = 0;
    bool   have_day_ = false;
    Points or_high_ = 0;
    Points or_low_ = 0;
    bool   or_valid_ = false;
    bool   traded_today_ = false;
};

// ---------------------------------------------------------------------------
// 2. Asia-range breakout at the London open
//
// Gold coils through the thin Asia session and frequently resolves when London
// arrives. The range window wraps midnight, which is why session_day() exists.
// ---------------------------------------------------------------------------
class AsiaRangeBreakout final : public Strategy {
public:
    struct Config {
        int         asia_from_hour = 23;      // 23:00 -> 07:00 UTC, wraps midnight
        int         asia_to_hour = 7;
        int         trade_until_hour = 12;
        std::size_t atr_period = 14;
        double      sl_atr = 1.0;
        double      tp_r = 2.0;
        double      min_range_atr = 0.4;
        double      max_range_atr = 2.5;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    AsiaRangeBreakout();
    explicit AsiaRangeBreakout(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "AsiaRangeBreakout"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config cfg_;
    Atr    atr_;
    TimeUs cur_key_ = 0;
    bool   have_key_ = false;
    Points hi_ = 0;
    Points lo_ = 0;
    bool   range_valid_ = false;
    bool   traded_ = false;
};

// ---------------------------------------------------------------------------
// 3. Trend continuation on a pullback
//
// Trade with the slow trend, but only after price has pulled back into the fast
// average and then resumed. Entering on the resumption rather than the pullback
// is what keeps it from being a knife-catch.
// ---------------------------------------------------------------------------
class TrendPullback final : public Strategy {
public:
    struct Config {
        std::size_t trend_ema = 200;
        std::size_t pullback_ema = 20;
        std::size_t atr_period = 14;
        double      pullback_atr = 0.5;   // how close to the fast EMA counts
        double      sl_atr = 1.5;
        double      tp_r = 2.0;
        int         trade_from_hour = 7;
        int         trade_until_hour = 20;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    TrendPullback();
    explicit TrendPullback(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "TrendPullback"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config cfg_;
    Atr    atr_;
    Ema    slow_;
    Ema    fast_;
    bool   armed_long_ = false;
    bool   armed_short_ = false;
};

// ---------------------------------------------------------------------------
// 4. Volatility-compression breakout
//
// An NR7 bar marks compression; the trade is the expansion out of it. The setup
// expires after a few bars, because compression that does not resolve quickly
// usually just becomes more compression.
// ---------------------------------------------------------------------------
class VolatilityCompression final : public Strategy {
public:
    struct Config {
        std::size_t nr_lookback = 7;    // NR7
        std::size_t atr_period = 14;
        std::size_t expiry_bars = 4;
        double      sl_atr = 1.0;
        double      tp_r = 2.0;
        int         trade_from_hour = 6;
        int         trade_until_hour = 20;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    VolatilityCompression();
    explicit VolatilityCompression(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "VolatilityCompression"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config      cfg_;
    Atr         atr_;
    bool        armed_ = false;
    std::size_t bars_since_arm_ = 0;
    Points      arm_high_ = 0;
    Points      arm_low_ = 0;
};

}  // namespace xau
