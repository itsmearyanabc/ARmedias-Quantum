#include "xau/features.hpp"

#include "xau/indicators.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace xau {

namespace {

constexpr std::size_t kEmaPeriod = 20;
constexpr std::size_t kAtrPeriod = 14;
constexpr std::size_t kLookback = 20;
constexpr int         kHtfStride = 4;

// A division that cannot produce inf or NaN. Every feature here divides by
// something that is usually positive and occasionally zero -- a dead bar, a
// frozen session -- and one NaN poisons a whole tree ensemble silently.
constexpr double safe_div(double num, double den, double fallback = 0.0) noexcept {
    return (den > 1e-12 || den < -1e-12) ? num / den : fallback;
}

double atr_over(std::span<const Bar> h, std::size_t period) noexcept {
    if (h.size() < 2) return 0.0;
    const std::size_t n = std::min(period, h.size() - 1);
    double            sum = 0.0;
    for (std::size_t k = h.size() - n; k < h.size(); ++k) {
        const double hi = static_cast<double>(h[k].high);
        const double lo = static_cast<double>(h[k].low);
        const double pc = static_cast<double>(h[k - 1].close);
        sum += std::max({hi - lo, std::abs(hi - pc), std::abs(lo - pc)});
    }
    return n > 0 ? sum / static_cast<double>(n) : 0.0;
}

double ema_close(std::span<const Bar> h, std::size_t period) noexcept {
    if (h.empty()) return 0.0;
    const double      k = 2.0 / (static_cast<double>(period) + 1.0);
    const std::size_t n = std::min(period * 3, h.size());
    double            e = static_cast<double>(h[h.size() - n].close);
    for (std::size_t i = h.size() - n + 1; i < h.size(); ++i) {
        e = static_cast<double>(h[i].close) * k + e * (1.0 - k);
    }
    return e;
}

// Return over `back` bars, from the close `back` bars ago to the current close.
double ret_pts(std::span<const Bar> h, std::size_t back) noexcept {
    if (h.size() <= back) return 0.0;
    return static_cast<double>(h.back().close) -
           static_cast<double>(h[h.size() - 1 - back].close);
}

}  // namespace

std::string_view feature_name(Feat f) noexcept {
    switch (f) {
        case Feat::Ret1Atr: return "ret1_atr";
        case Feat::Ret4Atr: return "ret4_atr";
        case Feat::Ret16Atr: return "ret16_atr";
        case Feat::EmaDistAtr: return "ema_dist_atr";
        case Feat::RangePosition: return "range_position";
        case Feat::RangeExpansion: return "range_expansion";
        case Feat::BodyFraction: return "body_fraction";
        case Feat::SpreadAtr: return "spread_atr";
        case Feat::SpreadMaxRatio: return "spread_max_ratio";
        case Feat::TickIntensity: return "tick_intensity";
        case Feat::HtfRetAtr: return "htf_ret_atr";
        case Feat::HtfAlign: return "htf_align";
        case Feat::HourSin: return "hour_sin";
        case Feat::HourCos: return "hour_cos";
        case Feat::SessionId: return "session_id";
        case Feat::MinutesIntoSession: return "minutes_into_session";
        case Feat::IsRollover: return "is_rollover";
        case Feat::COUNT: break;
    }
    return "?";
}

