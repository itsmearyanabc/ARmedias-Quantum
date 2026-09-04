// Time bars built from the tick stream.
//
// One rule governs this whole file, and it is the most common source of silent
// lookahead in retail backtests: **a bar's timestamp is its OPEN**. An M15 bar
// stamped 14:00 covers 14:00-14:15, and you cannot act on it until 14:15.
//
// BarSeries therefore separates `closed()` from `forming()`. Only the former is
// safe to make decisions on. The names are the guard rail; Phase 1 will make it
// a type-level one when the Strategy interface exists to enforce it against.
#pragma once

#include "xau/tick_store.hpp"
#include "xau/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace xau {

enum class Timeframe : int { M1, M5, M15, M30, H1, H4, D1, COUNT };

constexpr TimeUs timeframe_us(Timeframe tf) noexcept {
    switch (tf) {
        case Timeframe::M1:  return 60LL * 1'000'000;
        case Timeframe::M5:  return 300LL * 1'000'000;
        case Timeframe::M15: return 900LL * 1'000'000;
        case Timeframe::M30: return 1800LL * 1'000'000;
        case Timeframe::H1:  return 3600LL * 1'000'000;
        case Timeframe::H4:  return 14400LL * 1'000'000;
        case Timeframe::D1:  return 86400LL * 1'000'000;
        default:             return 900LL * 1'000'000;
    }
}

constexpr const char* timeframe_name(Timeframe tf) noexcept {
    switch (tf) {
        case Timeframe::M1:  return "M1";
        case Timeframe::M5:  return "M5";
        case Timeframe::M15: return "M15";
        case Timeframe::M30: return "M30";
        case Timeframe::H1:  return "H1";
        case Timeframe::H4:  return "H4";
        case Timeframe::D1:  return "D1";
        default:             return "?";
    }
}

// OHLC is quoted on the BID, matching MT5 chart convention. The ask side is
// carried separately as spread statistics, because on gold the spread is a
// first-class part of the cost model rather than a rounding detail.
struct Bar {
    TimeUs        open_time_us = 0;
    Points        open = 0;
    Points        high = 0;
    Points        low = 0;
    Points        close = 0;
    std::uint32_t ticks = 0;             // tick count, the only volume proxy we get
    std::uint32_t spread_mean_pts = 0;
    std::uint32_t spread_max_pts = 0;

    [[nodiscard]] constexpr TimeUs close_time_us(Timeframe tf) const noexcept {
        return open_time_us + timeframe_us(tf);
    }
    [[nodiscard]] constexpr Points range_pts() const noexcept { return high - low; }
};

// Bucket boundary for a timestamp: bars align to epoch multiples of the
// timeframe.
//
// Note for D1: this aligns to UTC midnight, which is NOT where a broker's daily
// bar starts (typically 17:00 New York, i.e. the server day). Session-correct
// daily bars need the server clock offset from config/symbol_spec.json, and
// arrive with the session features in Phase 4. Until then, treat D1 here as a
// chart convenience, not something to compute features from.
constexpr TimeUs bar_open_for(TimeUs ts_us, Timeframe tf) noexcept {
    const TimeUs step = timeframe_us(tf);
    // Floor division that stays correct for pre-1970 timestamps.
    TimeUs q = ts_us / step;
    if (ts_us < 0 && q * step != ts_us) --q;
    return q * step;
}

class BarSeries {
public:
    BarSeries() = default;

    // Build bars over [from_us, to_us) from the store.
    //
    // Buckets with no ticks produce no bar. Gold closes every weekend and the
    // holidays are real; emitting empty bars across them would put a flat line
    // in the middle of every chart and, worse, feed zero-range bars to any
    // indicator built on this.
    static BarSeries build(const TickStore& store, Timeframe tf, TimeUs from_us,
                           TimeUs to_us);

    // Complete bars. Safe to make decisions on.
    [[nodiscard]] std::span<const Bar> closed() const noexcept {
        return std::span<const Bar>(bars_).first(closed_count_);
    }

    // The bar still being built at to_us, if any. NOT safe to act on: it will
    // keep changing. Present so a live chart can draw the current candle.
    [[nodiscard]] std::optional<Bar> forming() const noexcept {
        if (closed_count_ == bars_.size()) return std::nullopt;
        return bars_.back();
    }

    // Every bar including any forming one — for rendering only.
    [[nodiscard]] std::span<const Bar> all_for_display() const noexcept { return bars_; }

    [[nodiscard]] Timeframe timeframe() const noexcept { return tf_; }
    [[nodiscard]] std::size_t size() const noexcept { return bars_.size(); }
    [[nodiscard]] bool empty() const noexcept { return bars_.empty(); }

private:
    std::vector<Bar> bars_;
    std::size_t      closed_count_ = 0;
    Timeframe        tf_ = Timeframe::M15;
};

}  // namespace xau
