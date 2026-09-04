"""Dukascopy historical tick fetch and decode.

Dukascopy publishes free bid/ask tick history back to ~2003. It is the backtest
backbone: the prop firm's own MT5 server only keeps weeks-to-months of history,
so it can tell us what our spread looks like *now* but not what a strategy would
have done in 2022.

Wire format, for the next person who has to debug this:

  URL   https://datafeed.dukascopy.com/datafeed/{SYM}/{YYYY}/{MM}/{DD}/{HH}h_ticks.bi5
        The month is ZERO-INDEXED. January is 00, December is 11. This is the
        single most common mistake when working with this feed; get it wrong and
        you silently ingest the wrong month.

  Body  LZMA ("alone" container, not xz), decompressing to 20-byte big-endian
        records:
            uint32  milliseconds after the hour
            uint32  ask, in instrument points
            uint32  bid, in instrument points
            float32 ask volume
            float32 bid volume

  Scale For XAUUSD the point scale is 1000 (three decimals). It differs per
        instrument, so POINT_SCALE below is keyed by symbol and every decode is
        range-checked — a wrong scale is otherwise invisible until a backtest
        produces nonsense.

Missing hours (weekends, holidays) return 404 or an empty body. Both are normal.

This module downloads from a third party. Run it deliberately, not by accident:
a full 10-year pull is tens of thousands of requests.
"""

from __future__ import annotations

import argparse
import datetime as dt
import lzma
import struct
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .tickfmt import (
    TF_ASK,
    TF_BID,
    TickWriter,
    pack_ticks,
    verify_store,
    format_report,
)

BASE_URL = "https://datafeed.dukascopy.com/datafeed"
RECORD = struct.Struct(">IIIff")
RECORD_SIZE = RECORD.size  # 20

# Points per unit price, per instrument. XAUUSD quotes to three decimals.
POINT_SCALE = {
    "XAUUSD": 1000,
    "XAGUSD": 1000,
    "EURUSD": 100_000,
    "GBPUSD": 100_000,
    "USDJPY": 1000,
}

# Our storage scale, from tickfmt: 1 point = 0.001 USD.
STORE_SCALE = 1000

PLAUSIBLE_USD = {"XAUUSD": (200.0, 20_000.0), "XAGUSD": (2.0, 500.0)}


@dataclass
class FetchStats:
    requested: int = 0
    from_cache: int = 0
    downloaded: int = 0
    empty: int = 0
    failed: int = 0
    ticks: int = 0


class DecodeError(Exception):
    pass


# ---------------------------------------------------------------------------
# fetching
# ---------------------------------------------------------------------------


def hour_url(symbol: str, when: dt.datetime) -> str:
    # NOTE the month - 1. See the module docstring.
    return (
        f"{BASE_URL}/{symbol}/{when.year:04d}/{when.month - 1:02d}/"
        f"{when.day:02d}/{when.hour:02d}h_ticks.bi5"
    )


def cache_path(cache_dir: Path, symbol: str, when: dt.datetime) -> Path:
    return (
        cache_dir
        / symbol
        / f"{when.year:04d}"
        / f"{when.month - 1:02d}"
        / f"{when.day:02d}"
        / f"{when.hour:02d}h_ticks.bi5"
    )


def fetch_hour(
    session,
    symbol: str,
    when: dt.datetime,
    cache_dir: Path,
    retries: int = 3,
    timeout: float = 30.0,
) -> bytes | None:
    """Raw .bi5 bytes for one hour. None means the feed has no data then.

    Cached on disk, so re-running an ingest costs nothing and we do not hammer
    a free public service. An empty file in the cache means "known to be empty".
    """
    cp = cache_path(cache_dir, symbol, when)
    if cp.exists():
        return cp.read_bytes() or None

    url = hour_url(symbol, when)
    delay = 1.0
    for attempt in range(retries):
        try:
            r = session.get(url, timeout=timeout)
            if r.status_code == 404:
                cp.parent.mkdir(parents=True, exist_ok=True)
                cp.write_bytes(b"")  # remember the hole
                return None
            r.raise_for_status()
            cp.parent.mkdir(parents=True, exist_ok=True)
            cp.write_bytes(r.content)
            return r.content or None
        except Exception:
            if attempt == retries - 1:
                raise
            time.sleep(delay)
            delay *= 2
    return None


# ---------------------------------------------------------------------------
# decoding
# ---------------------------------------------------------------------------


