#include "xau/rules.hpp"

#include <algorithm>

namespace xau {
namespace {

// Stop and target from ATR, as points. Returns false when ATR is too small to
// produce a usable stop — which happens in dead sessions and on the first bars
// after a gap, and is a reason to skip the trade rather than to invent a stop.
bool atr_levels(double atr, double sl_atr, double tp_r, Points& sl, Points& tp) noexcept {
    if (!(atr > 0.0)) return false;
    sl = static_cast<Points>(sl_atr * atr);
    tp = static_cast<Points>(tp_r * sl_atr * atr);
    return sl > 0;
}

Decision entry(Side side, Points sl, Points tp, double lots, const char* why) noexcept {
    Decision d = Decision::enter(side, sl, tp, why);
    d.lots = lots;
    return d;
}

}  // namespace

bool should_time_exit(const BarContext& c, const ExitRules& r) noexcept {
    if (!c.position.is_open()) return false;

    if (r.max_hold_bars != 0) {
        const TimeUs held = c.now_us - c.position.entry_ts;
        if (held >= static_cast<TimeUs>(r.max_hold_bars) * timeframe_us(c.tf)) return true;
    }

    const int h = utc_hour(c.now_us);
    if (r.flat_by_hour >= 0 && h >= r.flat_by_hour) return true;

    // Friday: out before the weekend. Gold gaps over the close and a stop
    // cannot protect you across a market that is not trading.
    if (utc_weekday(c.now_us) == 5 && r.friday_flat_hour >= 0 && h >= r.friday_flat_hour) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1. London opening range
// ---------------------------------------------------------------------------

LondonOpeningRange::LondonOpeningRange() : LondonOpeningRange(Config{}) {}

LondonOpeningRange::LondonOpeningRange(const Config& c) : cfg_(c), atr_(c.atr_period) {}

std::size_t LondonOpeningRange::warmup_bars() const noexcept { return cfg_.atr_period + 2; }

Decision LondonOpeningRange::on_bar(const BarContext& c) {
    const Bar& b = c.bar();
    atr_.update(b);   // every bar, position or not, or the indicator desyncs

    const TimeUs day = day_start(b.open_time_us);
    if (!have_day_ || day != cur_day_) {
        cur_day_ = day;
        have_day_ = true;
        or_valid_ = false;
        traded_today_ = false;
    }

    // While the range is still forming there is nothing to break out of.
    if (in_hours(b.open_time_us, cfg_.range_from_hour, cfg_.range_to_hour)) {
        if (!or_valid_) {
            or_high_ = b.high;
            or_low_ = b.low;
            or_valid_ = true;
        } else {
            or_high_ = std::max(or_high_, b.high);
            or_low_ = std::min(or_low_, b.low);
        }
        return Decision::hold();
    }

    if (c.position.is_open()) {
        return should_time_exit(c, cfg_.exits) ? Decision::close("time/session exit")
                                               : Decision::hold();
    }

    if (!or_valid_ || traded_today_ || !atr_.ready()) return Decision::hold();

    const int h = utc_hour(b.open_time_us);
    if (h < cfg_.range_to_hour || h >= cfg_.trade_until_hour) return Decision::hold();

    const double atr = atr_.value();
    const double ratio = static_cast<double>(or_high_ - or_low_) / (atr > 0.0 ? atr : 1.0);
    if (ratio < cfg_.min_range_atr || ratio > cfg_.max_range_atr) return Decision::hold();

    Points sl = 0, tp = 0;
    if (!atr_levels(atr, cfg_.sl_atr, cfg_.tp_r, sl, tp)) return Decision::hold();

    if (b.close > or_high_) {
        traded_today_ = true;
        return entry(Side::Long, sl, tp, cfg_.lots, "london OR break up");
    }
    if (b.close < or_low_) {
        traded_today_ = true;
        return entry(Side::Short, sl, tp, cfg_.lots, "london OR break down");
    }
    return Decision::hold();
}

// ---------------------------------------------------------------------------
// 2. Asia range breakout
// ---------------------------------------------------------------------------

AsiaRangeBreakout::AsiaRangeBreakout() : AsiaRangeBreakout(Config{}) {}

AsiaRangeBreakout::AsiaRangeBreakout(const Config& c) : cfg_(c), atr_(c.atr_period) {}

std::size_t AsiaRangeBreakout::warmup_bars() const noexcept { return cfg_.atr_period + 2; }

Decision AsiaRangeBreakout::on_bar(const BarContext& c) {
    const Bar& b = c.bar();
    atr_.update(b);

    // The Asia window wraps midnight, so the session key is the day the window
    // *started*, not the calendar day of the bar. Both halves of one session
    // therefore share a key, and it rolls when the next Asia session opens.
    const TimeUs key = session_day(b.open_time_us, cfg_.asia_from_hour);
    if (!have_key_ || key != cur_key_) {
        cur_key_ = key;
        have_key_ = true;
        range_valid_ = false;
        traded_ = false;
    }

    if (in_hours(b.open_time_us, cfg_.asia_from_hour, cfg_.asia_to_hour)) {
        if (!range_valid_) {
            hi_ = b.high;
            lo_ = b.low;
            range_valid_ = true;
        } else {
            hi_ = std::max(hi_, b.high);
            lo_ = std::min(lo_, b.low);
        }
        return Decision::hold();
    }

    if (c.position.is_open()) {
        return should_time_exit(c, cfg_.exits) ? Decision::close("time/session exit")
                                               : Decision::hold();
    }

    if (!range_valid_ || traded_ || !atr_.ready()) return Decision::hold();

    const int h = utc_hour(b.open_time_us);
    if (h < cfg_.asia_to_hour || h >= cfg_.trade_until_hour) return Decision::hold();

    const double atr = atr_.value();
    const double ratio = static_cast<double>(hi_ - lo_) / (atr > 0.0 ? atr : 1.0);
    if (ratio < cfg_.min_range_atr || ratio > cfg_.max_range_atr) return Decision::hold();

    Points sl = 0, tp = 0;
    if (!atr_levels(atr, cfg_.sl_atr, cfg_.tp_r, sl, tp)) return Decision::hold();

    if (b.close > hi_) {
        traded_ = true;
        return entry(Side::Long, sl, tp, cfg_.lots, "asia range break up");
    }
    if (b.close < lo_) {
        traded_ = true;
        return entry(Side::Short, sl, tp, cfg_.lots, "asia range break down");
    }
    return Decision::hold();
}

// ---------------------------------------------------------------------------
// 3. Trend pullback
// ---------------------------------------------------------------------------

TrendPullback::TrendPullback() : TrendPullback(Config{}) {}

TrendPullback::TrendPullback(const Config& c)
    : cfg_(c), atr_(c.atr_period), slow_(c.trend_ema), fast_(c.pullback_ema) {}

std::size_t TrendPullback::warmup_bars() const noexcept { return cfg_.trend_ema + 2; }

Decision TrendPullback::on_bar(const BarContext& c) {
    const Bar& b = c.bar();
    const double close = static_cast<double>(b.close);
    atr_.update(b);
    slow_.update(close);
    fast_.update(close);

    if (c.position.is_open()) {
        return should_time_exit(c, cfg_.exits) ? Decision::close("time/session exit")
                                               : Decision::hold();
    }
    if (!atr_.ready() || !slow_.ready() || !fast_.ready() || !c.has(1)) return Decision::hold();

    const int h = utc_hour(b.open_time_us);
    if (h < cfg_.trade_from_hour || h >= cfg_.trade_until_hour) {
        armed_long_ = armed_short_ = false;   // do not carry a setup across a session
        return Decision::hold();
    }

    const double atr = atr_.value();
    const double tol = cfg_.pullback_atr * atr;
    const bool   uptrend = close > slow_.value();
    const bool   downtrend = close < slow_.value();

    // Arm on the pullback into the fast average; disarm the moment the trend
    // flips, so a stale setup cannot fire on the wrong side.
    if (!uptrend) armed_long_ = false;
    if (!downtrend) armed_short_ = false;
    if (uptrend && static_cast<double>(b.low) <= fast_.value() + tol) armed_long_ = true;
    if (downtrend && static_cast<double>(b.high) >= fast_.value() - tol) armed_short_ = true;

    Points sl = 0, tp = 0;
    if (!atr_levels(atr, cfg_.sl_atr, cfg_.tp_r, sl, tp)) return Decision::hold();

    // Fire on resumption, not on the pullback itself.
    const Bar& prev = c.ago(1);
    if (armed_long_ && b.close > prev.high) {
        armed_long_ = false;
        return entry(Side::Long, sl, tp, cfg_.lots, "trend pullback long");
    }
    if (armed_short_ && b.close < prev.low) {
        armed_short_ = false;
        return entry(Side::Short, sl, tp, cfg_.lots, "trend pullback short");
    }
    return Decision::hold();
}

// ---------------------------------------------------------------------------
// 4. Volatility compression
// ---------------------------------------------------------------------------

VolatilityCompression::VolatilityCompression() : VolatilityCompression(Config{}) {}

VolatilityCompression::VolatilityCompression(const Config& c) : cfg_(c), atr_(c.atr_period) {}

std::size_t VolatilityCompression::warmup_bars() const noexcept {
    return std::max(cfg_.atr_period, cfg_.nr_lookback) + 2;
}

Decision VolatilityCompression::on_bar(const BarContext& c) {
    const Bar& b = c.bar();
    atr_.update(b);

    if (c.position.is_open()) {
        armed_ = false;
        return should_time_exit(c, cfg_.exits) ? Decision::close("time/session exit")
                                               : Decision::hold();
    }
    if (!atr_.ready()) return Decision::hold();

    // Compression that does not resolve quickly usually just becomes more
    // compression, so a setup ages out rather than waiting indefinitely.
    if (armed_) {
        ++bars_since_arm_;
        if (bars_since_arm_ > cfg_.expiry_bars) armed_ = false;
    }

    // A fresh compression bar replaces any older setup, and is never traded on
    // its own close — the breakout is the bar *after*.
    if (is_narrowest_range(c.history, cfg_.nr_lookback)) {
        armed_ = true;
        bars_since_arm_ = 0;
        arm_high_ = b.high;
        arm_low_ = b.low;
        return Decision::hold();
    }

    if (!armed_) return Decision::hold();

    const int h = utc_hour(b.open_time_us);
    if (h < cfg_.trade_from_hour || h >= cfg_.trade_until_hour) return Decision::hold();

    Points sl = 0, tp = 0;
    if (!atr_levels(atr_.value(), cfg_.sl_atr, cfg_.tp_r, sl, tp)) return Decision::hold();

    if (b.close > arm_high_) {
        armed_ = false;
        return entry(Side::Long, sl, tp, cfg_.lots, "squeeze break up");
    }
    if (b.close < arm_low_) {
        armed_ = false;
        return entry(Side::Short, sl, tp, cfg_.lots, "squeeze break down");
    }
    return Decision::hold();
}

}  // namespace xau
