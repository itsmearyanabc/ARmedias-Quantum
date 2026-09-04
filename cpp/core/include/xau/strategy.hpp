// The Strategy interface.
//
// This is the object the plan's central architectural claim rests on: the
// backtester and the live engine run the SAME compiled Strategy. The only thing
// that differs between them is which Broker sits behind the fill. Backtest-vs-
// live divergence is the number one killer of deployed strategies, and it is
// prevented here structurally rather than debugged later.
#pragma once

#include "xau/bar.hpp"
#include "xau/order.hpp"
#include "xau/symbol_spec.hpp"

#include <functional>
#include <memory>
#include <span>

namespace xau {

class Strategy;

// Builds a fresh strategy. Used wherever one must not be shared across runs -
// walk-forward folds, repeated backtests from the terminal - because a reused
// instance carries armed setups and indicator state forward.
using StrategyFactory = std::function<std::unique_ptr<Strategy>()>;

// Everything a strategy is allowed to see at a decision point.
//
// `history` holds CLOSED bars only, and `history.back()` is the bar that just
// closed. There is deliberately no way to reach the forming bar from here: a
// bar stamped 14:00 on M15 covers 14:00-14:15, and acting on it before 14:15 is
// the most common silent lookahead in retail backtesting. The type simply does
// not offer the mistake.
struct BarContext {
    std::span<const Bar> history;
    const Position&      position;
    const SymbolSpec&    spec;
    Timeframe            tf = Timeframe::M15;
    double               equity = 0.0;
    double               balance = 0.0;
    TimeUs               now_us = 0;   // the instant the bar closed

    [[nodiscard]] const Bar& bar() const noexcept { return history.back(); }
    [[nodiscard]] std::size_t index() const noexcept { return history.size() - 1; }
    [[nodiscard]] bool has(std::size_t lookback) const noexcept {
        return history.size() > lookback;
    }
    // `ago(0)` is the bar that just closed, `ago(1)` the one before it.
    [[nodiscard]] const Bar& ago(std::size_t n) const noexcept {
        return history[history.size() - 1 - n];
    }
};

class Strategy {
public:
    virtual ~Strategy() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;

    // Called once before the first bar.
    virtual void on_start(const SymbolSpec&) {}

    // Called once per closed bar. The only place decisions are made.
    [[nodiscard]] virtual Decision on_bar(const BarContext&) = 0;

    // Called after a position closes, for strategies that adapt.
    virtual void on_trade_closed(const Trade&) {}

    virtual void on_finish() {}

    // How many closed bars must exist before on_bar is worth calling. The
    // engine skips the warm-up rather than making every strategy guard its own
    // indicator windows.
    [[nodiscard]] virtual std::size_t warmup_bars() const noexcept { return 0; }
};

}  // namespace xau