def decode_hour(raw: bytes, symbol: str, hour_start: dt.datetime) -> np.ndarray:
    """One hour of .bi5 bytes to a TICK_DTYPE array. Empty array if no ticks."""
    if not raw:
        return pack_ticks(np.empty(0, np.int64), np.empty(0, np.int64), np.empty(0, np.int64))

    try:
        body = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(raw)
    except lzma.LZMAError as e:
        raise DecodeError(f"{hour_url(symbol, hour_start)}: LZMA failed: {e}") from e

    if len(body) % RECORD_SIZE:
        raise DecodeError(
            f"{hour_url(symbol, hour_start)}: {len(body)} bytes is not a whole "
            f"number of {RECORD_SIZE}-byte records"
        )
    n = len(body) // RECORD_SIZE
    if n == 0:
        return pack_ticks(np.empty(0, np.int64), np.empty(0, np.int64), np.empty(0, np.int64))

    # Big-endian structured read, then reinterpret. Faster than struct.iter_unpack
    # by a wide margin, and we do this ~87,000 times for a 10-year pull.
    dt_be = np.dtype(
        [("ms", ">u4"), ("ask", ">u4"), ("bid", ">u4"), ("avol", ">f4"), ("bvol", ">f4")]
    )
    recs = np.frombuffer(body, dtype=dt_be, count=n)

    ask_raw = recs["ask"].astype(np.int64)
    bid_raw = recs["bid"].astype(np.int64)

    # The field order (ask before bid) is not self-describing on the wire, so
    # check it rather than trust it. A systematically negative spread means the
    # order is reversed and every downstream cost number would be wrong.
    inverted = int(np.count_nonzero(bid_raw > ask_raw))
    if inverted > n // 2:
        raise DecodeError(
            f"{hour_url(symbol, hour_start)}: bid > ask in {inverted}/{n} records — "
            "the ask/bid field order is reversed from what this decoder assumes"
        )

    src_scale = POINT_SCALE.get(symbol)
    if src_scale is None:
        raise DecodeError(f"no POINT_SCALE for {symbol}; add it before ingesting")

    # Rescale from the instrument's own point size to our storage points.
    if src_scale == STORE_SCALE:
        bid_pts = bid_raw
        ask_pts = ask_raw
    else:
        bid_pts = np.rint(bid_raw * (STORE_SCALE / src_scale)).astype(np.int64)
        ask_pts = np.rint(ask_raw * (STORE_SCALE / src_scale)).astype(np.int64)

    lo, hi = PLAUSIBLE_USD.get(symbol, (0.0, 1e9))
    med = float(np.median(bid_pts)) / STORE_SCALE
    if not (lo <= med <= hi):
        raise DecodeError(
            f"{hour_url(symbol, hour_start)}: median price {med:,.2f} USD is outside "
            f"the plausible band {lo:,.0f}-{hi:,.0f}. POINT_SCALE[{symbol}]={src_scale} "
            "is probably wrong."
        )

    base_us = int(hour_start.timestamp()) * 1_000_000
    ts_us = base_us + recs["ms"].astype(np.int64) * 1000

    # Dukascopy hours are ordered, but do not assume it.
    order = np.argsort(ts_us, kind="stable")
    ts_us = ts_us[order]
    bid_pts = bid_pts[order]
    spread = np.maximum(ask_pts[order] - bid_pts, 0)

    return pack_ticks(ts_us, bid_pts, spread, TF_BID | TF_ASK)


# ---------------------------------------------------------------------------
# driving a whole month
# ---------------------------------------------------------------------------


def hours_of_month(year: int, month: int):
    t = dt.datetime(year, month, 1, tzinfo=dt.timezone.utc)
    while t.month == month:
        yield t
        t += dt.timedelta(hours=1)


def ingest_month(
    session,
    symbol: str,
    year: int,
    month: int,
    out_dir: Path,
    cache_dir: Path,
    workers: int = 8,
    stats: FetchStats | None = None,
) -> int:
    """Fetch, decode and write one month. Returns the tick count."""
    stats = stats or FetchStats()
    hours = list(hours_of_month(year, month))

    def one(h: dt.datetime):
        raw = fetch_hour(session, symbol, h, cache_dir)
        return h, raw

    # Fetch concurrently (I/O bound), then decode and write strictly in order —
    # the writer enforces monotonic timestamps and would reject shuffled hours.
    results: dict[dt.datetime, bytes | None] = {}
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for h, raw in pool.map(one, hours):
            results[h] = raw
            stats.requested += 1
            if raw is None:
                stats.empty += 1
            else:
                stats.downloaded += 1

    path = out_dir / f"{symbol}-{year:04d}-{month:02d}.bin"
    with TickWriter(path, symbol) as w:
        for h in hours:
            raw = results.get(h)
            if not raw:
                continue
            arr = decode_hour(raw, symbol, h)
            if len(arr):
                w.write(arr)
        count = w.count

    stats.ticks += count
    return count


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Fetch Dukascopy tick history into the tick store.")
    ap.add_argument("--symbol", default="XAUUSD")
    ap.add_argument("--start", required=True, help="first month, YYYY-MM")
    ap.add_argument("--end", required=True, help="last month inclusive, YYYY-MM")
    ap.add_argument("--out", default="data/ticks/XAUUSD")
    ap.add_argument("--cache", default="data/raw/dukascopy")
    ap.add_argument("--workers", type=int, default=8, help="concurrent requests (be polite)")
    ap.add_argument("--verify", action="store_true", help="verify the store when done")
    args = ap.parse_args(argv)

    try:
        import requests
    except ImportError:
        print("this needs `pip install requests`")
        return 2

    y0, m0 = (int(x) for x in args.start.split("-"))
    y1, m1 = (int(x) for x in args.end.split("-"))
    months = []
    y, m = y0, m0
    while (y, m) <= (y1, m1):
        months.append((y, m))
        m += 1
        if m == 13:
            y, m = y + 1, 1

    out_dir = Path(args.out)
    cache_dir = Path(args.cache)
    stats = FetchStats()

    session = requests.Session()
    session.headers.update({"User-Agent": "xau-terminal/0.1 (research; contact via repo)"})

    print(f"{args.symbol}: {len(months)} months, {len(months) * 730:,} hours to fetch")
    print(f"cache {cache_dir}  ->  store {out_dir}\n")

    t0 = time.time()
    for y, m in months:
        n = ingest_month(session, args.symbol, y, m, out_dir, cache_dir, args.workers, stats)
        print(f"  {args.symbol}-{y:04d}-{m:02d}.bin  {n:>12,} ticks")

    dur = time.time() - t0
    print(
        f"\n{stats.ticks:,} ticks in {dur / 60:.1f} min "
        f"({stats.downloaded:,} hours with data, {stats.empty:,} empty)"
    )
    print(f"{stats.ticks * 16 / 1e9:.2f} GB in the store")

    if args.verify:
        print()
        print(format_report(verify_store(out_dir, args.symbol)))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
