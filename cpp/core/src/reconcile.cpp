#include "xau/reconcile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace xau {

const char* discrepancy_name(Discrepancy d) noexcept {
    switch (d) {
        case Discrepancy::MissingAtBroker: return "missing_at_broker";
        case Discrepancy::UnknownAtBroker: return "unknown_at_broker";
        case Discrepancy::SideMismatch: return "side_mismatch";
        case Discrepancy::SizeMismatch: return "size_mismatch";
        case Discrepancy::PriceMismatch: return "price_mismatch";
    }
    return "?";
}

bool ReconcileResult::must_halt() const noexcept {
    return std::any_of(issues.begin(), issues.end(), [](const ReconcileIssue& i) {
        return i.kind != Discrepancy::PriceMismatch;
    });
}

ReconcileResult reconcile(std::span<const BookEntry> ours, std::span<const BookEntry> broker,
                          const ReconcileTolerance& tol) {
    ReconcileResult out;

    std::unordered_map<std::uint64_t, const BookEntry*> theirs;
    theirs.reserve(broker.size());
    for (const BookEntry& b : broker) theirs.emplace(b.ticket, &b);

    // Matched tickets are erased as we go, so whatever survives is a position
    // the broker holds that we have no record of.
    for (const BookEntry& o : ours) {
        const auto it = theirs.find(o.ticket);
        if (it == theirs.end()) {
            out.issues.push_back({Discrepancy::MissingAtBroker, o.ticket, o.lots, 0.0});
            continue;
        }
        const BookEntry& b = *it->second;

        // Side before size: a reversed position is the worse error, and
        // reporting it as a size mismatch would understate it.
        if (o.side != b.side) {
            out.issues.push_back({Discrepancy::SideMismatch, o.ticket,
                                  static_cast<double>(sign_of(o.side)),
                                  static_cast<double>(sign_of(b.side))});
        } else if (std::abs(o.lots - b.lots) > tol.lots_epsilon) {
            out.issues.push_back({Discrepancy::SizeMismatch, o.ticket, o.lots, b.lots});
        }

        if (std::abs(static_cast<long long>(o.open_pts) - static_cast<long long>(b.open_pts)) >
            static_cast<long long>(tol.price_tolerance_pts)) {
            out.issues.push_back({Discrepancy::PriceMismatch, o.ticket,
                                  static_cast<double>(o.open_pts),
                                  static_cast<double>(b.open_pts)});
        }
        theirs.erase(it);
    }

    // Unordered-map iteration order is unspecified; sort so the report is
    // deterministic and a diff between two runs means something.
    std::vector<const BookEntry*> leftover;
    leftover.reserve(theirs.size());
    for (const auto& [ticket, entry] : theirs) leftover.push_back(entry);
    std::sort(leftover.begin(), leftover.end(),
              [](const BookEntry* a, const BookEntry* b) { return a->ticket < b->ticket; });
    for (const BookEntry* b : leftover) {
        out.issues.push_back({Discrepancy::UnknownAtBroker, b->ticket, 0.0, b->lots});
    }
    return out;
}

const char* drift_verdict_name(DriftVerdict v) noexcept {
    switch (v) {
        case DriftVerdict::Insufficient: return "insufficient";
        case DriftVerdict::Consistent: return "consistent";
        case DriftVerdict::Drifting: return "drifting";
    }
    return "?";
}

namespace {

double mean_of(std::span<const double> xs) noexcept {
    if (xs.empty()) return 0.0;
    double s = 0.0;
    for (double x : xs) s += x;
    return s / static_cast<double>(xs.size());
}

// Sample standard deviation (n-1). With a few dozen live trades the n vs n-1
// difference is several percent of the standard error, and it is in the
// direction that makes the test less likely to alarm -- exactly the wrong way
// to be optimistic in a drift check.
double sample_stdev(std::span<const double> xs, double mean) noexcept {
    if (xs.size() < 2) return 0.0;
    double ss = 0.0;
    for (double x : xs) ss += (x - mean) * (x - mean);
    return std::sqrt(ss / static_cast<double>(xs.size() - 1));
}

}  // namespace

DriftReport measure_drift(std::span<const double> backtest_returns,
                          std::span<const double> live_returns, double modelled_slippage_pts,
                          std::span<const double> live_slippage_pts, const DriftConfig& cfg) {
    DriftReport r;
    r.live_trades = live_returns.size();
    r.backtest_mean = mean_of(backtest_returns);
    r.live_mean = mean_of(live_returns);
    r.modelled_slippage_pts = modelled_slippage_pts;
    r.live_slippage_pts = mean_of(live_slippage_pts);

    if (live_returns.size() < cfg.min_trades || backtest_returns.empty()) {
        r.verdict = DriftVerdict::Insufficient;
        return r;
    }

    // The standard error comes from the LIVE sample, not the backtest. The
    // question is whether this particular live record is consistent with the
    // backtest mean, and the uncertainty in that record is what bounds the
    // answer. Borrowing the backtest's larger sample would make the test far
    // too confident about a few dozen live fills.
    const double sd = sample_stdev(live_returns, r.live_mean);
    r.live_stderr = sd / std::sqrt(static_cast<double>(live_returns.size()));

    if (r.live_stderr > 0.0) {
        r.z = (r.live_mean - r.backtest_mean) / r.live_stderr;
        r.expectancy_exceeded = r.z < -cfg.expectancy_z;
    } else {
        // Zero variance with enough trades: every live trade returned the same
        // amount. Compare the means directly rather than dividing by zero.
        r.expectancy_exceeded = r.live_mean < r.backtest_mean;
    }

    // Slippage is judged against the model's assumption, and only when there
    // is a model to judge against. A modelled slippage of zero is itself the
    // bug worth surfacing, since any positive live slippage then counts.
    if (!live_slippage_pts.empty()) {
        if (modelled_slippage_pts > 0.0) {
            r.slippage_exceeded =
                r.live_slippage_pts > modelled_slippage_pts * cfg.slippage_ratio_limit;
        } else {
            r.slippage_exceeded = r.live_slippage_pts > 0.0;
        }
    }

    r.verdict = (r.expectancy_exceeded || r.slippage_exceeded) ? DriftVerdict::Drifting
                                                               : DriftVerdict::Consistent;
    return r;
}

}  // namespace xau
