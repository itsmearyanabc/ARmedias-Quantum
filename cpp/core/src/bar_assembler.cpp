#include "xau/bar_assembler.hpp"

#include <stdexcept>
#include <string>

namespace xau {

void require_history_covers_warmup(std::size_t max_history_bars, std::size_t warmup) {
    if (max_history_bars != 0 && max_history_bars <= warmup) {
        throw std::invalid_argument(
            "max_history_bars (" + std::to_string(max_history_bars) +
            ") must exceed the strategy warmup (" + std::to_string(warmup) +
            "), or every history trim silences the strategy until it regrows");
    }
}

BarAssembler::BarAssembler(Timeframe tf, std::size_t max_history_bars)
    : tf_(tf), tf_us_(timeframe_us(tf)), max_history_bars_(max_history_bars) {
    history_.reserve(8192);
}

bool BarAssembler::close_if_boundary(const Tick& t) {
    just_closed_ = false;
    const TimeUs open = bar_open_for(t.ts_us, tf_);
    if (!have_forming_ || open == forming_open_) return false;

    // Integer division of the running sum, exactly as the engine did it. A
    // floating-point mean would round differently on some bars, and "almost
    // the same spread" is not the same input to a strategy that thresholds it.
    forming_.spread_mean_pts =
        forming_.ticks ? static_cast<std::uint32_t>(spread_sum_ / forming_.ticks) : 0u;
    history_.push_back(forming_);
    ++bars_closed_;
    recent_range_pts_ = static_cast<double>(forming_.range_pts());
    last_close_us_ = forming_open_ + tf_us_;
    just_closed_ = true;
    return true;
}

void BarAssembler::absorb(const Tick& t) {
    if (just_closed_) {
        if (max_history_bars_ != 0 && history_.size() > max_history_bars_ * 2) {
            const auto drop = static_cast<std::ptrdiff_t>(history_.size() - max_history_bars_);
            history_.erase(history_.begin(), history_.begin() + drop);
        }
        have_forming_ = false;
        just_closed_ = false;
    }

    if (!have_forming_) {
        const TimeUs open = bar_open_for(t.ts_us, tf_);
        forming_ = Bar{};
        forming_.open_time_us = open;
        forming_.open = forming_.high = forming_.low = forming_.close = t.bid_pts;
        forming_open_ = open;
        have_forming_ = true;
        spread_sum_ = 0;
    }

    if (t.bid_pts > forming_.high) forming_.high = t.bid_pts;
    if (t.bid_pts < forming_.low) forming_.low = t.bid_pts;
    forming_.close = t.bid_pts;
    ++forming_.ticks;
    spread_sum_ += t.spread_pts;
    if (t.spread_pts > forming_.spread_max_pts) forming_.spread_max_pts = t.spread_pts;
}

}  // namespace xau