FeatureRow compute_row(std::span<const Bar> history, Timeframe tf) noexcept {
    FeatureRow r{};
    r.fill(0.0);
    if (history.empty()) return r;

    const Bar&   cur = history.back();
    const double atr = atr_over(history, kAtrPeriod);
    const auto   at = [&](Feat f) -> double& { return r[static_cast<std::size_t>(f)]; };

    // -- price ------------------------------------------------------------
    at(Feat::Ret1Atr) = safe_div(ret_pts(history, 1), atr);
    at(Feat::Ret4Atr) = safe_div(ret_pts(history, 4), atr);
    at(Feat::Ret16Atr) = safe_div(ret_pts(history, 16), atr);
    at(Feat::EmaDistAtr) =
        safe_div(static_cast<double>(cur.close) - ema_close(history, kEmaPeriod), atr);

    const std::size_t look = std::min(kLookback, history.size());
    Points            hi = cur.high;
    Points            lo = cur.low;
    double            range_sum = 0.0;
    double            tick_sum = 0.0;
    for (std::size_t i = history.size() - look; i < history.size(); ++i) {
        hi = std::max(hi, history[i].high);
        lo = std::min(lo, history[i].low);
        range_sum += static_cast<double>(history[i].range_pts());
        tick_sum += static_cast<double>(history[i].ticks);
    }
    const double span = static_cast<double>(hi - lo);
    at(Feat::RangePosition) =
        safe_div(static_cast<double>(cur.close - lo), span, 0.5);

    const double mean_range = range_sum / static_cast<double>(look);
    at(Feat::RangeExpansion) =
        safe_div(static_cast<double>(cur.range_pts()), mean_range, 1.0);

    at(Feat::BodyFraction) =
        safe_div(std::abs(static_cast<double>(cur.close - cur.open)),
                 static_cast<double>(cur.range_pts()));

    // -- microstructure ---------------------------------------------------
    at(Feat::SpreadAtr) = safe_div(static_cast<double>(cur.spread_mean_pts), atr);
    at(Feat::SpreadMaxRatio) = safe_div(static_cast<double>(cur.spread_max_pts),
                                        static_cast<double>(cur.spread_mean_pts), 1.0);
    const double mean_ticks = tick_sum / static_cast<double>(look);
    at(Feat::TickIntensity) =
        safe_div(static_cast<double>(cur.ticks), mean_ticks, 1.0);

    // -- cross-timeframe --------------------------------------------------
    // Strided: the close kHtfStride*k bars back is the same series a higher
    // timeframe would see, without building a second bar array that could
    // drift out of alignment with this one.
    const double htf_ret = ret_pts(history, static_cast<std::size_t>(kHtfStride) * 4);
    at(Feat::HtfRetAtr) = safe_div(htf_ret, atr);
    const double ltf_ret = ret_pts(history, 1);
    at(Feat::HtfAlign) = (htf_ret > 0.0 && ltf_ret > 0.0)   ? 1.0
                         : (htf_ret < 0.0 && ltf_ret < 0.0) ? -1.0
                                                            : 0.0;

    // -- session ----------------------------------------------------------
    // The bar's CLOSE time, not its open: the row is knowable only once the
    // bar has closed, and dating it by the open would shift every feature one
    // bar into the past relative to the label that uses it.
    const TimeUs close_us = cur.close_time_us(tf);
    const int    hour = utc_hour(close_us);
    const double theta = 2.0 * std::numbers::pi * static_cast<double>(hour) / 24.0;
    at(Feat::HourSin) = std::sin(theta);
    at(Feat::HourCos) = std::cos(theta);

    const Session s = session_at(close_us);
    at(Feat::SessionId) = static_cast<double>(static_cast<std::uint8_t>(s));
    at(Feat::MinutesIntoSession) =
        static_cast<double>((close_us - day_start(close_us)) / (60 * 1'000'000LL)) ;
    at(Feat::IsRollover) = (s == Session::Rollover) ? 1.0 : 0.0;

    return r;
}

FeatureMatrix compute_features(std::span<const Bar> bars, Timeframe tf) {
    FeatureMatrix m;
    m.rows.reserve(bars.size());
    m.bar_close_us.reserve(bars.size());
    m.valid.reserve(bars.size());

    for (std::size_t i = 0; i < bars.size(); ++i) {
        // subspan(0, i+1) is the causality guarantee made structural: the row
        // for bar i literally cannot see bar i+1.
        m.rows.push_back(compute_row(bars.subspan(0, i + 1), tf));
        m.bar_close_us.push_back(bars[i].close_time_us(tf));
        m.valid.push_back(i + 1 >= kFeatureWarmup ? 1 : 0);
    }
    return m;
}

}  // namespace xau
