// Tick-to-bar assembly, shared by the backtest engine and the live session.
//
// This exists as its own class for one reason: the promise that the thing
// trading live is the same thing that was backtested. Before it, the engine
// built bars inline in run() and the live path had no bar builder at all.
// Two implementations would have been the obvious next step, and two
// implementations drift -- quietly, on exactly the edge cases nobody tests.
// One implementation cannot disagree with itself.
//
// The rules below are the engine's rules, extracted unchanged, and a golden
// comparison of run_baselines output across the gold decade confirms the
// extraction altered nothing. Two of them are easy to get wrong:
//
//   A bar closes when the first tick of the NEXT bucket arrives, not when the
//   clock crosses the boundary. A live path closing bars on a timer would hand
//   the strategy a bar before the market had finished making it.
//
//   The close time reported is the NOMINAL boundary (open + timeframe), not the
//   arrival time of the tick that triggered it. Across a weekend Friday's last
//   bar does not close until Sunday's first tick, yet it is still reported as
//   closing on Friday -- and a decision made on it executes into Sunday's gap.
//
// Usage mirrors the engine's per-tick order exactly:
//
//   if (bars.close_if_boundary(t)) { ... run the strategy on bars.history() ... }
//   bars.absorb(t);
//
// The split matters: history is trimmed in absorb(), AFTER the strategy has
// seen the bar. Trimming inside close_if_boundary would change history.size()
// under the strategy on precisely the bars where trimming fires.

#ifndef XAU_BAR_ASSEMBLER_HPP
#define XAU_BAR_ASSEMBLER_HPP

#include "xau/bar.hpp"
#include "xau/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xau {

// Throws std::invalid_argument when a history cap would silence the strategy.
//
// The strategy is only called once history holds MORE than warmup bars, and a
// trim cuts history back to max_history_bars. If that is not above warmup,
// every trim drops history under the gate and the strategy stops being called
// until it regrows -- periodically, silently, and with no error anywhere. In a
// backtest that quietly thins the trade count; live, it is a bot that has
// stopped trading and looks healthy. Zero means "never trim" and is always
// accepted.
void require_history_covers_warmup(std::size_t max_history_bars, std::size_t warmup);

class BarAssembler {
public:
    // max_history_bars == 0 keeps every bar. Otherwise history is trimmed back
    // to max_history_bars once it exceeds twice that -- hysteresis, because
    // erasing from the front of a vector is linear and doing it every bar
    // would make a decade-long run quadratic.
    explicit BarAssembler(Timeframe tf, std::size_t max_history_bars = 0);

    // Phase 1. If `t` falls in a later bucket than the bar being formed, that
    // bar is completed and appended to history, and this returns true.
    [[nodiscard]] bool close_if_boundary(const Tick& t);

    // Phase 2. Trims history if a bar has just closed, then folds `t` into the
    // forming bar, starting a new one if needed.
    void absorb(const Tick& t);

    [[nodiscard]] std::span<const Bar> history() const noexcept { return history_; }

    // Nominal close time of the most recently closed bar. Only meaningful
    // after close_if_boundary has returned true at least once.
    [[nodiscard]] TimeUs last_close_us() const noexcept { return last_close_us_; }

    // High minus low of the most recently closed bar, which the fill model
    // uses to scale slippage.
    [[nodiscard]] double recent_range_pts() const noexcept { return recent_range_pts_; }

    // Bars closed over the whole run, unaffected by trimming.
    [[nodiscard]] std::size_t bars_closed() const noexcept { return bars_closed_; }

    [[nodiscard]] Timeframe timeframe() const noexcept { return tf_; }

private:
    Timeframe        tf_;
    TimeUs           tf_us_;
    std::size_t      max_history_bars_;

    std::vector<Bar> history_;
    Bar              forming_{};
    bool             have_forming_ = false;
    TimeUs           forming_open_ = 0;
    std::uint64_t    spread_sum_ = 0;

    bool             just_closed_ = false;
    TimeUs           last_close_us_ = 0;
    double           recent_range_pts_ = 0.0;
    std::size_t      bars_closed_ = 0;
};

}  // namespace xau

#endif  // XAU_BAR_ASSEMBLER_HPP
