// Reconciliation and live-vs-backtest drift.
//
// Two different questions, both of which a live system has to answer on its
// own, because by the time a human notices either one the money is gone.
//
//   RECONCILE  Does the broker's book match ours, right now?
//              Positions we think we hold and do not, positions we hold and do
//              not know about, sizes and sides that disagree. Every one of these
//              has an innocent-looking cause -- a partial fill, a manual trade,
//              a restart that lost state -- and every one means the next order
//              is being placed against a book nobody actually understands.
//
//   DRIFT      Is live trading still the strategy we backtested?
//              Fills slipping further than the cost model assumed, a win rate
//              that has quietly moved, an expectancy that no longer sits inside
//              the distribution the backtest produced. This is Phase 8's gate,
//              and it is the only honest check on whether a backtest described
//              the program that is actually running.
//
// Both are pure functions of their inputs. No broker calls happen here, which
// is what makes them testable -- and a safety check that can only be tested
// against a live account is a safety check that has not been tested.

#ifndef XAU_RECONCILE_HPP
#define XAU_RECONCILE_HPP

#include "xau/order.hpp"
#include "xau/types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xau {

// --- reconciliation --------------------------------------------------------

struct BookEntry {
    std::uint64_t ticket = 0;
    Side          side = Side::None;
    double        lots = 0.0;
    Points        open_pts = 0;
};

enum class Discrepancy : std::uint8_t {
    MissingAtBroker,   // we believe it is open; the broker has no such position
    UnknownAtBroker,   // the broker holds it; we never opened it
    SideMismatch,      // same ticket, opposite direction -- never benign
    SizeMismatch,      // same ticket, different volume (a partial fill or close)
    PriceMismatch,     // opened further from our price than tolerance allows
};

[[nodiscard]] const char* discrepancy_name(Discrepancy d) noexcept;

struct ReconcileIssue {
    Discrepancy   kind;
    std::uint64_t ticket = 0;
    double        ours = 0.0;
    double        theirs = 0.0;
};

struct ReconcileTolerance {
    // Volume is decimal on the broker and binary here; anything inside a
    // hundredth of a step is representation noise, not a partial fill.
    double lots_epsilon = 1e-6;

    // A recorded open price can legitimately differ from the fill by
    // slippage. Beyond this it is not slippage, it is a different trade.
    Points price_tolerance_pts = 500;
};

struct ReconcileResult {
    std::vector<ReconcileIssue> issues;

    [[nodiscard]] bool clean() const noexcept { return issues.empty(); }

    // Whether to stop trading. Price drift alone is reported but does not
    // halt: a bad fill on a correct position is a cost, not a broken book.
    // Anything that means the positions themselves disagree does halt.
    [[nodiscard]] bool must_halt() const noexcept;
};

[[nodiscard]] ReconcileResult reconcile(std::span<const BookEntry> ours,
                                        std::span<const BookEntry> broker,
                                        const ReconcileTolerance& tol = {});

// --- drift -----------------------------------------------------------------

enum class DriftVerdict : std::uint8_t {
    // Not enough live trades to say anything. Reported as its own state rather
    // than folded into "ok", because "no alarm yet" and "confirmed consistent"
    // are different claims and conflating them is how a system trades for
    // weeks on the strength of three lucky fills.
    Insufficient,
    Consistent,
    Drifting,
};

[[nodiscard]] const char* drift_verdict_name(DriftVerdict v) noexcept;

struct DriftConfig {
    // Below this many live trades every verdict is Insufficient. Phase 8's
    // own bar is 40.
    std::size_t min_trades = 40;

    // Expectancy: how far below the backtest mean, in standard errors of the
    // LIVE sample, before it counts as drift. One-sided on purpose -- doing
    // better than the backtest is worth investigating, but it is not the
    // failure that loses money.
    double expectancy_z = 2.0;

    // Slippage: how much worse than the cost model's assumption, as a
    // multiple, before the cost model itself is considered wrong.
    double slippage_ratio_limit = 1.5;
};

struct DriftReport {
    DriftVerdict verdict = DriftVerdict::Insufficient;
    std::size_t  live_trades = 0;

    double backtest_mean = 0.0;
    double live_mean = 0.0;
    double live_stderr = 0.0;
    double z = 0.0;   // (live - backtest) / live_stderr; negative is worse

    double modelled_slippage_pts = 0.0;
    double live_slippage_pts = 0.0;
    bool   slippage_exceeded = false;
    bool   expectancy_exceeded = false;
};

// backtest_returns: per-trade net returns the strategy produced in backtest.
// live_returns: the same measure from live trading, same units.
// live_slippage_pts: realised slippage per live fill, positive = worse.
[[nodiscard]] DriftReport measure_drift(std::span<const double> backtest_returns,
                                        std::span<const double> live_returns,
                                        double                  modelled_slippage_pts,
                                        std::span<const double> live_slippage_pts,
                                        const DriftConfig&      cfg = {});

}  // namespace xau

#endif  // XAU_RECONCILE_HPP
