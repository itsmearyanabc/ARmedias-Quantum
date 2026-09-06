// Triple-barrier labelling at tick resolution, and the sample weights that
// make overlapping labels safe to train on.
//
// The method is Lopez de Prado's. Each event gets three barriers: a profit
// target above, a stop below, and a time limit. Whichever is touched first
// decides the label. Barriers are set in ATR units so a label means the same
// thing in 2015 gold at 1,100 USD as in 2024 gold at 2,700.
//
// Two things here are deliberate and easy to get wrong:
//
//   1. Barriers are resolved against TICKS, not bars. A bar that opens at 100,
//      trades down to 95 and closes at 105 touches a stop at 97 -- but a
//      bar-close scan sees only the 105 and labels it a win. That single
//      shortcut manufactures edge out of nothing, and it is invisible in
//      aggregate P&L.
//
//   2. When both barriers fall inside the same tick's spread, the STOP wins.
//      That matches the fill model in engine.cpp, and matches reality: you do
//      not get the good side of an ambiguity you cannot observe.

#ifndef XAU_LABELS_HPP
#define XAU_LABELS_HPP

#include "xau/bar.hpp"
#include "xau/order.hpp"
#include "xau/tick_store.hpp"
#include "xau/types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace xau {

enum class Barrier : std::uint8_t {
    None = 0,   // still open when the data ran out
    Profit,     // upper barrier for a long, lower for a short
    Stop,
    Time,       // vertical barrier: held to the time limit
};

// One labelled event.
struct Label {
    TimeUs  event_us = 0;      // when the signal fired (a bar close)
    TimeUs  entry_us = 0;      // when the position was actually opened
    TimeUs  touch_us = 0;      // when a barrier was hit
    Points  entry_pts = 0;
    Points  exit_pts = 0;
    Side    side = Side::Long;
    Barrier hit = Barrier::None;

    // Signed return in ATR units at event time. Scale-free across a decade in
    // which gold went from 1,100 to 2,700, which raw point returns are not.
    double  ret_atr = 0.0;

    // Meta-label: did taking this side actually pay, after the barriers
    // resolved? This is the target a secondary model learns to predict.
    [[nodiscard]] constexpr int meta() const noexcept { return ret_atr > 0.0 ? 1 : 0; }

    // Fraction of the label's life for which it did not overlap another label.
    // Filled in by compute_uniqueness(); 1.0 means fully unique.
    double  uniqueness = 1.0;

    // Training weight: uniqueness scaled by the magnitude of the outcome, so a
    // large clean move counts for more than a scratch that overlapped nine
    // other trades.
    double  weight = 1.0;
};

struct LabelConfig {
    // Barrier distances in ATR multiples. Asymmetry is allowed and often
    // wanted: a 2:1 target-to-stop changes what "a win" means to the model.
    double profit_atr = 2.0;
    double stop_atr = 1.0;

    // Vertical barrier. Gold's swap is punitive, so the default keeps trades
    // inside a day.
    TimeUs max_hold_us = 8 * 60 * 60 * 1'000'000LL;

    // Delay between the signal bar closing and the position opening. Never
    // zero: entering at the signal bar's own close is look-ahead, because that
    // close is only known once the bar is over.
    TimeUs entry_delay_us = 150'000;

    // Guard against a degenerate ATR (a dead session) producing barriers so
    // tight that every label is noise.
    Points min_barrier_pts = 100;   // 0.10 USD
};

// Resolve one event against the tick stream.
//
// atr_pts is the ATR at the event bar, in points, and must be computed from
// bars at or before the event -- passing a forward-looking ATR silently leaks
// the future into the barrier widths.
[[nodiscard]] Label label_event(const TickStore& store, TimeUs event_us, Side side,
                                Points atr_pts, const LabelConfig& cfg);

// Label a batch of events. Events must be sorted by time.
[[nodiscard]] std::vector<Label> label_events(const TickStore&          store,
                                              std::span<const TimeUs>   event_us,
                                              std::span<const Side>     sides,
                                              std::span<const Points>   atr_pts,
                                              const LabelConfig&        cfg);

// Average uniqueness, from the concurrency of overlapping label windows.
//
// Labels that share a holding period are not independent observations, and
// training as if they were is the single most common way a backtest that
// looks brilliant fails live. A label overlapped by three others across its
// whole life gets a quarter of the weight.
void compute_uniqueness(std::span<Label> labels);

// Fill in `weight` from uniqueness and |ret_atr|, normalised to mean 1.0.
void compute_weights(std::span<Label> labels);

}  // namespace xau

#endif  // XAU_LABELS_HPP
