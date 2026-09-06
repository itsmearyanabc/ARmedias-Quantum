#include "xau/zoo.hpp"

#include "xau/session.hpp"

#include <utility>

#include <algorithm>
#include <cmath>

namespace xau {
namespace {

// Same contract as rules.cpp: a stop that ATR cannot support is a reason to
// skip the trade, never a reason to invent a level.
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

double mean_close(std::span<const Bar> h, std::size_t n) noexcept {
    if (h.empty()) return 0.0;
    const std::size_t k = std::min(n, h.size());
    double            s = 0.0;
    for (std::size_t i = h.size() - k; i < h.size(); ++i) s += static_cast<double>(h[i].close);
    return s / static_cast<double>(k);
}

// Wilder-free RSI over a short lookback. For period 2 the classic smoothing
// makes no difference and the simple form is easier to verify by hand.
double rsi(std::span<const Bar> h, std::size_t period) noexcept {
    if (h.size() <= period) return 50.0;
    double up = 0.0, down = 0.0;
    for (std::size_t i = h.size() - period; i < h.size(); ++i) {
        const double d = static_cast<double>(h[i].close) - static_cast<double>(h[i - 1].close);
        if (d > 0.0) up += d;
        else down -= d;
    }
    const double total = up + down;
    if (!(total > 0.0)) return 50.0;
    return 100.0 * up / total;
}

// A session filter is only meaningful while a bar is shorter than the window
// it is filtering on. A D1 bar closes at one instant that spans every hour of
// the day, so "trade between 07:00 and 20:00" can never match it -- and the
// strategy goes silent instead of erroring. That cost us nine of twelve
// strategies in the first D1 sweep, all reported as zero trades as though the
// hypothesis had been tested and found empty.
bool in_window(TimeUs us, int from_h, int to_h, Timeframe tf) noexcept {
    if (timeframe_us(tf) >= 4 * kUsPerHour) return true;   // bar spans the window
    const int h = utc_hour(us);
    return h >= from_h && h < to_h;
}

}  // namespace

// ---------------------------------------------------------------------------
// 5. Bollinger mean reversion
// ---------------------------------------------------------------------------
BollingerReversion::BollingerReversion() : BollingerReversion(Config{}) {}
BollingerReversion::BollingerReversion(const Config& c) : cfg_(c), atr_(c.atr_period) {}

std::size_t BollingerReversion::warmup_bars() const noexcept {
    return std::max(cfg_.period, cfg_.atr_period) + 2;
}

Decision BollingerReversion::on_bar(const BarContext& c) {
    atr_.update(c.bar());
    if (should_time_exit(c, cfg_.exits)) return Decision::close("time");
    if (c.position.is_open()) return Decision::hold();
    if (!c.has(cfg_.period)) return Decision::hold();
    if (!in_window(c.now_us, cfg_.trade_from_hour, cfg_.trade_until_hour, c.tf)) {
        return Decision::hold();
    }

    const double mid = mean_close(c.history, cfg_.period);
    const double sd = stdev_close(c.history, cfg_.period);
    if (!(sd > 0.0)) return Decision::hold();

    const double close = static_cast<double>(c.bar().close);
    const double z = (close - mid) / sd;

    Points sl = 0, tp = 0;
    if (!atr_levels(atr_.value(), cfg_.sl_atr, cfg_.tp_r, sl, tp)) return Decision::hold();

    // Fade the stretch. Long when price is far BELOW the mean.
    if (z <= -cfg_.entry_sd) return entry(Side::Long, sl, tp, cfg_.lots, "bb_low");
    if (z >= cfg_.entry_sd) return entry(Side::Short, sl, tp, cfg_.lots, "bb_high");
    return Decision::hold();
}

// ---------------------------------------------------------------------------
// 6. RSI-2 extreme
// ---------------------------------------------------------------------------
Rsi2Extreme::Rsi2Extreme() : Rsi2Extreme(Config{}) {}
Rsi2Extreme::Rsi2Extreme(const Config& c) : cfg_(c), atr_(c.atr_period) {}

std::size_t Rsi2Extreme::warmup_bars() const noexcept {
    return std::max({cfg_.rsi_period, cfg_.atr_period, cfg_.exit_ema}) + 2;
}

Decision Rsi2Extreme::on_bar(const BarContext& c) {
    atr_.update(c.bar());
    if (should_time_exit(c, cfg_.exits)) return Decision::close("time");
    if (c.position.is_open()) return Decision::hold();
    if (!c.has(cfg_.rsi_period + 1)) return Decision::hold();

    const double r = rsi(c.history, cfg_.rsi_period);

    Points sl = 0, tp = 0;
    if (!atr_levels(atr_.value(), cfg_.sl_atr, cfg_.tp_r, sl, tp)) return Decision::hold();

    if (r <= cfg_.oversold) return entry(Side::Long, sl, tp, cfg_.lots, "rsi_low");
    if (r >= cfg_.overbought) return entry(Side::Short, sl, tp, cfg_.lots, "rsi_high");
    return Decision::hold();
}

// ---------------------------------------------------------------------------
// 7. Momentum continuation
// ---------------------------------------------------------------------------
MomentumContinuation::MomentumContinuation() : MomentumContinuation(Config{}) {}
MomentumContinuation::MomentumContinuation(const Config& c) : cfg_(c), atr_(c.atr_period) {}

std::size_t MomentumContinuation::warmup_bars() const noexcept {
    return std::max(cfg_.lookback, cfg_.atr_period) + 2;
}

Decision MomentumContinuation::on_bar(const BarContext& c) {
    atr_.update(c.bar());
    if (should_time_exit(c, cfg_.exits)) return Decision::close("time");
    if (c.position.is_open()) return Decision::hold();
    if (!c.has(cfg_.lookback)) return Decision::hold();
    if (!in_window(c.now_us, cfg_.trade_from_hour, cfg_.trade_until_hour, c.tf)) {
        return Decision::hold();
    }

    const double atr = atr_.value();
    if (!(atr > 0.0)) return Decision::hold();

    const double moved = static_cast<double>(c.bar().close) -
                         static_cast<double>(c.ago(cfg_.lookback).close);
    const double in_atr = moved / atr;

    Points sl = 0, tp = 0;
    if (!atr_levels(atr, cfg_.sl_atr, cfg_.tp_r, sl, tp)) return Decision::hold();

    if (in_atr >= cfg_.min_move_atr) return entry(Side::Long, sl, tp, cfg_.lots, "mom_up");
    if (in_atr <= -cfg_.min_move_atr) return entry(Side::Short, sl, tp, cfg_.lots, "mom_dn");
    return Decision::hold();
}

// ---------------------------------------------------------------------------
// 8. Session-open drift
// ---------------------------------------------------------------------------
SessionDrift::SessionDrift() : SessionDrift(Config{}) {}
SessionDrift::SessionDrift(const Config& c) : cfg_(c), atr_(c.atr_period) {}

std::size_t SessionDrift::warmup_bars() const noexcept {
    return cfg_.atr_period + cfg_.confirm_bars + 2;
}

Decision SessionDrift::on_bar(const BarContext& c) {
    atr_.update(c.bar());
    if (should_time_exit(c, cfg_.exits)) return Decision::close("time");
    if (c.position.is_open()) return Decision::hold();
    if (!c.has(cfg_.confirm_bars + 1)) return Decision::hold();

    const TimeUs day = day_start(c.now_us);
    if (armed_day_ == day) return Decision::hold();   // one trade per day

    // Only in the bars just after the session opens.
    // Same reasoning as in_window: on bars of 4h or more the session hour is
    // not identifiable, so this strategy simply does not apply there.
    if (timeframe_us(c.tf) >= 4 * kUsPerHour) return Decision::hold();
    const int h = utc_hour(c.now_us);
    if (h != cfg_.session_hour) return Decision::hold();

    // Direction of the first confirm_bars bars of the session.
    const double first = static_cast<double>(c.ago(cfg_.confirm_bars).open);
    const double now = static_cast<double>(c.bar().close);
    const double moved = now - first;
    if (std::abs(moved) < 1.0) return Decision::hold();

    Points sl = 0, tp = 0;
    if (!atr_levels(atr_.value(), cfg_.sl_atr, cfg_.tp_r, sl, tp)) return Decision::hold();

    armed_day_ = day;
    return entry(moved > 0.0 ? Side::Long : Side::Short, sl, tp, cfg_.lots, "session_drift");
}

// ---------------------------------------------------------------------------
// 9. Spread-aware liquidity fade
// ---------------------------------------------------------------------------
LiquidityFade::LiquidityFade() : LiquidityFade(Config{}) {}
LiquidityFade::LiquidityFade(const Config& c) : cfg_(c), atr_(c.atr_period) {}

std::size_t LiquidityFade::warmup_bars() const noexcept {
    return std::max(cfg_.lookback, cfg_.atr_period) + 2;
}

Decision LiquidityFade::on_bar(const BarContext& c) {
    atr_.update(c.bar());
    if (should_time_exit(c, cfg_.exits)) return Decision::close("time");
    if (c.position.is_open()) return Decision::hold();
    if (!c.has(cfg_.lookback)) return Decision::hold();

    // Recent means, excluding the current bar so the comparison is against
    // history rather than against a window containing the event itself.
    double spread_sum = 0.0, range_sum = 0.0;
    for (std::size_t i = 1; i <= cfg_.lookback; ++i) {
        spread_sum += static_cast<double>(c.ago(i).spread_mean_pts);
        range_sum += static_cast<double>(c.ago(i).range_pts());
    }
    const double mean_spread = spread_sum / static_cast<double>(cfg_.lookback);
    const double mean_range = range_sum / static_cast<double>(cfg_.lookback);
    if (!(mean_spread > 0.0) || !(mean_range > 0.0)) return Decision::hold();

    const Bar&   b = c.bar();
    const double spread_ratio = static_cast<double>(b.spread_mean_pts) / mean_spread;
    const double range_ratio = static_cast<double>(b.range_pts()) / mean_range;

    // Spread blew out but the range did not: liquidity left, news did not
    // arrive. Fade the bar's own direction.
    if (spread_ratio < cfg_.spread_spike) return Decision::hold();
    if (range_ratio > cfg_.max_range_expansion) return Decision::hold();

    Points sl = 0, tp = 0;
    if (!atr_levels(atr_.value(), cfg_.sl_atr, cfg_.tp_r, sl, tp)) return Decision::hold();

    const bool bar_up = b.close > b.open;
    return entry(bar_up ? Side::Short : Side::Long, sl, tp, cfg_.lots, "liq_fade");
}

// ---------------------------------------------------------------------------
// 10. Inside-bar continuation
// ---------------------------------------------------------------------------
InsideBarBreak::InsideBarBreak() : InsideBarBreak(Config{}) {}
InsideBarBreak::InsideBarBreak(const Config& c) : cfg_(c), atr_(c.atr_period) {}

std::size_t InsideBarBreak::warmup_bars() const noexcept { return cfg_.atr_period + 3; }

Decision InsideBarBreak::on_bar(const BarContext& c) {
    atr_.update(c.bar());
    if (should_time_exit(c, cfg_.exits)) return Decision::close("time");
    if (c.position.is_open()) return Decision::hold();
    if (!c.has(2)) return Decision::hold();

    const Bar& b = c.bar();

    if (armed_) {
        ++bars_since_arm_;
        if (bars_since_arm_ > cfg_.expiry_bars) {
            armed_ = false;
        } else if (in_window(c.now_us, cfg_.trade_from_hour, cfg_.trade_until_hour, c.tf)) {
            Points sl = 0, tp = 0;
            if (atr_levels(atr_.value(), cfg_.sl_atr, cfg_.tp_r, sl, tp)) {
                if (b.close > mother_high_) {
                    armed_ = false;
                    return entry(Side::Long, sl, tp, cfg_.lots, "inside_up");
                }
                if (b.close < mother_low_) {
                    armed_ = false;
                    return entry(Side::Short, sl, tp, cfg_.lots, "inside_dn");
                }
            }
        }
    }

    // Arm on a fresh inside bar: this bar's range sits entirely inside the
    // previous bar's.
    const Bar& prev = c.ago(1);
    if (b.high <= prev.high && b.low >= prev.low) {
        armed_ = true;
        bars_since_arm_ = 0;
        mother_high_ = prev.high;
        mother_low_ = prev.low;
    }
    return Decision::hold();
}

// ---------------------------------------------------------------------------
// 11. Weekend gap fade
// ---------------------------------------------------------------------------
WeekendGapFade::WeekendGapFade() : WeekendGapFade(Config{}) {}
WeekendGapFade::WeekendGapFade(const Config& c) : cfg_(c), atr_(c.atr_period) {}

std::size_t WeekendGapFade::warmup_bars() const noexcept { return cfg_.atr_period + 2; }

Decision WeekendGapFade::on_bar(const BarContext& c) {
    atr_.update(c.bar());
    const TimeUs prev_us = last_bar_us_;
    last_bar_us_ = c.now_us;

    if (should_time_exit(c, cfg_.exits)) return Decision::close("time");
    if (c.position.is_open()) return Decision::hold();
    if (!c.has(1) || prev_us == 0) return Decision::hold();

    // A real weekend: more than a day of wall clock with no bars. Using the
    // clock rather than the weekday handles holidays for free.
    const TimeUs elapsed = c.now_us - prev_us;
    if (elapsed < 24 * kUsPerHour) return Decision::hold();

    const double atr = atr_.value();
    if (!(atr > 0.0)) return Decision::hold();

    const double gap = static_cast<double>(c.bar().open) - static_cast<double>(c.ago(1).close);
    if (std::abs(gap) < cfg_.min_gap_atr * atr) return Decision::hold();

    Points sl = 0, tp = 0;
    if (!atr_levels(atr, cfg_.sl_atr, cfg_.tp_r, sl, tp)) return Decision::hold();

    // Fade it: a gap up is sold, expecting a fill back down.
    return entry(gap > 0.0 ? Side::Short : Side::Long, sl, tp, cfg_.lots, "gap_fade");
}

// ---------------------------------------------------------------------------
// 12. Adaptive trend
// ---------------------------------------------------------------------------
AdaptiveTrend::AdaptiveTrend() : AdaptiveTrend(Config{}) {}
AdaptiveTrend::AdaptiveTrend(const Config& c)
    : cfg_(c), atr_(c.atr_period), fast_(c.fast_ema), slow_(c.slow_ema) {}

std::size_t AdaptiveTrend::warmup_bars() const noexcept {
    return std::max({cfg_.slow_ema, cfg_.vol_lookback, cfg_.atr_period}) + 2;
}

Decision AdaptiveTrend::on_bar(const BarContext& c) {
    const Bar& b = c.bar();
    atr_.update(b);
    fast_.update(static_cast<double>(b.close));
    slow_.update(static_cast<double>(b.close));

    if (should_time_exit(c, cfg_.exits)) return Decision::close("time");
    if (!fast_.ready() || !slow_.ready()) return Decision::hold();

    const bool above = fast_.value() > slow_.value();
    const bool crossed_up = primed_ && above && !was_above_;
    const bool crossed_dn = primed_ && !above && was_above_;
    was_above_ = above;
    primed_ = true;

    if (c.position.is_open()) return Decision::hold();
    if (!c.has(cfg_.vol_lookback)) return Decision::hold();
    if (!in_window(c.now_us, cfg_.trade_from_hour, cfg_.trade_until_hour, c.tf)) {
        return Decision::hold();
    }

    // The regime gate: only trade the cross when volatility is expanding.
    // A trend system without this bleeds through every quiet range, which is
    // most of gold's life.
    double range_sum = 0.0;
    for (std::size_t i = 1; i <= cfg_.vol_lookback; ++i) {
        range_sum += static_cast<double>(c.ago(i).range_pts());
    }
    const double mean_range = range_sum / static_cast<double>(cfg_.vol_lookback);
    const double atr = atr_.value();
    if (!(mean_range > 0.0) || !(atr > 0.0)) return Decision::hold();
    if (atr / mean_range < cfg_.min_vol_ratio) return Decision::hold();

    Points sl = 0, tp = 0;
    if (!atr_levels(atr, cfg_.sl_atr, cfg_.tp_r, sl, tp)) return Decision::hold();

    if (crossed_up) return entry(Side::Long, sl, tp, cfg_.lots, "trend_up");
    if (crossed_dn) return entry(Side::Short, sl, tp, cfg_.lots, "trend_dn");
    return Decision::hold();
}

// ---------------------------------------------------------------------------
// 13. Regime-gated wrapper
// ---------------------------------------------------------------------------
RegimeGated::RegimeGated(std::unique_ptr<Strategy> inner, unsigned allowed_mask,
                         const char* label)
    : inner_(std::move(inner)), allowed_(allowed_mask), label_(label) {}

std::size_t RegimeGated::warmup_bars() const noexcept {
    // The regime classifier needs its own history, and it is the longer of the
    // two requirements -- gating on a regime computed from eight bars would be
    // gating on noise.
    const RegimeConfig rc{};
    return std::max(inner_->warmup_bars(), rc.vol_lookback + rc.atr_period + 2);
}

Decision RegimeGated::on_bar(const BarContext& c) {
    // The inner strategy sees EVERY bar regardless of the gate. Its indicators
    // are stateful; feeding it only the bars we like would desynchronise every
    // EMA and ATR it owns and quietly change what it computes.
    Decision d = inner_->on_bar(c);
    if (d.kind != Decision::Kind::Enter) return d;

    const Regime r = classify(c.history);
    if ((allowed_ & (1u << r.index())) == 0u) return Decision::hold();
    return d;
}

}  // namespace xau
