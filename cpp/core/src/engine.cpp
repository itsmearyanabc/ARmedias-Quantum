#include "xau/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <utility>

namespace xau {
namespace {

constexpr TimeUs kUsPerSec = 1'000'000;
constexpr TimeUs kUsPerDay = 86'400LL * kUsPerSec;

// ISO weekday, 1 = Monday .. 7 = Sunday. 1970-01-01 was a Thursday.
int iso_weekday(TimeUs us) noexcept {
    TimeUs days = us / kUsPerDay;
    if (us < 0 && days * kUsPerDay != us) --days;
    long long w = (days + 3) % 7;   // 0 = Monday
    if (w < 0) w += 7;
    return static_cast<int>(w) + 1;
}

// First instant at hour:00:00 UTC strictly after `us`.
TimeUs next_rollover(TimeUs us, int hour_utc) noexcept {
    TimeUs day = (us / kUsPerDay) * kUsPerDay;
    if (us < 0 && day != us) day -= kUsPerDay;
    TimeUs at = day + static_cast<TimeUs>(hour_utc) * 3600 * kUsPerSec;
    while (at <= us) at += kUsPerDay;
    return at;
}

}  // namespace

double size_position(const RiskConfig& risk, const SymbolSpec& spec, double equity,
                     Points sl_dist_pts) noexcept {
    double lots = 0.0;
    if (risk.mode == RiskConfig::Mode::FixedLots) {
        lots = risk.fixed_lots;
    } else {
        // Fractional sizing is meaningless without a stop: there is no distance
        // to divide the risk budget by. Refusing is correct; guessing a default
        // stop would silently size positions off a number nobody chose.
        if (sl_dist_pts <= 0 || equity <= 0.0) return 0.0;
        const double per_lot = static_cast<double>(sl_dist_pts) * spec.usd_per_point_per_lot();
        if (per_lot <= 0.0) return 0.0;
        lots = (equity * risk.risk_fraction) / per_lot;
    }
    lots = std::min(lots, risk.max_lots);
    return spec.round_lots(lots);
}

BacktestEngine::BacktestEngine(const TickStore& store, BacktestConfig cfg)
    : store_(store), cfg_(std::move(cfg)) {}

BacktestResult BacktestEngine::run(Strategy& strategy) {
    const auto wall_start = std::chrono::steady_clock::now();

    BacktestResult res;
    res.initial_balance = cfg_.initial_balance;
    res.strategy_name = strategy.name();

    const SymbolSpec& spec = cfg_.spec;
    const CostModel&  cm = cfg_.costs;
    const TimeUs      tf_us = timeframe_us(cfg_.tf);

    const TimeUs from = cfg_.from_us ? cfg_.from_us : store_.first_ts();
    const TimeUs to   = cfg_.to_us   ? cfg_.to_us   : store_.last_ts() + 1;

    strategy.on_start(spec);

    double           balance = cfg_.initial_balance;
    Position         pos{};
    std::vector<Bar> history;
    history.reserve(8192);

    Bar           forming{};
    bool          have_forming = false;
    TimeUs        forming_open = 0;
    std::uint64_t spread_sum = 0;
    double        recent_range_pts = 0.0;

    struct Pending {
        bool     active = false;
        TimeUs   exec_at = 0;
        Decision d{};
    };
    Pending pending{};

    TimeUs next_swap = 0;
    bool   swap_init = false;

    Tick last_tick{};
    bool have_tick = false;

    const std::size_t warmup = strategy.warmup_bars();

    // Price we could exit the open position at right now: a long sells the bid,
    // a short buys the ask.
    auto close_side_price = [&](const Tick& t) noexcept -> Points {
        return pos.side == Side::Long
                   ? t.bid_pts
                   : static_cast<Points>(t.bid_pts +
                                         cm.effective_spread(static_cast<Points>(t.spread_pts)));
    };

    auto equity_now = [&](const Tick& t) noexcept -> double {
        if (!pos.is_open()) return balance;
        const Points move =
            static_cast<Points>(sign_of(pos.side) * (close_side_price(t) - pos.entry_pts));
        return balance + spec.pnl_usd(move, pos.lots) - cm.commission_usd(pos.lots) + pos.swap_usd;
    };

    auto close_position = [&](const Tick& t, Points fill_pts, ExitReason why) {
        const Points move = static_cast<Points>(sign_of(pos.side) * (fill_pts - pos.entry_pts));
        Trade tr;
        tr.entry_ts       = pos.entry_ts;
        tr.exit_ts        = t.ts_us;
        tr.side           = pos.side;
        tr.lots           = pos.lots;
        tr.entry_pts      = pos.entry_pts;
        tr.exit_pts       = fill_pts;
        tr.sl_pts         = pos.sl_pts;
        tr.tp_pts         = pos.tp_pts;
        tr.mfe_pts        = pos.mfe_pts;
        tr.mae_pts        = pos.mae_pts;
        tr.gross_usd      = spec.pnl_usd(move, pos.lots);
        tr.commission_usd = cm.commission_usd(pos.lots);
        tr.swap_usd       = pos.swap_usd;
        tr.net_usd        = tr.gross_usd - tr.commission_usd + tr.swap_usd;
        tr.exit_reason    = why;
        tr.entry_reason   = pos.reason;
        balance += tr.net_usd;
        tr.balance_after = balance;
        res.trades.push_back(tr);
        strategy.on_trade_closed(tr);
        pos = Position{};
    };

    auto try_enter = [&](const Tick& t, const Decision& d) {
        if (pos.is_open()) {
            ++res.stats.rejected_in_position;
            return;
        }
        const Points spread = cm.effective_spread(static_cast<Points>(t.spread_pts));

        // A stop nearer than the spread is stopped out the instant it is
        // placed: a long fills on the ask and its stop triggers on the bid.
        // Real brokers accept such an order and then take you out; a backtest
        // that lets it through records a phantom loss on entry.
        if (d.sl_dist_pts > 0 && d.sl_dist_pts <= spread) {
            ++res.stats.rejected_stop_inside_spread;
            return;
        }
        if (!spec.stop_distance_ok(d.sl_dist_pts) || !spec.stop_distance_ok(d.tp_dist_pts)) {
            ++res.stats.rejected_stop_too_close;
            return;
        }

        const double lots = d.lots > 0.0
                                ? spec.round_lots(d.lots)
                                : size_position(cfg_.risk, spec, equity_now(t), d.sl_dist_pts);
        if (lots <= 0.0) {
            ++res.stats.rejected_volume;
            return;
        }

        const bool   buying = (d.side == Side::Long);
        const Points px = fill_price(t, buying, cm, recent_range_pts);
        const int    sgn = sign_of(d.side);

        pos = Position{};
        pos.side      = d.side;
        pos.lots      = lots;
        pos.entry_pts = px;
        pos.entry_ts  = t.ts_us;
        pos.reason    = d.reason;
        pos.sl_pts = d.sl_dist_pts ? static_cast<Points>(px - sgn * d.sl_dist_pts) : 0;
        pos.tp_pts = d.tp_dist_pts ? static_cast<Points>(px + sgn * d.tp_dist_pts) : 0;
    };

    auto check_exits = [&](const Tick& t) -> bool {
        const Points px = close_side_price(t);
        const Points excursion = static_cast<Points>(sign_of(pos.side) * (px - pos.entry_pts));
        if (excursion > pos.mfe_pts) pos.mfe_pts = excursion;
        if (excursion < pos.mae_pts) pos.mae_pts = excursion;

        // Stop is tested first. When a single tick jumps past both levels we
        // take the adverse one: a backtest that resolves its own ambiguity
        // favourably is how a losing system reads as profitable.
        if (pos.sl_pts != 0) {
            const bool hit =
                (pos.side == Side::Long) ? (px <= pos.sl_pts) : (px >= pos.sl_pts);
            if (hit) {
                // Gapped through the stop: fill where the market actually is,
                // never at the stop price the market never traded at.
                const Points base = (pos.side == Side::Long) ? std::min(px, pos.sl_pts)
                                                             : std::max(px, pos.sl_pts);
                const bool buying = (pos.side == Side::Short);
                close_position(t,
                               CostModel::apply_slippage(base, buying,
                                                         cm.slippage_pts(recent_range_pts)),
                               ExitReason::StopLoss);
                return true;
            }
        }
        if (pos.tp_pts != 0) {
            const bool hit =
                (pos.side == Side::Long) ? (px >= pos.tp_pts) : (px <= pos.tp_pts);
            if (hit) {
                // A resting limit fills at its price or better and never slips
                // against you. The asymmetry with the stop above is deliberate
                // and is most of what separates an honest fill model from a
                // flattering one.
                close_position(t, pos.tp_pts, ExitReason::TakeProfit);
                return true;
            }
        }
        return false;
    };

    // ------------------------------------------------------------------
    // main loop
    // ------------------------------------------------------------------
    store_.for_each_chunk(from, to, [&](std::span<const Tick> chunk) {
        for (const Tick& t : chunk) {
            ++res.stats.ticks;
            last_tick = t;
            have_tick = true;

            if (!swap_init) {
                next_swap = next_rollover(t.ts_us, cfg_.swap_hour_utc);
                swap_init = true;
            }

            // 1) rollover swap
            while (cfg_.apply_swap && t.ts_us >= next_swap) {
                if (pos.is_open()) {
                    const double mult =
                        (iso_weekday(next_swap) == spec.triple_swap_weekday) ? 3.0 : 1.0;
                    const double pts =
                        (pos.side == Side::Long) ? spec.swap_long_pts : spec.swap_short_pts;
                    pos.swap_usd += pts * spec.usd_per_point_per_lot() * pos.lots * mult;
                    ++res.stats.swap_charges;
                }
                next_swap += kUsPerDay;
            }

            // 2) stops and targets, before anything new is opened, so a
            //    position cannot be stopped out by the tick that filled it
            if (pos.is_open() && check_exits(t)) {
                if (pending.active && pending.d.kind == Decision::Kind::Close) {
                    pending.active = false;   // the stop got there first
                }
            }

            // 3) bar boundary
            const TimeUs open = bar_open_for(t.ts_us, cfg_.tf);
            if (have_forming && open != forming_open) {
                forming.spread_mean_pts =
                    forming.ticks ? static_cast<std::uint32_t>(spread_sum / forming.ticks) : 0u;
                history.push_back(forming);
                ++res.stats.bars;
                recent_range_pts = static_cast<double>(forming.range_pts());

                const TimeUs close_time = forming_open + tf_us;
                res.equity.push_back(EquityPoint{close_time, equity_now(t), balance});

                if (history.size() > warmup) {
                    const BarContext ctx{
                        .history  = history,
                        .position = pos,
                        .spec     = spec,
                        .tf       = cfg_.tf,
                        .equity   = equity_now(t),
                        .balance  = balance,
                        .now_us   = close_time,
                    };
                    const Decision d = strategy.on_bar(ctx);
                    if (d.kind != Decision::Kind::Hold) {
                        ++res.stats.signals;
                        pending.active  = true;
                        pending.exec_at = close_time + cm.latency_us;
                        pending.d       = d;
                    }
                }

                // Trim with hysteresis: erasing from the front of a vector is
                // linear, so do it rarely rather than every bar.
                if (cfg_.max_history_bars && history.size() > cfg_.max_history_bars * 2) {
                    const auto drop =
                        static_cast<std::ptrdiff_t>(history.size() - cfg_.max_history_bars);
                    history.erase(history.begin(), history.begin() + drop);
                }
                have_forming = false;
            }

            if (!have_forming) {
                forming = Bar{};
                forming.open_time_us = open;
                forming.open = forming.high = forming.low = forming.close = t.bid_pts;
                forming_open = open;
                have_forming = true;
                spread_sum = 0;
            }

            if (t.bid_pts > forming.high) forming.high = t.bid_pts;
            if (t.bid_pts < forming.low) forming.low = t.bid_pts;
            forming.close = t.bid_pts;
            ++forming.ticks;
            spread_sum += t.spread_pts;
            if (t.spread_pts > forming.spread_max_pts) forming.spread_max_pts = t.spread_pts;

            // 4) latency-delayed execution. The order was queued at a bar close
            //    and fills on the first tick at or after close + latency, so the
            //    price genuinely moves underneath it.
            if (pending.active && t.ts_us >= pending.exec_at) {
                if (pending.d.kind == Decision::Kind::Enter) {
                    try_enter(t, pending.d);
                } else if (pending.d.kind == Decision::Kind::Close && pos.is_open()) {
                    const bool buying = (pos.side == Side::Short);
                    close_position(t,
                                   CostModel::apply_slippage(close_side_price(t), buying,
                                                             cm.slippage_pts(recent_range_pts)),
                                   ExitReason::StrategyClose);
                }
                pending.active = false;
            }
        }
        return true;
    });

    if (pos.is_open() && cfg_.close_at_end && have_tick) {
        const bool   buying = (pos.side == Side::Short);
        const Points px = CostModel::apply_slippage(close_side_price(last_tick), buying,
                                                    cm.slippage_pts(recent_range_pts));
        close_position(last_tick, px, ExitReason::EndOfData);
    }
    if (have_tick) {
        res.equity.push_back(EquityPoint{last_tick.ts_us, equity_now(last_tick), balance});
    }

    strategy.on_finish();

    res.final_balance = balance;
    res.metrics = Metrics::compute(res.trades, res.equity, cfg_.initial_balance, cfg_.tf);
    res.stats.wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    return res;
}

// ---------------------------------------------------------------------------
// metrics
// ---------------------------------------------------------------------------

Metrics Metrics::compute(const std::vector<Trade>& trades,
                         const std::vector<EquityPoint>& equity, double initial_balance,
                         Timeframe tf) {
    Metrics m;
    m.trades = trades.size();

    for (const Trade& t : trades) {
        if (t.net_usd > 0.0) {
            ++m.wins;
            m.gross_profit += t.net_usd;
        } else {
            ++m.losses;
            m.gross_loss += -t.net_usd;
        }
        m.total_commission += t.commission_usd;
        m.total_swap += t.swap_usd;
        m.net_profit += t.net_usd;
    }

    const auto dtrades = static_cast<double>(m.trades);
    if (m.trades) {
        m.win_rate = static_cast<double>(m.wins) / dtrades;
        m.expectancy_usd = m.net_profit / dtrades;
    }
    if (m.wins) m.avg_win = m.gross_profit / static_cast<double>(m.wins);
    if (m.losses) m.avg_loss = m.gross_loss / static_cast<double>(m.losses);
    m.profit_factor = m.gross_loss > 0.0 ? m.gross_profit / m.gross_loss : 0.0;
    if (initial_balance > 0.0) m.return_pct = 100.0 * m.net_profit / initial_balance;

    double peak = initial_balance;
    for (const EquityPoint& e : equity) {
        peak = std::max(peak, e.equity);
        const double dd = peak - e.equity;
        if (dd > m.max_drawdown_usd) {
            m.max_drawdown_usd = dd;
            m.max_drawdown_pct = peak > 0.0 ? 100.0 * dd / peak : 0.0;
        }
    }

    if (equity.size() > 3) {
        std::vector<double> rets;
        rets.reserve(equity.size() - 1);
        for (std::size_t i = 1; i < equity.size(); ++i) {
            const double prev = equity[i - 1].equity;
            if (prev > 0.0) rets.push_back(equity[i].equity / prev - 1.0);
        }
        if (rets.size() > 2) {
            const double n = static_cast<double>(rets.size());
            const double mean = std::accumulate(rets.begin(), rets.end(), 0.0) / n;
            double var = 0.0;
            for (double r : rets) var += (r - mean) * (r - mean);
            var /= (n - 1.0);
            const double sd = std::sqrt(var);
            if (sd > 0.0) {
                // Bars per year for a 24x5 market. Approximate by construction:
                // it ignores holidays and the fact that empty buckets produce
                // no bar, so treat the figure as indicative, not decisive.
                const double per_year = (365.25 * 24.0 * 3600.0 * 1e6) /
                                        static_cast<double>(timeframe_us(tf)) * (5.0 / 7.0);
                m.sharpe = mean / sd * std::sqrt(per_year);
            }
        }
    }
    return m;
}

std::string Metrics::to_string() const {
    char buf[1400];
    std::snprintf(
        buf, sizeof(buf),
        "trades       %zu  (%zu won, %zu lost, %.1f%% win rate)\n"
        "net profit   %.2f USD   (%.2f%% on initial)\n"
        "gross        +%.2f / -%.2f   profit factor %.3f\n"
        "expectancy   %.4f USD per trade\n"
        "avg win/loss %.2f / %.2f\n"
        "costs        commission %.2f, swap %.2f\n"
        "max drawdown %.2f USD  (%.2f%%)\n"
        "sharpe       %.2f  (annualised, indicative)",
        trades, wins, losses, win_rate * 100.0, net_profit, return_pct, gross_profit, gross_loss,
        profit_factor, expectancy_usd, avg_win, avg_loss, total_commission, total_swap,
        max_drawdown_usd, max_drawdown_pct, sharpe);
    return buf;
}

std::string BacktestResult::summary() const {
    char head[512];
    std::snprintf(head, sizeof(head),
                  "%s\n%s\nbalance      %.2f -> %.2f\n"
                  "processed    %llu ticks, %llu bars, %llu signals in %.2f s\n"
                  "rejected     %llu  (stop too close %llu, inside spread %llu, "
                  "volume %llu, in position %llu)\n",
                  strategy_name.c_str(), std::string(60, '-').c_str(), initial_balance,
                  final_balance, static_cast<unsigned long long>(stats.ticks),
                  static_cast<unsigned long long>(stats.bars),
                  static_cast<unsigned long long>(stats.signals), stats.wall_seconds,
                  static_cast<unsigned long long>(stats.rejected_total()),
                  static_cast<unsigned long long>(stats.rejected_stop_too_close),
                  static_cast<unsigned long long>(stats.rejected_stop_inside_spread),
                  static_cast<unsigned long long>(stats.rejected_volume),
                  static_cast<unsigned long long>(stats.rejected_in_position));
    return std::string(head) + metrics.to_string();
}

}  // namespace xau
