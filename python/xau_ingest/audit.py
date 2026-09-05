"""Audit a tick store deeply enough to trust it for backtesting.

`tickfmt --verify` answers "is this file well-formed". This answers the harder
question: "is this history COMPLETE, and does it mean what I think it means".

Those differ in a way that matters. A store with a silently missing week is
perfectly well-formed. A backtest over it produces a clean-looking result with
no indication that a chunk of the market never happened, and every statistic
downstream inherits the hole.

Four things are checked, in rising order of how easy they are to miss:

  coverage    ticks per month against the local median. A month far below its
              neighbours lost hours to failed fetches.
  gaps        every quiet stretch, cross-referenced against the raw fetch
              cache. The cache is the ground truth for "we asked and the feed
              said nothing": an hour stored as a zero-length file is a genuine
              market closure - a holiday, an early close - while an hour ABSENT
              from the cache was never successfully fetched, and only that is a
              hole in our data. Without this distinction every public holiday
              reads as missing data and the verdict becomes noise.
  prices      per-year range against what gold actually did. A decode-scale
              fault shows up here and nowhere else.
  costs       spread and M15 range by session, per year. The Phase 3 gate turns
              on spread-to-range, and if that ratio has drifted across the
              decade then a single cost model cannot represent all of it.

    python -m xau_ingest.audit data/ticks/real/XAUUSD --symbol XAUUSD
"""

from __future__ import annotations

import argparse
import datetime as dt
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from .dukascopy import cache_path
from .tickfmt import XAUUSD_POINT_DEN, TF_SYNTHETIC, open_ticks, store_files

M15_US = 900_000_000
HOUR_US = 3_600_000_000

# Gold's actual yearly range, for a decode sanity check. Deliberately loose:
# this is here to catch a wrong point scale (which would be off by 100x), not
# to second-guess the market.
PLAUSIBLE_YEAR_USD = (900.0, 5_000.0)


def session_of(hour: int) -> str:
    if 7 <= hour < 12:
        return "london"
    if 12 <= hour < 17:
        return "overlap"
    if 17 <= hour < 21:
        return "newyork"
    if 21 <= hour < 23:
        return "rollover"
    return "asia"


def market_open(ts: dt.datetime) -> bool:
    """Spot gold: Sunday 22:00 UTC through Friday 21:00 UTC."""
    wd = ts.weekday()  # Mon=0 .. Sun=6
    if wd == 5:
        return False
    if wd == 6:
        return ts.hour >= 22
    if wd == 4:
        return ts.hour < 21
    return True


def unfetched_hours(a_us: int, b_us: int, cache_dir: Path | None, symbol: str,
                    cap: int = 5000) -> tuple[int, int]:
    """(hours the market was open in this gap, how many were never fetched).

    Counting open hours alone is not enough: it flags Good Friday, Memorial Day
    and every other holiday as missing data. The cache settles it. An hour
    present there - even as a zero-length file - means the feed was asked and
    had nothing, which is a real closure. An hour absent was never successfully
    fetched, and that is the only kind of gap worth acting on.

    With no cache directory the second number is reported as -1, meaning
    "unknown", and holidays will show up as open hours.
    """
    a = dt.datetime.fromtimestamp(a_us / 1e6, dt.timezone.utc).replace(
        minute=0, second=0, microsecond=0
    )
    b = dt.datetime.fromtimestamp(b_us / 1e6, dt.timezone.utc)
    open_h = 0
    missing = 0
    t = a + dt.timedelta(hours=1)
    while t < b and open_h < cap:
        if market_open(t):
            open_h += 1
            if cache_dir is not None and not cache_path(cache_dir, symbol, t).exists():
                missing += 1
        t += dt.timedelta(hours=1)
    return open_h, (missing if cache_dir is not None else -1)


@dataclass
class Gap:
    start_us: int
    end_us: int
    open_hours: int
    unfetched: int = -1   # -1 when no cache was supplied

    @property
    def hours(self) -> float:
        return (self.end_us - self.start_us) / 3.6e9


@dataclass
class YearStats:
    ticks: int = 0
    min_px: float = 1e18
    max_px: float = -1e18
    spread_sum: float = 0.0
    spread_n: int = 0
    # session -> list of (spread median, range median) per month
    sess_spread: dict = field(default_factory=lambda: defaultdict(list))
    sess_range: dict = field(default_factory=lambda: defaultdict(list))


@dataclass
class AuditReport:
    files: int = 0
    ticks: int = 0
    synthetic_ticks: int = 0
    first_us: int = 0
    last_us: int = 0
    non_monotonic: int = 0
    months: dict = field(default_factory=dict)          # "YYYY-MM" -> tick count
    years: dict = field(default_factory=dict)           # int -> YearStats
    gaps: list = field(default_factory=list)            # suspicious ones only
    weekend_gaps: int = 0

    @property
    def ok(self) -> bool:
        return self.non_monotonic == 0 and not self.gaps and self.synthetic_ticks == 0


