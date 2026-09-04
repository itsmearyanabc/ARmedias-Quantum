"""Synthetic XAUUSD tick generator.

Two jobs, both real:

1. Phase 0 test data. It lets us exercise the store and hit the >= 20M ticks/s
   benchmark gate without downloading 10 years of history first.

2. The null hypothesis. Section 8 of the plan requires running the whole
   pipeline over a synthetic random walk with matched volatility structure: if
   the model finds an edge in *this*, the harness is broken and every result so
   far is void. That test needs a generator with no edge in it by construction,
   which is exactly what this is — a driftless random walk dressed in realistic
   session intensity, spread and volatility.

Nothing here is a market simulator. There is deliberately no autocorrelation,
no mean reversion and no trend: only the microstructure envelope is realistic.
"""

from __future__ import annotations

import argparse
import datetime as dt
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .tickfmt import (
    TF_ASK,
    TF_BID,
    TF_SYNTHETIC,
    XAUUSD_POINT_DEN,
    TickWriter,
    pack_ticks,
    prices_to_points,
)

US_PER_HOUR = 3_600_000_000


@dataclass(frozen=True)
class Session:
    name: str
    ticks_per_hour: float  # Poisson mean
    spread_pts: float  # median spread, in points (1 pt = 0.001 USD)
    vol_mult: float  # multiplier on baseline per-tick sigma


# Gold's activity is extremely session-skewed: dead in late Asia, violent at the
# London/NY overlap. Getting this envelope roughly right matters, because the
# spread-to-range ratio is what decides whether an intraday edge survives costs.
QUIET = Session("quiet", 600, 700, 0.35)
ASIA = Session("asia", 1_800, 320, 0.55)
LONDON = Session("london", 6_000, 180, 1.15)
OVERLAP = Session("overlap", 9_000, 150, 1.45)
NY_LATE = Session("ny_late", 3_500, 220, 0.85)


def session_for(hour_utc: int) -> Session:
    if 7 <= hour_utc < 12:
        return LONDON
    if 12 <= hour_utc < 17:
        return OVERLAP
    if 17 <= hour_utc < 21:
        return NY_LATE
    if 21 <= hour_utc < 23:
        return QUIET
    return ASIA  # 23:00-07:00


def market_open(ts: dt.datetime) -> bool:
    """Spot gold: Sunday 22:00 UTC through Friday 21:00 UTC."""
    wd = ts.weekday()  # Mon=0 .. Sun=6
    if wd == 5:  # Saturday
        return False
    if wd == 6:  # Sunday
        return ts.hour >= 22
    if wd == 4:  # Friday
        return ts.hour < 21
    return True


def month_range(start: dt.date, months: int):
    y, m = start.year, start.month
    for _ in range(months):
        yield y, m
        m += 1
        if m == 13:
            y, m = y + 1, 1


def _hours_in_month(year: int, month: int):
    t = dt.datetime(year, month, 1, tzinfo=dt.timezone.utc)
    while t.month == month:
        yield t
        t += dt.timedelta(hours=1)


def generate(
    out_dir: str | Path,
    symbol: str = "XAUUSD",
    start: dt.date = dt.date(2024, 1, 1),
    months: int = 3,
    seed: int = 42,
    intensity: float = 1.0,
    start_price: float = 2650.0,
    daily_vol: float = 0.010,
    news_per_month: float = 8.0,
    verbose: bool = True,
) -> dict[str, int]:
    """Write `months` month-files of synthetic ticks. Returns {filename: count}."""
    rng = np.random.default_rng(seed)
    out_dir = Path(out_dir)

    # Baseline per-tick sigma, calibrated so a full day of ticks accumulates
    # roughly `daily_vol` in log terms.
    nominal_ticks_per_day = 90_000 * intensity
    base_sigma = daily_vol / np.sqrt(nominal_ticks_per_day)

    log_price = float(np.log(start_price))
    written: dict[str, int] = {}

    for year, month in month_range(start, months):
        path = out_dir / f"{symbol}-{year:04d}-{month:02d}.bin"
        chunks: list[np.ndarray] = []

        for hour_start in _hours_in_month(year, month):
            if not market_open(hour_start):
                continue

            sess = session_for(hour_start.hour)
            n = int(rng.poisson(sess.ticks_per_hour * intensity))
            if n <= 0:
                continue

            base_us = int(hour_start.timestamp()) * 1_000_000
            offsets = np.sort(rng.integers(0, US_PER_HOUR, size=n, dtype=np.int64))
            ts_us = base_us + offsets

            # Driftless random walk. No trend, no mean reversion, by design.
            sigma = base_sigma * sess.vol_mult
            steps = rng.standard_normal(n) * sigma

            # Occasional scheduled-release jump, with the spread blowout that
            # comes with it. This is what makes a news blackout necessary.
            hours_per_month = 24 * 30
            if rng.random() < news_per_month / hours_per_month:
                at = int(rng.integers(0, n))
                steps[at] += rng.standard_normal() * sigma * 60.0
                spread_spike = 6.0
            else:
                spread_spike = 1.0

            log_path = log_price + np.cumsum(steps)
            log_price = float(log_path[-1])
            mid = np.exp(log_path)

            # Lognormal spread around the session median, decaying from any
            # spike over the hour.
            noise = np.exp(rng.normal(0.0, 0.22, size=n))
            decay = 1.0 + (spread_spike - 1.0) * np.exp(-np.arange(n) / max(n / 12.0, 1.0))
            spread_usd = (sess.spread_pts / XAUUSD_POINT_DEN) * noise * decay

            bid_usd = mid - spread_usd / 2.0
            bid_pts = prices_to_points(bid_usd)
            spread_pts = np.maximum(prices_to_points(spread_usd), 1)

            chunks.append(
                pack_ticks(ts_us, bid_pts, spread_pts, TF_BID | TF_ASK | TF_SYNTHETIC)
            )

        with TickWriter(path, symbol) as w:
            for c in chunks:
                w.write(c)
            written[path.name] = w.count

        if verbose:
            print(f"  {path.name}  {written[path.name]:>10,} ticks")

    return written


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default="data/ticks/XAUUSD", help="output directory")
    ap.add_argument("--symbol", default="XAUUSD")
    ap.add_argument("--start", default="2024-01", help="first month, YYYY-MM")
    ap.add_argument("--months", type=int, default=3)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument(
        "--intensity", type=float, default=1.0, help="tick-rate multiplier (2.0 = twice as many)"
    )
    ap.add_argument("--start-price", type=float, default=2650.0)
    ap.add_argument("--daily-vol", type=float, default=0.010, help="log-return vol per day")
    args = ap.parse_args(argv)

    y, m = (int(x) for x in args.start.split("-"))
    print(f"generating {args.months} month(s) of synthetic {args.symbol} from {args.start}")
    written = generate(
        out_dir=args.out,
        symbol=args.symbol,
        start=dt.date(y, m, 1),
        months=args.months,
        seed=args.seed,
        intensity=args.intensity,
        start_price=args.start_price,
        daily_vol=args.daily_vol,
    )
    total = sum(written.values())
    print(f"\n{total:,} ticks in {len(written)} files -> {args.out}")
    print(f"({total * 16 / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
