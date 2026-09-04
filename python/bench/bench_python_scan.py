"""Measure Python's tick-replay throughput — the baseline C++ has to beat.

This exists so the "why C++" argument in docs/PLAN.md section 3 rests on a
number measured on *this* machine and *this* data, not on a guess. Re-run it if
you ever doubt the decision.

Three modes, and the distinction between them is the whole point:

  numpy vectorised   Fast, but cannot express path-dependent logic. Whether a
                     stop is hit depends on the state the previous tick left,
                     and that dependency is precisely what vectorisation
                     removes. Fine for a screening pass, useless for the
                     event-driven engine.

  python per-tick    What an event-driven backtest actually needs. This is the
                     honest comparison against the C++ engine.

  python + work      The same loop with a token amount of per-tick strategy
                     work (an EMA update and two barrier checks). Real
                     strategies do much more; this shows which way the gap moves
                     once the loop stops being trivial.

    python -m bench.bench_python_scan [store_dir] [symbol]
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from xau_ingest import tickfmt as tf  # noqa: E402

CPP_TARGET_TPS = 20e6  # the Phase 0 gate
SUBSET = 300_000  # enough for a stable per-tick timing without a long wait


def vectorised(arr) -> float:
    t0 = time.perf_counter()
    reps = 3
    for _ in range(reps):
        bid = arr["bid_pts"].astype(np.int64)
        sp = arr["spread_pts"].astype(np.int64)
        _ = int((bid * 2 + sp).sum()), int(bid.min()), int(bid.max())
        _ = int(np.count_nonzero(sp > 500))
    return (time.perf_counter() - t0) / reps


def per_tick(sub) -> float:
    t0 = time.perf_counter()
    acc = 0
    lo = 1 << 62
    hi = -(1 << 62)
    wide = 0
    for t in sub:
        b = int(t["bid_pts"])
        s = int(t["spread_pts"])
        acc += b * 2 + s
        if b < lo:
            lo = b
        if b > hi:
            hi = b
        if s > 500:
            wide += 1
    return time.perf_counter() - t0


def per_tick_with_work(sub) -> float:
    """A token strategy: one EMA and two barrier checks per tick."""
    t0 = time.perf_counter()
    ema = float(sub[0]["bid_pts"])
    alpha = 0.001
    upper = ema + 2000.0
    lower = ema - 2000.0
    hits = 0
    for t in sub:
        b = float(t["bid_pts"])
        s = float(t["spread_pts"])
        ema += alpha * (b - ema)
        if b + s >= upper or b <= lower:
            hits += 1
            upper = ema + 2000.0
            lower = ema - 2000.0
    return time.perf_counter() - t0


def main(argv: list[str] | None = None) -> int:
    argv = argv if argv is not None else sys.argv[1:]
    store = argv[0] if argv else "../data/ticks/XAUUSD"
    symbol = argv[1] if len(argv) > 1 else "XAUUSD"

    paths = tf.store_files(store, symbol)
    if not paths:
        print(f"no {symbol} files in {store}")
        print("generate some first:  python -m xau_ingest.synth --months 3")
        return 2

    hdr, arr = tf.open_ticks(paths[0])
    n = len(arr)
    sub = arr[:SUBSET]
    _ = int(arr["bid_pts"].sum())  # warm the page cache

    print(f"{paths[0].name}   {n:,} ticks\n")

    rates = {
        "numpy vectorised": n / vectorised(arr),
        "python per-tick": len(sub) / per_tick(sub),
        "python + strategy work": len(sub) / per_tick_with_work(sub),
    }
    for label, tps in rates.items():
        print(f"  {label:<24} {tps / 1e6:8.2f} M ticks/s")
    print(f"  {'C++ gate (target)':<24} {CPP_TARGET_TPS / 1e6:8.2f} M ticks/s")

    # Order of magnitude for a decade of real XAUUSD ticks. Replace with the
    # measured count once the real store exists.
    decade = 300e6
    paths_cpcv = 1000
    cores = 12
    print(f"\none pass over ~{decade / 1e6:.0f}M ticks (about 10 years):")
    for label, tps in rates.items():
        print(f"  {label:<24} {decade / tps / 60:8.1f} min")
    print(f"  {'C++ gate (target)':<24} {decade / CPP_TARGET_TPS:8.1f} s")

    slowest = rates["python + strategy work"]
    print(f"\n{paths_cpcv}-path CPCV across {cores} cores:")
    print(f"  {'python + work':<24} {decade * paths_cpcv / slowest / cores / 3600:8.1f} hours")
    print(f"  {'C++ gate':<24} {decade * paths_cpcv / CPP_TARGET_TPS / cores / 60:8.1f} min")
    print(
        f"\nspeedup on the event-driven path: "
        f"{CPP_TARGET_TPS / slowest:.0f}x, and it widens as the strategy does more work"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