def audit_store(directory, symbol: str, min_open_hours: int = 2,
                cache_dir: Path | None = None) -> AuditReport:
    rep = AuditReport()
    paths = store_files(directory, symbol)
    if not paths:
        raise FileNotFoundError(f"no {symbol}-YYYY-MM.bin files in {directory}")

    prev_last: int | None = None

    for p in paths:
        hdr, arr = open_ticks(p)
        rep.files += 1
        if hdr.count == 0:
            continue

        ts = arr["ts_us"].astype(np.int64)
        bid = arr["bid_pts"].astype(np.int64)
        spread = arr["spread_pts"].astype(np.int64)
        flags = arr["flags"].astype(np.int64)

        key = p.stem.split("-", 1)[1]          # "YYYY-MM"
        year = int(key[:4])
        rep.months[key] = int(hdr.count)
        rep.ticks += int(hdr.count)
        rep.synthetic_ticks += int(np.count_nonzero(flags & TF_SYNTHETIC))
        rep.non_monotonic += int(np.count_nonzero(np.diff(ts) < 0))
        if rep.first_us == 0:
            rep.first_us = int(ts[0])
        rep.last_us = int(ts[-1])

        ys = rep.years.setdefault(year, YearStats())
        ys.ticks += int(hdr.count)
        ys.min_px = min(ys.min_px, float(bid.min()) / XAUUSD_POINT_DEN)
        ys.max_px = max(ys.max_px, float(bid.max()) / XAUUSD_POINT_DEN)
        ys.spread_sum += float(spread.sum())
        ys.spread_n += len(spread)

        # Gaps, including the seam between this file and the previous one.
        edges = np.flatnonzero(np.diff(ts) > 30 * 60_000_000)  # over 30 min
        starts = ts[edges]
        ends = ts[edges + 1]
        if prev_last is not None and ts[0] - prev_last > 30 * 60_000_000:
            starts = np.concatenate(([prev_last], starts))
            ends = np.concatenate(([ts[0]], ends))
        for gs, ge in zip(starts.tolist(), ends.tolist()):
            oh, miss = unfetched_hours(gs, ge, cache_dir, symbol)
            # With a cache, only hours we never fetched are holes. Without one,
            # fall back to open-hours and accept that holidays will show up.
            bad = (miss > 0) if cache_dir is not None else (oh >= min_open_hours)
            if bad:
                rep.gaps.append(Gap(gs, ge, oh, miss))
            else:
                rep.weekend_gaps += 1
        prev_last = int(ts[-1])

        # Per-session cost structure, one sample per month per session.
        hours = ((ts // HOUR_US) % 24).astype(np.int64)
        bar = ts // M15_US
        order = np.argsort(bar, kind="stable")
        b_sorted, p_sorted = bar[order], bid[order]
        cuts = np.flatnonzero(np.diff(b_sorted)) + 1
        bs = np.concatenate(([0], cuts))
        be = np.concatenate((cuts, [len(b_sorted)]))
        bar_id = b_sorted[bs]
        bar_rng = np.array([p_sorted[s:e].max() - p_sorted[s:e].min() for s, e in zip(bs, be)])
        bar_hour = ((bar_id * M15_US) // HOUR_US) % 24

        for name in ("asia", "london", "overlap", "newyork", "rollover"):
            mt = np.array([session_of(int(h)) == name for h in hours])
            mb = np.array([session_of(int(h)) == name for h in bar_hour])
            if mt.any():
                ys.sess_spread[name].append(float(np.median(spread[mt])) / XAUUSD_POINT_DEN)
            if mb.any() and (bar_rng[mb] > 0).any():
                ys.sess_range[name].append(
                    float(np.median(bar_rng[mb][bar_rng[mb] > 0])) / XAUUSD_POINT_DEN
                )

    return rep


def when(us: int) -> str:
    return dt.datetime.fromtimestamp(us / 1e6, dt.timezone.utc).strftime("%Y-%m-%d %H:%M")


def format_report(rep: AuditReport, expected_months: int = 0) -> str:
    out: list[str] = []
    gb = rep.ticks * 16 / 1e9
    out.append("=" * 78)
    out.append(f"tick store audit   {rep.files} files, {rep.ticks:,} ticks, {gb:.2f} GB")
    out.append(f"span               {when(rep.first_us)}  ..  {when(rep.last_us)}")
    out.append(
        f"provenance         {'REAL' if rep.synthetic_ticks == 0 else 'CONTAINS SYNTHETIC'}"
    )
    out.append(f"non-monotonic      {rep.non_monotonic:,}")
    out.append("=" * 78)

    # ---- coverage ----
    out.append("\ncoverage")
    if expected_months and rep.files < expected_months:
        out.append(f"  MISSING {expected_months - rep.files} of {expected_months} months")
    # Against a LOCAL median, not a decade-wide one. Tick density on this feed
    # more than doubled between 2015 and 2024 (25.7M ticks a year to 56.2M), so
    # a decade median sits far above any 2015 month and flags the whole year as
    # "lost hours" when every hour of it is present in the cache. The question
    # this check exists to answer is "is this month thin compared to the months
    # around it", which is a question about its neighbours.
    keys = sorted(rep.months)
    counts = np.array([rep.months[k] for k in keys], dtype=float)
    med = float(np.median(counts)) if len(counts) else 0.0

    WINDOW = 13  # ~a year, centred, so a month is judged against its own regime
    thin = []
    for i, k in enumerate(keys):
        lo = max(0, i - WINDOW // 2)
        local = counts[lo : lo + WINDOW]
        if len(local) == 0:
            continue
        lmed = float(np.median(local))
        if lmed > 0 and counts[i] < 0.55 * lmed:
            thin.append((k, rep.months[k], counts[i] / lmed * 100.0))

    out.append(f"  median month     {med:,.0f} ticks (decade), local window {WINDOW} months")
    if thin:
        out.append(f"  {len(thin)} month(s) well below their neighbours - likely lost hours:")
        for k, v, pct in thin[:10]:
            out.append(f"    {k}   {v:>10,}   ({pct:.0f}% of local median)")
    else:
        out.append("  every month within range of its neighbours")

    # ---- gaps ----
    out.append("\ngaps")
    out.append(f"  {rep.weekend_gaps:,} normal closes (weekends and holidays)")
    if rep.gaps:
        unfetched_total = sum(max(g.unfetched, 0) for g in rep.gaps)
        out.append(
            f"  {len(rep.gaps)} gap(s) with hours we never fetched "
            f"({unfetched_total:,} hours in total):"
        )
        for g in sorted(rep.gaps, key=lambda x: -x.unfetched)[:12]:
            miss = "unknown" if g.unfetched < 0 else f"{g.unfetched}h unfetched"
            out.append(
                f"    {when(g.start_us)} -> {when(g.end_us)}   "
                f"{g.hours:7.1f}h wall, {g.open_hours:4d}h open, {miss}"
            )
        out.append("  Re-run the ingest: failed hours are never cached, so it fills exactly these.")
    else:
        out.append("  none - every gap is a market close the feed confirmed")

    # ---- per year ----
    out.append("\nby year")
    out.append(
        f"  {'year':<6}{'ticks':>14}{'price range USD':>22}{'spread':>9}"
        f"{'overlap sp/rng':>16}"
    )
    for y in sorted(rep.years):
        s = rep.years[y]
        sp = s.spread_sum / max(s.spread_n, 1) / XAUUSD_POINT_DEN
        ov_sp = np.median(s.sess_spread["overlap"]) if s.sess_spread["overlap"] else float("nan")
        ov_rg = np.median(s.sess_range["overlap"]) if s.sess_range["overlap"] else float("nan")
        ratio = (ov_sp / ov_rg * 100.0) if ov_rg and ov_rg == ov_rg and ov_rg > 0 else float("nan")
        flag = ""
        if not (PLAUSIBLE_YEAR_USD[0] <= s.min_px <= PLAUSIBLE_YEAR_USD[1]):
            flag = "  <-- implausible, check the decode scale"
        out.append(
            f"  {y:<6}{s.ticks:>14,}{s.min_px:>11,.0f} ..{s.max_px:>8,.0f}"
            f"{sp:>9.3f}{ratio:>15.1f}%{flag}"
        )

    out.append("\nverdict            " + ("OK" if rep.ok else "PROBLEMS ABOVE"))
    if not rep.ok:
        out.append("                   Do not backtest on this until they are resolved.")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Audit a tick store for completeness and sanity.")
    ap.add_argument("directory")
    ap.add_argument("--symbol", default="XAUUSD")
    ap.add_argument(
        "--min-open-hours",
        type=int,
        default=2,
        help="a gap is suspicious once the market was open this many hours inside it",
    )
    ap.add_argument("--expect-months", type=int, default=0)
    ap.add_argument(
        "--cache",
        default=None,
        help="raw .bi5 cache. Without it every public holiday reads as missing data, "
        "because only the cache can tell a closure the feed confirmed from an hour "
        "we never managed to fetch.",
    )
    args = ap.parse_args(argv)

    cache = Path(args.cache) if args.cache else None
    if cache is None:
        print("NOTE: no --cache given; holidays will be reported as gaps.")
        print()
    rep = audit_store(Path(args.directory), args.symbol, args.min_open_hours, cache)
    print(format_report(rep, args.expect_months))
    return 0 if rep.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
