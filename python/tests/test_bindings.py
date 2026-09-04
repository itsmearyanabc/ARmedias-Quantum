"""Tests for the xaucore extension module.

The point of these is not that the bindings are wired up. It is that Python and
C++ produce *the same numbers*, because section 3 of the plan rests on Python
never reimplementing anything the engine does. Two tests below deliberately
mirror `cpp/tests/test_engine.cpp` figure for figure: if the binding ever drifts
onto a different code path, they diverge and both fail.

Run after building with -DXAU_BUILD_PYTHON=ON, with the module importable:

    cp build/<preset>/python/xaucore* python/
    cd python && python -m pytest tests/test_bindings.py -q
"""

from __future__ import annotations

import pathlib
import sys

import numpy as np
import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

xaucore = pytest.importorskip(
    "xaucore",
    reason="build with -DXAU_BUILD_PYTHON=ON and put build/<preset>/python on the path",
)

from xau_ingest import tickfmt as tfmt  # noqa: E402

# 2020-01-01T00:00:00Z, divisible by every timeframe up to D1 so bars land on
# exact boundaries and the arithmetic stays checkable by hand.
T0 = 1_577_836_800_000_000
MINUTE = 60_000_000


def make_store(tmp_path, n=60, bid0=2_650_000, slope=100, spread=200):
    """Write a tick file with the real writer, then open it with the real store.

    Going through xau_ingest.tickfmt rather than a test-only writer means this
    also exercises the format contract between the Python writer and the C++
    reader on every run.
    """
    ts = T0 + np.arange(n, dtype=np.int64) * MINUTE
    bid = bid0 + slope * np.arange(n, dtype=np.int64)
    sp = np.full(n, spread, dtype=np.int64)
    with tfmt.TickWriter(tmp_path / "XAUUSD-2020-01.bin", "XAUUSD") as w:
        w.write(tfmt.pack_ticks(ts, bid, sp))
    return xaucore.TickStore.open(str(tmp_path), "XAUUSD")


def frictionless_config():
    cfg = xaucore.BacktestConfig()
    cfg.tf = xaucore.Timeframe.M15
    cfg.initial_balance = 10_000.0
    cfg.apply_swap = False

    spec = xaucore.SymbolSpec.xauusd_default()
    spec.stops_level_pts = 0
    spec.swap_long_pts = 0.0
    spec.swap_short_pts = 0.0
    cfg.spec = spec

    costs = xaucore.CostModel()
    costs.slip_base_pts = 0.0
    costs.slip_vol_coef = 0.0
    costs.latency_us = 0
    costs.commission_per_lot_round_usd = 0.0
    cfg.costs = costs
    return cfg


# ---------------------------------------------------------------------------
# the same engine, proven by the same numbers
# ---------------------------------------------------------------------------


def test_buy_and_hold_matches_the_cpp_hand_calculation(tmp_path):
    """Mirrors buy_and_hold_reconciles_to_hand_calculation in the C++ suite.

    entry = ask on the tick at t0+15m = 2,651,500 + 200 = 2,651,700 pts
    exit  = bid on the final tick                       = 2,655,900 pts
    gross = 4,200 pts x 0.100 USD/pt/lot x 0.10 lots    =     42.00 USD
    """
    store = make_store(tmp_path)
    r = xaucore.BacktestEngine(store, frictionless_config()).run(
        xaucore.BuyAndHold(0.10, xaucore.Side.LONG)
    )

    assert len(r.trades) == 1
    t = r.trades[0]
    assert int(t["entry_pts"]) == 2_651_700
    assert int(t["exit_pts"]) == 2_655_900
    assert int(t["entry_ts"]) == T0 + 15 * MINUTE
    assert int(t["exit_ts"]) == T0 + 59 * MINUTE
    assert float(t["gross_usd"]) == pytest.approx(42.00, abs=1e-6)
    assert float(t["net_usd"]) == pytest.approx(42.00, abs=1e-6)
    assert r.final_balance == pytest.approx(10_042.00, abs=1e-6)
    # A long fills on the ask but excursions are measured on the bid, so it
    # opens one spread underwater. Same figure the C++ test pins.
    assert int(t["mae_pts"]) == -100
    assert int(t["mfe_pts"]) == 4_200


