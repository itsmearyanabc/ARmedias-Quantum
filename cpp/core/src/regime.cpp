#include "xau/regime.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace xau {

namespace {

double true_range(const Bar& cur, const Bar& prev) noexcept {
    const double hi = static_cast<double>(cur.high);
    const double lo = static_cast<double>(cur.low);
    const double pc = static_cast<double>(prev.close);
    return std::max({hi - lo, std::abs(hi - pc), std::abs(lo - pc)});
}

double mean_close(std::span<const Bar> h, std::size_t n) noexcept {
    const std::size_t k = std::min(n, h.size());
    if (k == 0) return 0.0;
    double s = 0.0;
    for (std::size_t i = h.size() - k; i < h.size(); ++i) s += static_cast<double>(h[i].close);
    return s / static_cast<double>(k);
}

double atr_at(std::span<const Bar> h, std::size_t end, std::size_t period) noexcept {
    if (end == 0 || end >= h.size()) return 0.0;
    const std::size_t n = std::min(period, end);
    double            s = 0.0;
    for (std::size_t i = end - n + 1; i <= end; ++i) s += true_range(h[i], h[i - 1]);
    return n > 0 ? s / static_cast<double>(n) : 0.0;
}

}  // namespace

std::string_view regime_name(Regime r) noexcept { return regime_name_at(r.index()); }

std::string_view regime_name_at(std::size_t index) noexcept {
    switch (index) {
        case 0: return "quiet/range";
        case 1: return "quiet/trend";
        case 2: return "normal/range";
        case 3: return "normal/trend";
        case 4: return "wild/range";
        case 5: return "wild/trend";
        default: break;
    }
    return "?";
}

Regime classify(std::span<const Bar> history, const RegimeConfig& cfg) noexcept {
    Regime out;
    if (history.size() < 3) return out;

    const std::size_t last = history.size() - 1;
    const double      atr = atr_at(history, last, cfg.atr_period);
    if (!(atr > 0.0)) return out;

    // --- volatility, relative to its own recent distribution ----------------
    // The MEDIAN, not the mean: a single news bar can be twenty times a normal
    // range, and a mean lets that one bar redefine "normal" for the next
    // hundred. The median does not care.
    const std::size_t window = std::min(cfg.vol_lookback, last);
    if (window >= 8) {
        std::vector<double> hist;
        hist.reserve(window);
        for (std::size_t i = last - window + 1; i <= last; ++i) {
            const double a = atr_at(history, i, cfg.atr_period);
            if (a > 0.0) hist.push_back(a);
        }
        if (!hist.empty()) {
            const std::size_t mid = hist.size() / 2;
            std::nth_element(hist.begin(), hist.begin() + static_cast<long>(mid), hist.end());
            const double median = hist[mid];
            if (median > 0.0) {
                const double ratio = atr / median;
                if (ratio < cfg.quiet_below) out.vol = Volatility::Quiet;
                else if (ratio > cfg.wild_above) out.vol = Volatility::Wild;
                else out.vol = Volatility::Normal;
            }
        }
    }

    // --- structure ----------------------------------------------------------
    if (history.size() > cfg.slow_ma) {
        const double fast = mean_close(history, cfg.fast_ma);
        const double slow = mean_close(history, cfg.slow_ma);
        const double separation = std::abs(fast - slow) / atr;
        out.structure = separation >= cfg.trend_atr ? Structure::Trending : Structure::Ranging;
    }
    return out;
}

}  // namespace xau
