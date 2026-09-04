#include "xau/bar.hpp"

#include <algorithm>

namespace xau {

BarSeries BarSeries::build(const TickStore& store, Timeframe tf, TimeUs from_us,
                           TimeUs to_us) {
    BarSeries out;
    out.tf_ = tf;
    if (from_us >= to_us) return out;

    const TimeUs step = timeframe_us(tf);

    TimeUs        cur_open = 0;
    bool          have_bar = false;
    std::uint64_t spread_sum = 0;

    auto flush = [&]() {
        if (!have_bar) return;
        Bar& b = out.bars_.back();
        b.spread_mean_pts =
            b.ticks ? static_cast<std::uint32_t>(spread_sum / b.ticks) : 0u;
        spread_sum = 0;
    };

    store.for_each_chunk(from_us, to_us, [&](std::span<const Tick> chunk) {
        for (const Tick& t : chunk) {
            const TimeUs open = bar_open_for(t.ts_us, tf);

            if (!have_bar || open != cur_open) {
                flush();
                cur_open = open;
                have_bar = true;

                Bar b;
                b.open_time_us = open;
                b.open = b.high = b.low = b.close = t.bid_pts;
                b.ticks = 0;
                b.spread_max_pts = 0;
                out.bars_.push_back(b);
            }

            Bar& b = out.bars_.back();
            if (t.bid_pts > b.high) b.high = t.bid_pts;
            if (t.bid_pts < b.low) b.low = t.bid_pts;
            b.close = t.bid_pts;
            ++b.ticks;

            const std::uint32_t sp = t.spread_pts;
            spread_sum += sp;
            if (sp > b.spread_max_pts) b.spread_max_pts = sp;
        }
        return true;
    });

    flush();

    // A bar is closed only once the window it covers has fully elapsed. The
    // last bucket is still forming if `to_us` lands inside it — which it always
    // does when `to_us` is "now".
    out.closed_count_ = out.bars_.size();
    if (!out.bars_.empty()) {
        const Bar& last = out.bars_.back();
        if (last.close_time_us(tf) > to_us) {
            out.closed_count_ = out.bars_.size() - 1;
        }
    }
    return out;
}

}  // namespace xau
