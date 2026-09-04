// Execution cost model.
//
// UNITS. Every point quantity here is a STORE point: 1 pt = 0.001 USD, matching
// xau::Points. A broker that quotes gold to two decimals calls 0.01 USD "one
// point" — that is TEN of ours. Getting this confused silently scales every
// cost in the system, so the defaults below are written with their USD value
// spelled out.
//
// The single most common source of fake edge in a retail gold backtest is a
// constant spread. Gold's spread widens 5-20x around NFP, CPI and FOMC, and
// again at the 21:00-22:00 UTC rollover. This model never invents a spread: it
// takes the one recorded on the tick and only ever adds to it.
#pragma once

#include "xau/order.hpp"
#include "xau/types.hpp"

#include <algorithm>
#include <cstdint>

namespace xau {

struct CostModel {
    // Slippage floor, applied to every market fill. 15 pts = 0.015 USD.
    double slip_base_pts = 15.0;

    // Extra slippage proportional to recent volatility, as a fraction of the
    // recent bar range. News fills are worse because the market is moving, not
    // because the broker is punishing you.
    double slip_vol_coef = 0.05;

    // Cap so a single wild bar cannot produce an absurd fill.
    double slip_max_pts = 800.0;   // 0.80 USD

    // Signal-to-fill delay. The engine replays the tick stream forward by this
    // much before filling, so the price genuinely moves underneath the order.
    TimeUs latency_us = 150'000;   // 150 ms, typical retail round trip

    // USD per lot for a full round turn.
    double commission_per_lot_round_usd = 0.0;

    // Stress multipliers. Section 8 of the plan requires a strategy to stay
    // profitable at 2x spread and 2x slippage; these are how that test is run,
    // rather than by maintaining a separate "pessimistic" cost model that can
    // drift out of sync with this one.
    double spread_mult = 1.0;
    double slippage_mult = 1.0;

    [[nodiscard]] Points effective_spread(Points raw_spread_pts) const noexcept {
        const double s = static_cast<double>(raw_spread_pts) * spread_mult;
        return static_cast<Points>(s < 0.0 ? 0.0 : s);
    }

    // recent_range_pts: a recent bar's high-low, or an ATR. Zero is fine.
    [[nodiscard]] Points slippage_pts(double recent_range_pts) const noexcept {
        double s = slip_base_pts + slip_vol_coef * std::max(0.0, recent_range_pts);
        s *= slippage_mult;
        s = std::clamp(s, 0.0, slip_max_pts);
        return static_cast<Points>(s);
    }

    // Slippage always works against the trader. `buying` is true when the
    // order lifts the offer (opening a long, or closing a short).
    [[nodiscard]] static Points apply_slippage(Points price_pts, bool buying,
                                               Points slip_pts) noexcept {
        return buying ? price_pts + slip_pts : price_pts - slip_pts;
    }

    [[nodiscard]] double commission_usd(double lots) const noexcept {
        return commission_per_lot_round_usd * lots;
    }

    // A cost profile with the knobs doubled, for the stress test.
    [[nodiscard]] CostModel stressed(double factor = 2.0) const noexcept {
        CostModel c = *this;
        c.spread_mult *= factor;
        c.slippage_mult *= factor;
        return c;
    }
};

// The price a market order actually fills at, given the tick it lands on.
//
// Buying pays the ask, selling hits the bid; the spread is therefore paid on
// entry AND exit, which is why a round turn costs roughly one full spread plus
// two slippages before the strategy has been right about anything.
[[nodiscard]] inline Points fill_price(const Tick& t, bool buying, const CostModel& cm,
                                       double recent_range_pts) noexcept {
    const Points spread = cm.effective_spread(static_cast<Points>(t.spread_pts));
    const Points quoted = buying ? t.bid_pts + spread : t.bid_pts;
    return CostModel::apply_slippage(quoted, buying, cm.slippage_pts(recent_range_pts));
}

}  // namespace xau
