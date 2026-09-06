#include "xau/labels.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace xau {

namespace {

// A long is opened at the ask and closed at the bid; a short is the reverse.
// Ignoring that half-spread on both ends is a quiet way to invent about a
// point of edge per trade, which is most of the edge these baselines have.
constexpr Points entry_price(const Tick& t, Side side) noexcept {
    return side == Side::Long ? t.ask_pts() : t.bid_pts;
}
constexpr Points exit_price(const Tick& t, Side side) noexcept {
    return side == Side::Long ? t.bid_pts : t.ask_pts();
}

}  // namespace

Label label_event(const TickStore& store, TimeUs event_us, Side side, Points atr_pts,
                  const LabelConfig& cfg) {
    Label out;
    out.event_us = event_us;
    out.side = side;

    if (side == Side::None || atr_pts <= 0) return out;

    const TimeUs open_from = event_us + cfg.entry_delay_us;
    const TimeUs deadline = open_from + cfg.max_hold_us;

    // Barrier widths, floored so a dead session cannot produce a one-point
    // target that every passing tick satisfies.
    const auto width = [&](double mult) {
        const double w = static_cast<double>(atr_pts) * mult;
        const auto   p = static_cast<Points>(w);
        return std::max(p, cfg.min_barrier_pts);
    };
    const Points profit_w = width(cfg.profit_atr);
    const Points stop_w = width(cfg.stop_atr);

    bool   entered = false;
    Points profit_at = 0;
    Points stop_at = 0;

    // Walk the ticks in file-sized chunks. The first tick at or after the
    // entry delay is the fill; everything after it is barrier resolution.
    store.for_each_chunk(open_from, deadline, [&](std::span<const Tick> chunk) {
        for (const Tick& t : chunk) {
            if (!entered) {
                entered = true;
                out.entry_us = t.ts_us;
                out.entry_pts = entry_price(t, side);
                const int s = sign_of(side);
                profit_at = out.entry_pts + static_cast<Points>(s * profit_w);
                stop_at = out.entry_pts - static_cast<Points>(s * stop_w);
                continue;
            }

            const Points px = exit_price(t, side);

            // Stop first, deliberately. If one tick straddles both barriers we
            // cannot know which the market reached first, and assuming the
            // profitable one is how a backtest flatters itself.
            const bool stopped =
                side == Side::Long ? (px <= stop_at) : (px >= stop_at);
            if (stopped) {
                out.hit = Barrier::Stop;
                out.touch_us = t.ts_us;
                out.exit_pts = px;
                return false;   // stop iterating
            }

            const bool took_profit =
                side == Side::Long ? (px >= profit_at) : (px <= profit_at);
            if (took_profit) {
                out.hit = Barrier::Profit;
                out.touch_us = t.ts_us;
                out.exit_pts = px;
                return false;
            }

            // Carry the last seen price so a time-barrier exit is priced at
            // the market rather than at the entry.
            out.touch_us = t.ts_us;
            out.exit_pts = px;
        }
        return true;   // keep going
    });

    if (!entered) return out;                     // no ticks: weekend, or data ends
    if (out.hit == Barrier::None) out.hit = Barrier::Time;

    const double moved = static_cast<double>(out.exit_pts - out.entry_pts);
    out.ret_atr = sign_of(side) * moved / static_cast<double>(atr_pts);
    return out;
}

std::vector<Label> label_events(const TickStore& store, std::span<const TimeUs> event_us,
                                std::span<const Side> sides, std::span<const Points> atr_pts,
                                const LabelConfig& cfg) {
    const std::size_t n =
        std::min({event_us.size(), sides.size(), atr_pts.size()});
    std::vector<Label> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(label_event(store, event_us[i], sides[i], atr_pts[i], cfg));
    }
    compute_uniqueness(out);
    compute_weights(out);
    return out;
}

void compute_uniqueness(std::span<Label> labels) {
    if (labels.empty()) return;

    // Concurrency by sweep. Every label start is +1 and every label end is -1;
    // sorting the events gives the number of labels live at any instant
    // without an O(n^2) pairwise comparison, which matters because a decade of
    // M15 signals is hundreds of thousands of labels.
    struct Edge {
        TimeUs at;
        int    delta;
    };
    std::vector<Edge> edges;
    edges.reserve(labels.size() * 2);
    for (Label& l : labels) {
        if (l.hit == Barrier::None || l.touch_us <= l.entry_us) {
            l.uniqueness = 0.0;   // never opened: carries no information
            continue;
        }
        edges.push_back({l.entry_us, +1});
        edges.push_back({l.touch_us, -1});
    }
    // Close before open at the same instant, so a label ending exactly where
    // the next begins is not counted as overlapping it.
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (a.at != b.at) return a.at < b.at;
        return a.delta < b.delta;
    });

    // Build a running integral of 1/concurrency over the whole timeline, then
    // read each label off it as a difference. Accumulating per-label inside the
    // sweep instead would be O(labels x edges) -- fine on a toy, quadratic on
    // the ~300k labels a decade of M15 signals produces.
    std::vector<TimeUs> times;
    std::vector<double> cum;      // cum[k] = integral of 1/c dt up to times[k]
    times.reserve(edges.size());
    cum.reserve(edges.size());

    int    concurrent = 0;
    double running = 0.0;
    for (std::size_t k = 0; k < edges.size();) {
        const TimeUs at = edges[k].at;
        if (!times.empty()) {
            const TimeUs dt = at - times.back();
            if (dt > 0 && concurrent > 0) {
                running += static_cast<double>(dt) / static_cast<double>(concurrent);
            }
        }
        times.push_back(at);
        cum.push_back(running);
        // Apply every edge at this instant before opening the next interval.
        while (k < edges.size() && edges[k].at == at) {
            concurrent += edges[k].delta;
            ++k;
        }
    }

    const auto integral_at = [&](TimeUs t) {
        const auto it = std::lower_bound(times.begin(), times.end(), t);
        if (it == times.end()) return cum.empty() ? 0.0 : cum.back();
        return cum[static_cast<std::size_t>(it - times.begin())];
    };

    for (Label& l : labels) {
        if (l.hit == Barrier::None || l.touch_us <= l.entry_us) continue;
        const auto span_us = static_cast<double>(l.touch_us - l.entry_us);
        const double own = integral_at(l.touch_us) - integral_at(l.entry_us);
        l.uniqueness = span_us > 0.0 ? std::clamp(own / span_us, 0.0, 1.0) : 0.0;
    }
}

void compute_weights(std::span<Label> labels) {
    double sum = 0.0;
    std::size_t counted = 0;
    for (Label& l : labels) {
        l.weight = l.uniqueness * std::abs(l.ret_atr);
        if (l.weight > 0.0) {
            sum += l.weight;
            ++counted;
        }
    }
    // Normalise to mean 1.0 so the absolute scale of the weights does not
    // change the effective learning rate when these reach a model.
    if (counted > 0 && sum > 0.0) {
        const double scale = static_cast<double>(counted) / sum;
        for (Label& l : labels) l.weight *= scale;
    }
}

}  // namespace xau
