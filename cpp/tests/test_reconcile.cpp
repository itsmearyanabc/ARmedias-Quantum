// Reconciliation and drift. Each test names a way a live book goes wrong
// without anything crashing.

#include "harness.hpp"

#include "xau/reconcile.hpp"

#include <cmath>
#include <random>
#include <vector>

using namespace xau;

namespace {

BookEntry pos(std::uint64_t t, Side s, double lots, Points px) {
    BookEntry e;
    e.ticket = t;
    e.side = s;
    e.lots = lots;
    e.open_pts = px;
    return e;
}

bool has(const ReconcileResult& r, Discrepancy k, std::uint64_t ticket) {
    for (const ReconcileIssue& i : r.issues) {
        if (i.kind == k && i.ticket == ticket) return true;
    }
    return false;
}

}  // namespace

XAU_TEST(identical_books_reconcile_clean) {
    const std::vector<BookEntry> a = {pos(1, Side::Long, 0.10, 2650000),
                                      pos(2, Side::Short, 0.05, 2651000)};
    const ReconcileResult r = reconcile(a, a);
    CHECK(r.clean());
    CHECK(!r.must_halt());
}

XAU_TEST(a_position_the_broker_lost_is_reported_and_halts) {
    // We think we are long; the broker has nothing. Often a rejected order we
    // recorded as filled. The next "close" would open a fresh short.
    const std::vector<BookEntry> ours = {pos(7, Side::Long, 0.10, 2650000)};
    const std::vector<BookEntry> broker = {};
    const ReconcileResult        r = reconcile(ours, broker);
    CHECK(has(r, Discrepancy::MissingAtBroker, 7));
    CHECK(r.must_halt());
}

XAU_TEST(a_position_we_never_opened_is_reported_and_halts) {
    // A manual trade, a second EA, or state lost across a restart. Adding to
    // it means trading a book whose risk nobody computed.
    const std::vector<BookEntry> ours = {};
    const std::vector<BookEntry> broker = {pos(99, Side::Short, 1.00, 2640000)};
    const ReconcileResult        r = reconcile(ours, broker);
    CHECK(has(r, Discrepancy::UnknownAtBroker, 99));
    CHECK(r.must_halt());
}

XAU_TEST(a_reversed_position_is_a_side_mismatch_not_a_size_one) {
    // Same ticket, same size, opposite direction. Reporting that as a size
    // difference -- or not at all, since the sizes match -- would hide the
    // single worst thing a book can get wrong.
    const std::vector<BookEntry> ours = {pos(3, Side::Long, 0.10, 2650000)};
    const std::vector<BookEntry> broker = {pos(3, Side::Short, 0.10, 2650000)};
    const ReconcileResult        r = reconcile(ours, broker);
    CHECK(has(r, Discrepancy::SideMismatch, 3));
    CHECK(!has(r, Discrepancy::SizeMismatch, 3));
    CHECK(r.must_halt());
}

XAU_TEST(a_partial_fill_is_a_size_mismatch) {
    const std::vector<BookEntry> ours = {pos(4, Side::Long, 0.10, 2650000)};
    const std::vector<BookEntry> broker = {pos(4, Side::Long, 0.04, 2650000)};
    const ReconcileResult        r = reconcile(ours, broker);
    CHECK(has(r, Discrepancy::SizeMismatch, 4));
    CHECK(r.must_halt());
}

XAU_TEST(decimal_volume_noise_is_not_a_partial_fill) {
    // 0.1 is not exactly representable in binary. A broker reporting 0.1 and a
    // book holding 0.1 computed as 0.3 - 0.2 must still agree, or every trade
    // halts the system on a rounding artefact.
    const std::vector<BookEntry> ours = {pos(5, Side::Long, 0.3 - 0.2, 2650000)};
    const std::vector<BookEntry> broker = {pos(5, Side::Long, 0.1, 2650000)};
    CHECK(reconcile(ours, broker).clean());
}

XAU_TEST(price_drift_is_reported_but_does_not_halt) {
    // A bad fill on a correct position is a cost, not a broken book. Halting
    // on it would take the system offline every time slippage spiked.
    const std::vector<BookEntry> ours = {pos(6, Side::Long, 0.10, 2650000)};
    const std::vector<BookEntry> broker = {pos(6, Side::Long, 0.10, 2651200)};   // 1.20 USD
    ReconcileTolerance           tol;
    tol.price_tolerance_pts = 500;
    const ReconcileResult r = reconcile(ours, broker, tol);
    CHECK(has(r, Discrepancy::PriceMismatch, 6));
    CHECK(!r.must_halt());
}

XAU_TEST(the_report_order_is_deterministic) {
    // Unordered-map iteration is unspecified. Two runs over the same books
    // must print the same report, or diffing them means nothing.
    const std::vector<BookEntry> ours = {};
    const std::vector<BookEntry> broker = {pos(50, Side::Long, 0.1, 1), pos(10, Side::Long, 0.1, 1),
                                           pos(30, Side::Long, 0.1, 1)};
    const ReconcileResult r = reconcile(ours, broker);
    CHECK_EQ(r.issues.size(), std::size_t{3});
    CHECK_EQ(r.issues[0].ticket, std::uint64_t{10});
    CHECK_EQ(r.issues[1].ticket, std::uint64_t{30});
    CHECK_EQ(r.issues[2].ticket, std::uint64_t{50});
}

// --- drift -----------------------------------------------------------------

