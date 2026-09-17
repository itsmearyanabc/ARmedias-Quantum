#include "xau/live.hpp"

namespace xau {

LiveSession::LiveSession(Timeframe tf, Strategy& strategy, std::size_t max_history_bars)
    : bars_(tf, max_history_bars), strategy_(strategy), warmup_(strategy.warmup_bars()) {
    require_history_covers_warmup(max_history_bars, warmup_);
}

void LiveSession::start(const SymbolSpec& spec) {
    spec_ = spec;
    strategy_.on_start(spec_);
    started_ = true;
}

Decision LiveSession::on_tick(const Tick& t, const Position& pos, double equity, double balance) {
    // A session that was never started has not told the strategy which
    // instrument it is trading. Deciding anything on that basis is worse than
    // deciding nothing.
    if (!started_) return Decision::hold();

    Decision d = Decision::hold();

    // Same two-phase order as the engine: close, let the strategy see the bar,
    // THEN absorb -- which is where history is trimmed. Absorbing first would
    // shorten history under the strategy on exactly the bars where trimming
    // fires, and that difference would show up nowhere except in live P&L.
    if (bars_.close_if_boundary(t) && bars_.history().size() > warmup_) {
        const BarContext ctx{
            .history = bars_.history(),
            .position = pos,
            .spec = spec_,
            .tf = bars_.timeframe(),
            .equity = equity,
            .balance = balance,
            .now_us = bars_.last_close_us(),
        };
        d = strategy_.on_bar(ctx);
    }
    bars_.absorb(t);
    return d;
}

}  // namespace xau
