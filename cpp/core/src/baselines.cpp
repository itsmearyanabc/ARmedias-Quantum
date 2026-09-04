#include "xau/baselines.hpp"

namespace xau {

Decision BuyAndHold::on_bar(const BarContext& c) {
    if (signalled_ || c.position.is_open()) return Decision::hold();
    signalled_ = true;
    Decision d = Decision::enter(side_, 0, 0, "buy and hold");
    d.lots = lots_;
    return d;
}

Decision RandomEntry::on_bar(const BarContext& c) {
    if (c.position.is_open()) {
        // Hold time is derived from the fill timestamp rather than counted in
        // the strategy, so a fill delayed by latency does not shorten the hold.
        const TimeUs held = c.now_us - c.position.entry_ts;
        if (held >= static_cast<TimeUs>(cfg_.hold_bars) * timeframe_us(c.tf)) {
            return Decision::close("hold elapsed");
        }
        return Decision::hold();
    }

    if (rng_.unit() >= cfg_.entry_prob) return Decision::hold();

    const Side side = (rng_.next() & 1ULL) ? Side::Long : Side::Short;
    Decision   d = Decision::enter(side, 0, 0, "random");
    d.lots = cfg_.lots;
    return d;
}

}  // namespace xau