def test_null_model_loses_exactly_the_predicted_cost(tmp_path):
    """Mirrors null_model_expectancy_equals_predicted_cost in the C++ suite.

    Flat price, so gross P&L is zero on every trade and the expectancy must
    equal the round-turn cost with no sampling error at all.
    """
    store = make_store(tmp_path, n=30_000, slope=0, spread=200)

    cfg = frictionless_config()
    costs = xaucore.CostModel()
    costs.slip_base_pts = 30.0
    costs.slip_vol_coef = 0.0
    costs.latency_us = 0
    costs.commission_per_lot_round_usd = 7.0
    cfg.costs = costs

    rc = xaucore.RandomEntry.Config()
    rc.entry_prob = 0.02
    rc.hold_bars = 8
    rc.lots = 0.10

    r = xaucore.BacktestEngine(store, cfg).run(xaucore.RandomEntry(rc))

    # spread 200 + slippage 2x30 = 260 pts x 0.100 x 0.10 lots = 2.60 USD
    # commission 7.00 USD/lot x 0.10 lots                      = 0.70 USD
    predicted = -(200.0 + 2 * 30.0) * 0.100 * 0.10 - 7.00 * 0.10
    assert predicted == pytest.approx(-3.30, abs=1e-12)

    assert len(r.trades) > 10
    assert r.metrics.expectancy_usd == pytest.approx(predicted, abs=1e-9)
    assert np.allclose(r.trades["net_usd"], predicted, atol=1e-9)
    assert r.metrics.wins == 0
    assert r.metrics.net_profit < 0.0


def test_stress_multipliers_double_the_cost(tmp_path):
    store = make_store(tmp_path, n=30_000, slope=0, spread=200)
    cfg = frictionless_config()
    costs = xaucore.CostModel()
    costs.slip_base_pts = 30.0
    costs.slip_vol_coef = 0.0
    costs.latency_us = 0
    cfg.costs = costs.stressed(2.0)

    rc = xaucore.RandomEntry.Config()
    rc.entry_prob = 0.02
    rc.hold_bars = 8
    rc.lots = 0.10
    r = xaucore.BacktestEngine(store, cfg).run(xaucore.RandomEntry(rc))

    predicted = -(400.0 + 2 * 60.0) * 0.100 * 0.10
    assert r.metrics.expectancy_usd == pytest.approx(predicted, abs=1e-9)


# ---------------------------------------------------------------------------
# strategies written in Python
# ---------------------------------------------------------------------------


class EnterOnThirdBar(xaucore.Strategy):
    """The research workflow: a strategy defined in Python, run by the C++ engine."""

    def __init__(self, lots=0.10):
        super().__init__("EnterOnThirdBar", 0)
        self.lots = lots
        self.calls = 0
        self.closed = []

    def on_bar(self, ctx):
        self.calls += 1
        if self.calls == 3 and not ctx.position.is_open():
            return xaucore.Decision.enter(xaucore.Side.LONG, 0, 0, self.lots)
        return xaucore.Decision.hold()

    def on_trade_closed(self, trade):
        self.closed.append(trade.net_usd)


def test_python_strategy_is_driven_by_the_cpp_engine(tmp_path):
    store = make_store(tmp_path)
    strat = EnterOnThirdBar()
    r = xaucore.BacktestEngine(store, frictionless_config()).run(strat)

    assert strat.calls == r.stats.bars
    assert len(r.trades) == 1
    # Bar 3 closes at t0+45m, so the fill is the ask on that tick.
    assert int(r.trades[0]["entry_ts"]) == T0 + 45 * MINUTE
    assert int(r.trades[0]["entry_pts"]) == 2_654_500 + 200
    # on_trade_closed fired back into Python.
    assert len(strat.closed) == 1
    assert strat.closed[0] == pytest.approx(float(r.trades[0]["net_usd"]))


def test_bar_context_never_exposes_the_forming_bar(tmp_path):
    """The lookahead guard, checked from the Python side too."""
    seen = []

    class Peeker(xaucore.Strategy):
        def __init__(self):
            super().__init__("Peeker", 0)

        def on_bar(self, ctx):
            seen.append((ctx.bar.close_time_us(ctx.tf), ctx.now_us))
            return xaucore.Decision.hold()

    store = make_store(tmp_path, n=120)
    xaucore.BacktestEngine(store, frictionless_config()).run(Peeker())

    assert seen
    # The bar handed over is exactly the one that just closed, never one whose
    # close time is still in the future.
    assert all(close == now for close, now in seen)


