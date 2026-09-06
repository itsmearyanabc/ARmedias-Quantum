// Contract terms for the instrument being traded.
//
// Every number the cost model and the risk layer consume traces back to here,
// and here traces back to config/symbol_spec.json, dumped once from the broker
// by python/xau_ingest/mt5spec.py. It is a committed snapshot, never read live
// during a backtest — a backtest whose parameters can change underneath it is
// not reproducible.
#pragma once

#include "xau/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace xau {

struct SymbolSpec {
    std::string  name = "XAUUSD";
    std::int32_t point_num = XAUUSD_POINT_NUM;
    std::int32_t point_den = XAUUSD_POINT_DEN;

    // Units of the base asset per lot. Gold is 100 troy ounces.
    double contract_size = 100.0;

    double volume_min = 0.01;
    double volume_max = 100.0;
    double volume_step = 0.01;

    // Broker minimum stop distance. An order whose stop sits closer than this
    // is *rejected*, not adjusted. Modelling it is what keeps the backtest from
    // taking trades that could never have been placed.
    Points stops_level_pts = 0;
    Points freeze_level_pts = 0;

    // Swap in points per night, signed (negative = you pay).
    double swap_long_pts = 0.0;
    double swap_short_pts = 0.0;
    int    triple_swap_weekday = 3;  // Wednesday: Mon=1 .. Sun=7 (ISO)

    // USD moved per point, per lot.
    //
    // XAUUSD: 100 oz x 0.001 USD/point = 0.10 USD per point per lot. A 1.00 USD
    // move on 1.00 lot is therefore 100 USD, which is the familiar figure.
    [[nodiscard]] double usd_per_point_per_lot() const noexcept {
        return contract_size * static_cast<double>(point_num) /
               static_cast<double>(point_den);
    }

    [[nodiscard]] double price_usd(Points p) const noexcept {
        return static_cast<double>(p) * static_cast<double>(point_num) /
               static_cast<double>(point_den);
    }

    [[nodiscard]] Points points_from_usd(double usd) const noexcept {
        return static_cast<Points>(std::llround(usd * static_cast<double>(point_den) /
                                                static_cast<double>(point_num)));
    }

    // Signed P&L for a price move of `move_pts` in the position's favour.
    [[nodiscard]] double pnl_usd(Points move_pts, double lots) const noexcept {
        return static_cast<double>(move_pts) * usd_per_point_per_lot() * lots;
    }

    // Snap to the broker's volume ladder. Rounds DOWN to the step so we never
    // silently take more risk than the sizer asked for, then clamps.
    [[nodiscard]] double round_lots(double lots) const noexcept {
        if (volume_step <= 0.0) return std::clamp(lots, volume_min, volume_max);
        const double steps = std::floor(lots / volume_step + 1e-9);
        double snapped = steps * volume_step;
        // Volume steps are decimal (0.01); binary floating point is not, so a
        // bare product accumulates error that shows up as 0.0299999 lots.
        snapped = std::round(snapped * 1e8) / 1e8;
        if (snapped < volume_min) return 0.0;   // below the minimum = no trade
        return std::min(snapped, volume_max);
    }

    [[nodiscard]] bool stop_distance_ok(Points dist_pts) const noexcept {
        return dist_pts == 0 || dist_pts >= stops_level_pts;
    }

    static SymbolSpec xauusd_default() { return SymbolSpec{}; }

    // Silver, for testing whether a hypothesis generalises beyond the one
    // instrument it was found on.
    //
    // Silver is NOT independent evidence -- it correlates with gold around 0.8,
    // so its bars do not add sample the way a bar count suggests. What it does
    // give is a test the gold data cannot: a rule discovered on gold either
    // describes a mechanism, in which case it should survive on a related
    // metal, or it describes gold's particular decade, in which case it will
    // not. That distinction is worth more here than more of the same series.
    //
    // Contract is 5,000 oz on MT5 and the quote runs to three decimals rather
    // than two, so a "point" is a different fraction of price than in gold --
    // getting either wrong silently rescales every P&L figure.
    static SymbolSpec xagusd_default() {
        SymbolSpec s;
        s.name = "XAGUSD";
        s.point_num = 1;
        s.point_den = 1000;      // 1 pt = 0.001 USD, as for gold
        s.contract_size = 5000.0;
        s.volume_min = 0.01;
        s.volume_max = 100.0;
        s.volume_step = 0.01;
        return s;
    }

    // Look a spec up by the symbol name a store carries, so tools do not have
    // to hardcode which metal they are being pointed at.
    static SymbolSpec for_symbol(const std::string& sym) {
        if (sym == "XAGUSD") return xagusd_default();
        return xauusd_default();
    }
};

}  // namespace xau
