// Incremental indicators.
//
// Stateful and updated once per bar rather than recomputed from a window, so a
// 200-period EMA costs the same as a 5-period one. Over 350k bars the
// difference between O(1) and O(period) per bar is the difference between a
// backtest you rerun after every change and one you schedule.
//
// This is not the Phase 4 feature engine. These are the handful of primitives
// the Phase 3 rule baselines need; the full feature set, with its
// stationarity and point-in-time requirements, is a later and larger job.
#pragma once

#include "xau/bar.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace xau {

// Wilder's Average True Range, in points.
//
// True range uses the previous close, so the first bar has no predecessor and
// falls back to high-low. Seeded with a simple average over the first `period`
// bars, then smoothed — which is Wilder's original definition, not an EMA with
// the same period.
class Atr {
public:
    explicit Atr(std::size_t period) noexcept : period_(period ? period : 1) {}

    void update(const Bar& b) noexcept {
        const double hl = static_cast<double>(b.high - b.low);
        double tr = hl;
        if (have_prev_) {
            tr = std::max({hl, std::fabs(static_cast<double>(b.high) - prev_close_),
                           std::fabs(static_cast<double>(b.low) - prev_close_)});
        }
        if (n_ < period_) {
            sum_ += tr;
            ++n_;
            if (n_ == period_) value_ = sum_ / static_cast<double>(period_);
        } else {
            value_ = (value_ * static_cast<double>(period_ - 1) + tr) /
                     static_cast<double>(period_);
        }
        prev_close_ = static_cast<double>(b.close);
        have_prev_ = true;
    }

    [[nodiscard]] bool ready() const noexcept { return n_ >= period_; }
    [[nodiscard]] double value() const noexcept { return value_; }
    [[nodiscard]] Points points() const noexcept { return static_cast<Points>(value_); }

private:
    std::size_t period_;
    std::size_t n_ = 0;
    double      sum_ = 0.0;
    double      value_ = 0.0;
    double      prev_close_ = 0.0;
    bool        have_prev_ = false;
};

// Exponential moving average, seeded with a simple average over the first
// `period` samples so it does not spend its first hundred bars converging from
// an arbitrary starting point.
class Ema {
public:
    explicit Ema(std::size_t period) noexcept
        : period_(period ? period : 1),
          k_(2.0 / (static_cast<double>(period ? period : 1) + 1.0)) {}

    void update(double x) noexcept {
        if (n_ < period_) {
            sum_ += x;
            ++n_;
            if (n_ == period_) value_ = sum_ / static_cast<double>(period_);
        } else {
            value_ += k_ * (x - value_);
        }
    }

    [[nodiscard]] bool ready() const noexcept { return n_ >= period_; }
    [[nodiscard]] double value() const noexcept { return value_; }

private:
    std::size_t period_;
    double      k_;
    std::size_t n_ = 0;
    double      sum_ = 0.0;
    double      value_ = 0.0;
};

// Highest high over `count` bars ending `skip` bars back from the newest.
// skip=1 excludes the bar that just closed, which is what a breakout test
// wants: "did this bar break the previous 20 bars' high", not "including
// itself", which is trivially true.
[[nodiscard]] inline Points highest_high(std::span<const Bar> h, std::size_t count,
                                         std::size_t skip = 0) noexcept {
    if (h.size() <= skip || count == 0) return 0;
    const std::size_t end = h.size() - skip;
    const std::size_t begin = (end > count) ? end - count : 0;
    Points best = h[begin].high;
    for (std::size_t i = begin + 1; i < end; ++i) best = std::max(best, h[i].high);
    return best;
}

[[nodiscard]] inline Points lowest_low(std::span<const Bar> h, std::size_t count,
                                       std::size_t skip = 0) noexcept {
    if (h.size() <= skip || count == 0) return 0;
    const std::size_t end = h.size() - skip;
    const std::size_t begin = (end > count) ? end - count : 0;
    Points best = h[begin].low;
    for (std::size_t i = begin + 1; i < end; ++i) best = std::min(best, h[i].low);
    return best;
}

// True when the newest bar's range is the narrowest of the last `count` bars.
// NR7 is the classic form: compression that often precedes expansion.
[[nodiscard]] inline bool is_narrowest_range(std::span<const Bar> h,
                                             std::size_t count) noexcept {
    if (h.size() < count || count == 0) return false;
    const Points r = h.back().range_pts();
    for (std::size_t i = h.size() - count; i + 1 < h.size(); ++i) {
        if (h[i].range_pts() <= r) return false;
    }
    return true;
}

// Sample standard deviation of the last `count` closes, in points.
[[nodiscard]] inline double stdev_close(std::span<const Bar> h, std::size_t count) noexcept {
    if (h.size() < count || count < 2) return 0.0;
    const std::size_t begin = h.size() - count;
    double mean = 0.0;
    for (std::size_t i = begin; i < h.size(); ++i) mean += static_cast<double>(h[i].close);
    mean /= static_cast<double>(count);
    double var = 0.0;
    for (std::size_t i = begin; i < h.size(); ++i) {
        const double d = static_cast<double>(h[i].close) - mean;
        var += d * d;
    }
    return std::sqrt(var / static_cast<double>(count - 1));
}

}  // namespace xau