def test_recent_price_helpers(tmp_path):
    got = {}

    class Grab(xaucore.Strategy):
        def __init__(self):
            super().__init__("Grab", 0)

        def on_bar(self, ctx):
            if ctx.history_size >= 3 and "closes" not in got:
                got["closes"] = np.array(ctx.recent_closes(3))
                got["highs"] = np.array(ctx.recent_highs(3))
                got["size"] = ctx.history_size
            return xaucore.Decision.hold()

    store = make_store(tmp_path, n=120)
    xaucore.BacktestEngine(store, frictionless_config()).run(Grab())

    assert got["size"] == 3
    assert len(got["closes"]) == 3
    # Rising ramp, so closes are strictly increasing and highs >= closes.
    assert np.all(np.diff(got["closes"]) > 0)
    assert np.all(got["highs"] >= got["closes"])


# ---------------------------------------------------------------------------
# shapes, types and the pieces research actually touches
# ---------------------------------------------------------------------------


def test_trades_and_equity_are_structured_arrays(tmp_path):
    store = make_store(tmp_path)
    r = xaucore.BacktestEngine(store, frictionless_config()).run(
        xaucore.BuyAndHold(0.10, xaucore.Side.LONG)
    )

    for field in ("entry_ts", "exit_ts", "side", "lots", "net_usd", "mfe_pts", "exit_reason"):
        assert field in r.trades.dtype.names
    for field in ("ts_us", "equity", "balance"):
        assert field in r.equity.dtype.names

    assert r.trades.dtype["entry_ts"] == np.int64
    assert r.trades.dtype["net_usd"] == np.float64
    assert len(r.equity) == r.stats.bars + 1  # one per bar close, plus the final mark

    # Vectorised analysis is the whole point of handing back arrays.
    assert float(r.trades["net_usd"].sum()) == pytest.approx(
        r.final_balance - r.initial_balance, abs=1e-6
    )


def test_symbol_spec_arithmetic_matches_the_contract():
    spec = xaucore.SymbolSpec.xauusd_default()
    assert spec.usd_per_point_per_lot() == pytest.approx(0.100, abs=1e-12)
    assert spec.pnl_usd(1_000, 1.0) == pytest.approx(100.0, abs=1e-9)
    assert spec.price_usd(2_650_000) == pytest.approx(2650.0, abs=1e-9)
    assert spec.points_from_usd(2650.0) == 2_650_000
    assert spec.round_lots(0.199) == pytest.approx(0.19, abs=1e-12)
    assert spec.round_lots(0.009) == 0.0


def test_size_position_matches_the_cpp_values():
    spec = xaucore.SymbolSpec.xauusd_default()
    risk = xaucore.RiskConfig()
    risk.mode = xaucore.RiskConfig.Mode.FIXED_FRACTIONAL
    risk.risk_fraction = 0.01

    # 1% of 10,000 = 100 USD over a 2,000 pt stop at 0.100 USD/pt/lot -> 0.50
    assert xaucore.size_position(risk, spec, 10_000.0, 2_000) == pytest.approx(0.50, abs=1e-12)
    assert xaucore.size_position(risk, spec, 10_000.0, 3_000) == pytest.approx(0.33, abs=1e-12)
    # No stop means no distance to size against; refusing is correct.
    assert xaucore.size_position(risk, spec, 10_000.0, 0) == 0.0
    # Below the broker minimum is no trade, not a rounded-up trade.
    assert xaucore.size_position(risk, spec, 10.0, 2_000) == 0.0


def test_store_metadata_round_trips(tmp_path):
    store = make_store(tmp_path, n=500)
    assert store.symbol == "XAUUSD"
    assert store.file_count == 1
    assert store.total_ticks == 500
    assert store.first_ts == T0
    assert store.last_ts == T0 + 499 * MINUTE


def test_timeframe_helpers():
    assert xaucore.timeframe_us(xaucore.Timeframe.M15) == 900_000_000
    assert xaucore.timeframe_name(xaucore.Timeframe.H1) == "H1"
    assert xaucore.bar_open_for(T0 + 1, xaucore.Timeframe.M15) == T0
    assert xaucore.bar_open_for(T0 + 900_000_000, xaucore.Timeframe.M15) == T0 + 900_000_000