namespace {

std::vector<double> draw(double mean, double sd, std::size_t n, std::uint64_t seed) {
    std::mt19937_64                  rng(seed);
    std::normal_distribution<double> d(mean, sd);
    std::vector<double>              v(n);
    for (double& x : v) x = d(rng);
    return v;
}

}  // namespace

XAU_TEST(too_few_live_trades_is_insufficient_not_consistent) {
    // Three good fills are not evidence the strategy works live. Folding
    // "no alarm yet" into "consistent" is how a system trades for weeks on
    // the strength of a lucky start.
    const auto  bt = draw(1.0, 5.0, 1000, 1);
    const auto  live = draw(1.0, 5.0, 3, 2);
    DriftConfig cfg;
    cfg.min_trades = 40;
    const DriftReport r = measure_drift(bt, live, 15.0, {}, cfg);
    CHECK(r.verdict == DriftVerdict::Insufficient);
}

XAU_TEST(live_trading_that_matches_the_backtest_is_consistent) {
    // Deterministic rather than drawn: a live record whose mean equals the
    // backtest's exactly. A test built on a random draw would depend on the
    // standard library's distribution, which differs between MSVC and
    // libstdc++, and could pass on one platform and fail on the other.
    std::vector<double> bt, live;
    for (int i = 0; i < 1000; ++i) bt.push_back((i % 2 == 0) ? 6.0 : -4.0);   // mean 1.0
    for (int i = 0; i < 200; ++i) live.push_back((i % 2 == 0) ? 6.0 : -4.0);  // mean 1.0

    const DriftReport r = measure_drift(bt, live, 15.0, {}, DriftConfig{});
    CHECK(r.verdict == DriftVerdict::Consistent);
    CHECK_NEAR(r.z, 0.0, 1e-9);
}

XAU_TEST(live_expectancy_well_below_backtest_is_drift) {
    // The backtest made +1.0 a trade; live makes -2.0 on a sample large enough
    // that the difference is not noise.
    std::vector<double> bt, live;
    for (int i = 0; i < 1000; ++i) bt.push_back((i % 2 == 0) ? 6.0 : -4.0);   // mean +1
    for (int i = 0; i < 200; ++i) live.push_back((i % 2 == 0) ? 3.0 : -7.0);  // mean -2

    const DriftReport r = measure_drift(bt, live, 15.0, {}, DriftConfig{});
    CHECK(r.expectancy_exceeded);
    CHECK(r.verdict == DriftVerdict::Drifting);
    CHECK(r.z < -2.0);
}

XAU_TEST(doing_better_than_the_backtest_is_not_drift) {
    // One-sided on purpose. An unexpectedly good live run deserves a look, but
    // it is not the failure mode that loses money, and alarming on it would
    // teach the operator to ignore the alarm.
    std::vector<double> bt, live;
    for (int i = 0; i < 1000; ++i) bt.push_back((i % 2 == 0) ? 6.0 : -4.0);   // mean +1
    for (int i = 0; i < 200; ++i) live.push_back((i % 2 == 0) ? 9.0 : -1.0);  // mean +4

    const DriftReport r = measure_drift(bt, live, 15.0, {}, DriftConfig{});
    CHECK(!r.expectancy_exceeded);
    CHECK(r.verdict == DriftVerdict::Consistent);
}

XAU_TEST(slippage_beyond_the_cost_model_is_drift) {
    // The backtest assumed 15 points of slippage. Live is filling at 40. The
    // strategy may be fine; the COST MODEL is wrong, and every backtest number
    // built on it is optimistic by exactly that gap.
    std::vector<double> bt, live;
    for (int i = 0; i < 1000; ++i) bt.push_back((i % 2 == 0) ? 6.0 : -4.0);
    for (int i = 0; i < 200; ++i) live.push_back((i % 2 == 0) ? 6.0 : -4.0);
    const std::vector<double> slips(200, 40.0);

    const DriftReport r = measure_drift(bt, live, 15.0, slips, DriftConfig{});
    CHECK(r.slippage_exceeded);
    CHECK(r.verdict == DriftVerdict::Drifting);
    CHECK_NEAR(r.live_slippage_pts, 40.0, 1e-9);
}

XAU_TEST(a_zero_slippage_model_flags_any_real_slippage) {
    // A cost model that assumes zero slippage is the bug. Any positive live
    // slippage must surface it rather than dividing by zero or passing.
    std::vector<double> bt, live;
    for (int i = 0; i < 1000; ++i) bt.push_back(1.0);
    for (int i = 0; i < 200; ++i) live.push_back(1.0);
    const std::vector<double> slips(200, 3.0);

    const DriftReport r = measure_drift(bt, live, 0.0, slips, DriftConfig{});
    CHECK(r.slippage_exceeded);
}

XAU_TEST(stderr_comes_from_the_live_sample) {
    // The uncertainty that bounds the verdict is the live record's, not the
    // backtest's. Borrowing the backtest's much larger n would make a few
    // dozen live fills look far more certain than they are.
    std::vector<double> bt, live;
    for (int i = 0; i < 10000; ++i) bt.push_back((i % 2 == 0) ? 6.0 : -4.0);
    for (int i = 0; i < 40; ++i) live.push_back((i % 2 == 0) ? 6.0 : -4.0);

    const DriftReport r = measure_drift(bt, live, 15.0, {}, DriftConfig{});
    // live: 40 samples of +6/-4, sample sd = sqrt(sum((x-1)^2)/39) = sqrt(1000/39)
    const double expected = std::sqrt(1000.0 / 39.0) / std::sqrt(40.0);
    CHECK_NEAR(r.live_stderr, expected, 1e-9);
}
