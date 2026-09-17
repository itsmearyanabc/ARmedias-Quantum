// The live decision path.
//
// Feeds ticks one at a time, as a broker delivers them, through the SAME bar
// assembler and the SAME strategy contract the backtest engine uses, and
// returns what the strategy decided. It does not fill anything: live, fills
// come from the broker, and pretending otherwise would reintroduce a second
// fill model to drift out of step with the first.
//
// What it guarantees is narrower and more important: given the same ticks and
// the same position, the strategy sees the same bars, at the same nominal
// times, with the same history length, as it did in backtest. A parity test
// pins that across trimming, warmup and weekend gaps. Without it, "the thing
// that trades live is the thing that was backtested" is a sentence rather than
// a property.

#ifndef XAU_LIVE_HPP
#define XAU_LIVE_HPP

#include "xau/bar_assembler.hpp"
#include "xau/order.hpp"
#include "xau/strategy.hpp"
#include "xau/symbol_spec.hpp"

#include <cstddef>
#include <span>

namespace xau {

class LiveSession {
public:
    // max_history_bars must match the backtest configuration the strategy was
    // validated under. A different value changes history.size() and, for any
    // strategy that reads it, what the strategy computes.
    LiveSession(Timeframe tf, Strategy& strategy, std::size_t max_history_bars = 0);

    // Call once before the first tick, as the engine calls on_start.
    void start(const SymbolSpec& spec);

    // One tick. Returns the strategy's decision if this tick closed a bar past
    // warmup, and Hold otherwise. Position, equity and balance are the
    // BROKER's view -- the session never infers them.
    [[nodiscard]] Decision on_tick(const Tick& t, const Position& pos, double equity,
                                   double balance);

    [[nodiscard]] std::span<const Bar> history() const noexcept { return bars_.history(); }
    [[nodiscard]] std::size_t          bars_closed() const noexcept { return bars_.bars_closed(); }
    [[nodiscard]] TimeUs               last_close_us() const noexcept { return bars_.last_close_us(); }
    [[nodiscard]] bool                 started() const noexcept { return started_; }

private:
    BarAssembler bars_;
    Strategy&    strategy_;
    SymbolSpec   spec_{};
    std::size_t  warmup_;
    bool         started_ = false;
};

}  // namespace xau

#endif  // XAU_LIVE_HPP
