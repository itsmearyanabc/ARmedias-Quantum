// Orders, fills, positions and completed trades.
#pragma once

#include "xau/types.hpp"

#include <cstdint>

namespace xau {

enum class Side : std::int8_t { None = 0, Long = 1, Short = -1 };

constexpr int sign_of(Side s) noexcept { return static_cast<int>(s); }

constexpr Side opposite(Side s) noexcept {
    return s == Side::Long ? Side::Short : (s == Side::Short ? Side::Long : Side::None);
}

constexpr const char* side_name(Side s) noexcept {
    return s == Side::Long ? "long" : (s == Side::Short ? "short" : "flat");
}

// What a Strategy asks the engine to do, expressed at bar close.
//
// Stops and targets are DISTANCES, not absolute prices, for two reasons: the
// risk layer sizes the position from the stop distance, and the stop itself is
// anchored to the price we actually filled at rather than the price we saw when
// we decided. Anchoring to the signal price would quietly change the risk taken
// whenever there was slippage.
struct Decision {
    enum class Kind : std::uint8_t { Hold, Enter, Close };

    Kind        kind = Kind::Hold;
    Side        side = Side::None;
    Points      sl_dist_pts = 0;   // 0 = no stop
    Points      tp_dist_pts = 0;   // 0 = no target
    double      lots = 0.0;        // 0 = let the risk layer size it
    const char* reason = "";       // recorded in the journal

    static Decision hold() noexcept { return Decision{}; }
    static Decision enter(Side s, Points sl, Points tp, const char* why = "") noexcept {
        Decision d;
        d.kind = Kind::Enter;
        d.side = s;
        d.sl_dist_pts = sl;
        d.tp_dist_pts = tp;
        d.reason = why;
        return d;
    }
    static Decision close(const char* why = "") noexcept {
        Decision d;
        d.kind = Kind::Close;
        d.reason = why;
        return d;
    }
};

enum class ExitReason : std::uint8_t {
    Open,
    StopLoss,
    TakeProfit,
    StrategyClose,
    EndOfData,
};

constexpr const char* exit_reason_name(ExitReason r) noexcept {
    switch (r) {
        case ExitReason::StopLoss:      return "sl";
        case ExitReason::TakeProfit:    return "tp";
        case ExitReason::StrategyClose: return "close";
        case ExitReason::EndOfData:     return "eod";
        default:                        return "open";
    }
}

// Why an order the strategy asked for never became a position. Counted rather
// than silently dropped: a strategy that is mostly rejected looks profitable
// on the trades that survive.
enum class RejectReason : std::uint8_t {
    None,
    StopTooClose,     // violates SymbolSpec::stops_level_pts
    VolumeBelowMin,   // sizer produced less than volume_min
    AlreadyInPosition,
    NoData,
};

struct Position {
    Side   side = Side::None;
    double lots = 0.0;
    Points entry_pts = 0;      // actual fill, slippage included
    TimeUs entry_ts = 0;
    Points sl_pts = 0;         // absolute; 0 = none
    Points tp_pts = 0;
    double commission_usd = 0.0;
    double swap_usd = 0.0;
    Points mfe_pts = 0;        // max favourable excursion
    Points mae_pts = 0;        // max adverse excursion
    const char* reason = "";

    [[nodiscard]] bool is_open() const noexcept { return side != Side::None; }
};

struct Trade {
    TimeUs      entry_ts = 0;
    TimeUs      exit_ts = 0;
    Side        side = Side::None;
    double      lots = 0.0;
    Points      entry_pts = 0;
    Points      exit_pts = 0;
    Points      sl_pts = 0;
    Points      tp_pts = 0;
    Points      mfe_pts = 0;
    Points      mae_pts = 0;
    double      gross_usd = 0.0;      // price move only
    double      commission_usd = 0.0;
    double      swap_usd = 0.0;
    double      net_usd = 0.0;        // gross - commission + swap
    double      balance_after = 0.0;
    ExitReason  exit_reason = ExitReason::Open;
    const char* entry_reason = "";

    [[nodiscard]] bool won() const noexcept { return net_usd > 0.0; }
    [[nodiscard]] TimeUs duration_us() const noexcept { return exit_ts - entry_ts; }
};

struct EquityPoint {
    TimeUs ts_us = 0;
    double equity = 0.0;   // balance + unrealised
    double balance = 0.0;
};

}  // namespace xau
