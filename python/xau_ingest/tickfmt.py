"""Binary tick format: writer, reader, verifier.

This module mirrors cpp/core/include/xau/types.hpp byte-for-byte. If you change
a field there, change it here in the same commit and bump FORMAT_VERSION in
both. The round-trip test in tests/test_tickfmt.py pins the layout.

Layout (little-endian, no padding):

    FileHeader, 64 bytes
        magic       8s   "XAUTICK1"
        version     I
        tick_size   I    == 16
        point_num   i    1 point = point_num / point_den USD
        point_den   i
        first_ts_us q    microseconds since epoch, UTC
        last_ts_us  q
        count       Q
        symbol      16s  NUL-padded

    then `count` Tick records, 16 bytes each
        ts_us       q
        bid_pts     i
        spread_pts  H    ask = bid + spread
        flags       H
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

import numpy as np

FORMAT_VERSION = 1
MAGIC = b"XAUTICK1"

# 1 point = 0.001 USD for XAUUSD: three decimals, matching Dukascopy and
# three-digit MT5 brokers. Integer prices keep replay deterministic.
XAUUSD_POINT_NUM = 1
XAUUSD_POINT_DEN = 1000

# Tick flags — must match xau::TickFlag.
TF_BID = 1 << 0
TF_ASK = 1 << 1
TF_LAST = 1 << 2
TF_VOLUME = 1 << 3
TF_SPREAD_SAT = 1 << 4  # spread exceeded uint16 and was clamped
TF_SYNTHETIC = 1 << 5  # generated, not observed

TICK_DTYPE = np.dtype(
    [
        ("ts_us", "<i8"),
        ("bid_pts", "<i4"),
        ("spread_pts", "<u2"),
        ("flags", "<u2"),
    ]
)

_HEADER = struct.Struct("<8sIIiiqqQ16s")
HEADER_BYTES = _HEADER.size

MAX_SPREAD_PTS = 0xFFFF

# Sanity band for XAUUSD in points. Gold has never been below $200 and is not
# plausibly above $20,000; anything outside means the decode scale is wrong.
PLAUSIBLE_MIN_PTS = 200 * XAUUSD_POINT_DEN
PLAUSIBLE_MAX_PTS = 20_000 * XAUUSD_POINT_DEN

assert TICK_DTYPE.itemsize == 16, "Tick must be 16 bytes to match the C++ struct"
assert HEADER_BYTES == 64, "FileHeader must be 64 bytes to match the C++ struct"
assert TICK_DTYPE.fields is not None
assert TICK_DTYPE.fields["ts_us"][1] == 0
assert TICK_DTYPE.fields["bid_pts"][1] == 8
assert TICK_DTYPE.fields["spread_pts"][1] == 12
assert TICK_DTYPE.fields["flags"][1] == 14


@dataclass(frozen=True)
class Header:
    version: int
    tick_size: int
    point_num: int
    point_den: int
    first_ts_us: int
    last_ts_us: int
    count: int
    symbol: str

    @property
    def point_usd(self) -> float:
        return self.point_num / self.point_den

    def to_usd(self, pts: int | float | np.ndarray):
        return np.asarray(pts) * self.point_num / self.point_den


class FormatError(Exception):
    """The file is not a tick store at this version, or is inconsistent."""


# ---------------------------------------------------------------------------
# building tick arrays
# ---------------------------------------------------------------------------


def pack_ticks(
    ts_us: np.ndarray,
    bid_pts: np.ndarray,
    spread_pts: np.ndarray,
    flags: int | np.ndarray = TF_BID | TF_ASK,
) -> np.ndarray:
    """Build a TICK_DTYPE array, clamping spreads that overflow uint16.

    A clamped spread gets TF_SPREAD_SAT so the cost model can exclude those
    ticks rather than quietly trading a spread that was wider than recorded.
    """
    n = len(ts_us)
    if not (len(bid_pts) == len(spread_pts) == n):
        raise ValueError("ts_us, bid_pts and spread_pts must be the same length")

    out = np.zeros(n, dtype=TICK_DTYPE)
    out["ts_us"] = np.asarray(ts_us, dtype="<i8")
    out["bid_pts"] = np.asarray(bid_pts, dtype="<i8").astype("<i4", casting="unsafe")

    sp = np.asarray(spread_pts, dtype="<i8")
    if np.any(sp < 0):
        raise ValueError("negative spread: bid and ask are probably swapped")
    saturated = sp > MAX_SPREAD_PTS
    out["spread_pts"] = np.clip(sp, 0, MAX_SPREAD_PTS).astype("<u2")

    fl = np.full(n, flags, dtype="<u2") if np.isscalar(flags) else np.asarray(flags, dtype="<u2")
    fl = fl | (saturated.astype("<u2") * TF_SPREAD_SAT)
    out["flags"] = fl
    return out


def prices_to_points(usd: np.ndarray, point_den: int = XAUUSD_POINT_DEN) -> np.ndarray:
    """USD prices to integer points.

    Uses numpy's round-half-to-even (banker's rounding). The choice is
    arbitrary — half a point is 0.0005 USD, far below anything that moves a P&L
    — but it must be *stated*, because from here on prices are integers and
    every barrier and stop comparison is exact. Determinism is the requirement,
    not a particular tie-breaking rule.
    """
    return np.rint(np.asarray(usd, dtype=np.float64) * point_den).astype(np.int64)


# ---------------------------------------------------------------------------
# writing
# ---------------------------------------------------------------------------


class TickWriter:
    """Streams ticks to one month file, patching the header on close.

    Enforces non-decreasing timestamps across every chunk. Our own writer must
    never produce data the C++ verifier would reject — the verifier exists to
    catch third-party data and corruption, not our own bugs.
    """

    def __init__(
        self,
        path: str | Path,
        symbol: str,
        point_num: int = XAUUSD_POINT_NUM,
        point_den: int = XAUUSD_POINT_DEN,
    ) -> None:
        self.path = Path(path)
        self.symbol = symbol
        self.point_num = point_num
        self.point_den = point_den
        self._count = 0
        self._first_ts = 0
        self._last_ts = 0
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._f = self.path.open("wb")
        self._f.write(b"\0" * HEADER_BYTES)  # placeholder, patched in close()

    def write(self, ticks: np.ndarray) -> None:
        if ticks.dtype != TICK_DTYPE:
            raise TypeError(f"expected TICK_DTYPE, got {ticks.dtype}")
        if len(ticks) == 0:
            return

        ts = ticks["ts_us"]
        if np.any(np.diff(ts) < 0):
            bad = int(np.argmax(np.diff(ts) < 0))
            raise ValueError(
                f"timestamps go backwards at index {bad}: {ts[bad]} -> {ts[bad + 1]}"
            )
        if self._count and ts[0] < self._last_ts:
            raise ValueError(
                f"chunk starts at {ts[0]}, before the previous chunk ended at {self._last_ts}"
            )

        if self._count == 0:
            self._first_ts = int(ts[0])
        self._last_ts = int(ts[-1])
        self._count += len(ticks)
        self._f.write(ticks.tobytes())

    def close(self) -> None:
        if self._f.closed:
            return
        sym = self.symbol.encode("ascii")[:16]
        header = _HEADER.pack(
            MAGIC,
            FORMAT_VERSION,
            TICK_DTYPE.itemsize,
            self.point_num,
            self.point_den,
            self._first_ts,
            self._last_ts,
            self._count,
            sym,
        )
        self._f.seek(0)
        self._f.write(header)
        self._f.close()

    @property
    def count(self) -> int:
        return self._count

    def __enter__(self) -> "TickWriter":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


# ---------------------------------------------------------------------------
# reading
# ---------------------------------------------------------------------------


def read_header(path: str | Path) -> Header:
    path = Path(path)
    with path.open("rb") as f:
        raw = f.read(HEADER_BYTES)
    if len(raw) < HEADER_BYTES:
        raise FormatError(f"{path}: shorter than a header")

    magic, version, tick_size, pnum, pden, first_ts, last_ts, count, sym = _HEADER.unpack(raw)
    if magic != MAGIC:
        raise FormatError(f"{path}: bad magic {magic!r}")
    if version != FORMAT_VERSION:
        raise FormatError(f"{path}: format version {version}, expected {FORMAT_VERSION}")
    if tick_size != TICK_DTYPE.itemsize:
        raise FormatError(f"{path}: tick_size {tick_size}, expected {TICK_DTYPE.itemsize}")
    if pden == 0:
        raise FormatError(f"{path}: point_den is zero")

    payload = path.stat().st_size - HEADER_BYTES
    if payload % TICK_DTYPE.itemsize:
        raise FormatError(f"{path}: payload is not a whole number of ticks")
    on_disk = payload // TICK_DTYPE.itemsize
    if on_disk != count:
        raise FormatError(f"{path}: header says {count} ticks, file holds {on_disk}")

    return Header(
        version=version,
        tick_size=tick_size,
        point_num=pnum,
        point_den=pden,
        first_ts_us=first_ts,
        last_ts_us=last_ts,
        count=count,
        symbol=sym.rstrip(b"\0").decode("ascii", "replace"),
    )


def open_ticks(path: str | Path) -> tuple[Header, np.ndarray]:
    """Header plus a read-only memmap of the ticks — no copy, no full read."""
    hdr = read_header(path)
    if hdr.count == 0:
        return hdr, np.zeros(0, dtype=TICK_DTYPE)
    arr = np.memmap(path, dtype=TICK_DTYPE, mode="r", offset=HEADER_BYTES, shape=(hdr.count,))
    return hdr, arr


def store_files(directory: str | Path, symbol: str) -> list[Path]:
    """`<symbol>-YYYY-MM.bin` files, chronologically ordered."""
    directory = Path(directory)
    if not directory.is_dir():
        raise FileNotFoundError(f"not a directory: {directory}")
    out = []
    for p in directory.iterdir():
        if not p.is_file() or p.suffix != ".bin":
            continue
        stem = p.stem
        if not stem.startswith(symbol + "-"):
            continue
        rest = stem[len(symbol) + 1 :]
        if len(rest) == 7 and rest[4] == "-" and rest[:4].isdigit() and rest[5:].isdigit():
            if 1 <= int(rest[5:]) <= 12:
                out.append(p)
    return sorted(out)


# ---------------------------------------------------------------------------
# verification — the Phase 0 gate evidence
# ---------------------------------------------------------------------------


@dataclass
class VerifyReport:
    files: int = 0
    ticks: int = 0
    non_monotonic: int = 0
    out_of_range: int = 0
    zero_spread: int = 0
    saturated_spread: int = 0
    gaps: int = 0
    largest_gap_us: int = 0
    largest_gap_at: int = 0
    min_bid_pts: int = 0
    max_bid_pts: int = 0
    first_ts_us: int = 0
    last_ts_us: int = 0
    synthetic_ticks: int = 0

    @property
    def mixed_provenance(self) -> bool:
        """Real and generated ticks in the same store.

        Now that both exist this is a live hazard, and a silent one: a store
        that is half real and half synthetic produces a backtest whose result
        means nothing, with nothing visibly wrong. Keep them in separate
        directories.
        """
        return 0 < self.synthetic_ticks < self.ticks

    @property
    def ok(self) -> bool:
        return (
            self.non_monotonic == 0
            and self.out_of_range == 0
            and not self.mixed_provenance
        )


def verify_store(
    directory: str | Path,
    symbol: str,
    gap_threshold_us: int = 60_000_000,
    min_pts: int = PLAUSIBLE_MIN_PTS,
    max_pts: int = PLAUSIBLE_MAX_PTS,
) -> VerifyReport:
    """Walk every tick, checking order, plausibility and gaps.

    Mirrors xau::TickStore::verify so the gate can be evidenced from either
    side. Weekend closes are real gaps, so a weekday-sized threshold will
    always report ~1 gap per week — that is expected, not a failure.
    """
    rep = VerifyReport()
    paths = store_files(directory, symbol)
    if not paths:
        raise FileNotFoundError(f"no {symbol}-YYYY-MM.bin files in {directory}")

    prev_last_ts: int | None = None
    for p in paths:
        hdr, arr = open_ticks(p)
        rep.files += 1
        if hdr.count == 0:
            continue

        ts = arr["ts_us"].astype(np.int64)
        bid = arr["bid_pts"].astype(np.int64)
        spread = arr["spread_pts"].astype(np.int64)
        flags = arr["flags"].astype(np.int64)

        d = np.diff(ts)
        rep.non_monotonic += int(np.count_nonzero(d < 0))

        if prev_last_ts is not None:
            if ts[0] < prev_last_ts:
                rep.non_monotonic += 1
            else:
                cross = int(ts[0] - prev_last_ts)
                if cross > gap_threshold_us:
                    rep.gaps += 1
                    if cross > rep.largest_gap_us:
                        rep.largest_gap_us = cross
                        rep.largest_gap_at = int(prev_last_ts)

        big = d > gap_threshold_us
        rep.gaps += int(np.count_nonzero(big))
        if np.any(big):
            i = int(np.argmax(d))
            if int(d[i]) > rep.largest_gap_us:
                rep.largest_gap_us = int(d[i])
                rep.largest_gap_at = int(ts[i])

        rep.out_of_range += int(np.count_nonzero((bid < min_pts) | (bid > max_pts)))
        rep.zero_spread += int(np.count_nonzero(spread == 0))
        rep.saturated_spread += int(np.count_nonzero(flags & TF_SPREAD_SAT))
        rep.synthetic_ticks += int(np.count_nonzero(flags & TF_SYNTHETIC))

        lo, hi = int(bid.min()), int(bid.max())
        rep.min_bid_pts = lo if rep.ticks == 0 else min(rep.min_bid_pts, lo)
        rep.max_bid_pts = hi if rep.ticks == 0 else max(rep.max_bid_pts, hi)
        if rep.ticks == 0:
            rep.first_ts_us = int(ts[0])
        rep.last_ts_us = int(ts[-1])

        rep.ticks += int(hdr.count)
        prev_last_ts = int(ts[-1])

    return rep


def format_report(rep: VerifyReport, point_den: int = XAUUSD_POINT_DEN) -> str:
    import datetime as _dt

    def when(us: int) -> str:
        if us == 0:
            return "-"
        return _dt.datetime.fromtimestamp(us / 1e6, _dt.timezone.utc).strftime(
            "%Y-%m-%d %H:%M:%SZ"
        )

    size_gb = rep.ticks * TICK_DTYPE.itemsize / 1e9
    lines = [
        f"files            {rep.files}",
        f"ticks            {rep.ticks:,}   ({size_gb:.2f} GB)",
        f"span             {when(rep.first_ts_us)}  ..  {when(rep.last_ts_us)}",
        f"non-monotonic    {rep.non_monotonic:,}" + ("   <-- CORRUPT" if rep.non_monotonic else ""),
        f"out of range     {rep.out_of_range:,}"
        + ("   <-- check the decode scale" if rep.out_of_range else ""),
        f"zero spread      {rep.zero_spread:,}",
        f"clamped spread   {rep.saturated_spread:,}",
        f"gaps over thresh {rep.gaps:,}   (largest {rep.largest_gap_us / 1e6:,.0f} s"
        f" at {when(rep.largest_gap_at)})",
        f"bid range        {rep.min_bid_pts / point_den:,.3f} .. {rep.max_bid_pts / point_den:,.3f} USD",
        f"provenance       {'synthetic' if rep.synthetic_ticks == rep.ticks else 'real'}"
        + (
            f"   <-- MIXED: {rep.synthetic_ticks:,} of {rep.ticks:,} ticks are "
            "synthetic. Keep real and generated data in separate directories."
            if rep.mixed_provenance
            else ""
        ),
        f"verdict          {'OK' if rep.ok else 'FAILED'}",
    ]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    import argparse

    ap = argparse.ArgumentParser(description="Verify a binary tick store.")
    ap.add_argument("directory", help="directory holding <SYMBOL>-YYYY-MM.bin")
    ap.add_argument("--symbol", default="XAUUSD")
    ap.add_argument(
        "--gap-seconds",
        type=float,
        default=60.0,
        help="report quiet stretches longer than this (default 60; weekends always trip it)",
    )
    args = ap.parse_args(argv)

    rep = verify_store(args.directory, args.symbol, int(args.gap_seconds * 1e6))
    print(format_report(rep))
    return 0 if rep.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
