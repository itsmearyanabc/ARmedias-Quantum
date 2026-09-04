"""Round-trip and validation tests for the tick format and Dukascopy decoder.

The Dukascopy tests synthesise .bi5 bytes rather than hitting the network, so
the decoder — including the ask/bid ordering guard and the point-scale guard —
is covered offline and in CI.

    python -m pytest python/tests -q        (or)        python python/tests/test_tickfmt.py
"""

from __future__ import annotations

import datetime as dt
import lzma
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from xau_ingest import dukascopy as dk  # noqa: E402
from xau_ingest import tickfmt as tf  # noqa: E402

HOUR = dt.datetime(2024, 3, 14, 13, tzinfo=dt.timezone.utc)
HOUR_US = int(HOUR.timestamp()) * 1_000_000


def make_ticks(n=1000, ts0=HOUR_US, bid0=2_650_000, step_us=1_000_000):
    ts = ts0 + np.arange(n, dtype=np.int64) * step_us
    bid = bid0 + np.arange(n, dtype=np.int64)
    spread = np.full(n, 250, dtype=np.int64)
    return tf.pack_ticks(ts, bid, spread)


# ---------------------------------------------------------------------------
# layout — these pin the ABI shared with cpp/core/include/xau/types.hpp
# ---------------------------------------------------------------------------


def test_layout_matches_cpp_struct():
    assert tf.TICK_DTYPE.itemsize == 16
    assert tf.HEADER_BYTES == 64
    offsets = {k: v[1] for k, v in tf.TICK_DTYPE.fields.items()}
    assert offsets == {"ts_us": 0, "bid_pts": 8, "spread_pts": 12, "flags": 14}


def test_prices_to_points_is_exact_and_deterministic():
    got = tf.prices_to_points(np.array([2650.0, 2650.456, 2650.999, 0.0]))
    assert got.tolist() == [2_650_000, 2_650_456, 2_650_999, 0]

    # Exact ties round half-to-even (numpy's rule). Pinned not because the rule
    # matters — half a point is 0.0005 USD — but because prices become integers
    # here and every downstream barrier comparison is exact.
    ties = tf.prices_to_points(np.array([2650.5, 2651.5, -2650.5]), point_den=1)
    assert ties.tolist() == [2650, 2652, -2650]

    # Idempotent: converting the same input twice cannot disagree.
    xs = np.array([2650.4564, 2650.4565, 2703.3335])
    assert tf.prices_to_points(xs).tolist() == tf.prices_to_points(xs).tolist()


# ---------------------------------------------------------------------------
# writer / reader
# ---------------------------------------------------------------------------


def test_roundtrip(tmp_path):
    p = tmp_path / "XAUUSD-2024-03.bin"
    ticks = make_ticks(500)
    with tf.TickWriter(p, "XAUUSD") as w:
        w.write(ticks)
        assert w.count == 500

    hdr, arr = tf.open_ticks(p)
    assert hdr.count == 500
    assert hdr.symbol == "XAUUSD"
    assert hdr.point_den == tf.XAUUSD_POINT_DEN
    assert hdr.first_ts_us == int(ticks["ts_us"][0])
    assert hdr.last_ts_us == int(ticks["ts_us"][-1])
    assert np.array_equal(arr["bid_pts"], ticks["bid_pts"])
    assert np.array_equal(arr["ts_us"], ticks["ts_us"])
    # File is exactly header + payload, no slack.
    assert p.stat().st_size == tf.HEADER_BYTES + 500 * 16


def test_multiple_chunks_concatenate(tmp_path):
    p = tmp_path / "XAUUSD-2024-03.bin"
    a = make_ticks(100, ts0=HOUR_US)
    b = make_ticks(100, ts0=HOUR_US + 200_000_000, bid0=2_660_000)
    with tf.TickWriter(p, "XAUUSD") as w:
        w.write(a)
        w.write(b)
    hdr, arr = tf.open_ticks(p)
    assert hdr.count == 200
    assert arr["bid_pts"][100] == 2_660_000


def test_empty_file_is_valid(tmp_path):
    p = tmp_path / "XAUUSD-2024-04.bin"
    with tf.TickWriter(p, "XAUUSD"):
        pass
    hdr, arr = tf.open_ticks(p)
    assert hdr.count == 0
    assert len(arr) == 0


def test_writer_rejects_backwards_time_within_a_chunk(tmp_path):
    ticks = make_ticks(10)
    ticks["ts_us"][5] = ticks["ts_us"][0] - 1
    with pytest.raises(ValueError, match="backwards"):
        with tf.TickWriter(tmp_path / "x.bin", "XAUUSD") as w:
            w.write(ticks)


def test_writer_rejects_backwards_time_across_chunks(tmp_path):
    a = make_ticks(10, ts0=HOUR_US + 10_000_000)
    b = make_ticks(10, ts0=HOUR_US)  # earlier — an out-of-order hour
    with pytest.raises(ValueError, match="before the previous chunk"):
        with tf.TickWriter(tmp_path / "x.bin", "XAUUSD") as w:
            w.write(a)
            w.write(b)


def test_spread_clamping_sets_the_saturation_flag():
    ts = np.array([HOUR_US], dtype=np.int64)
    bid = np.array([2_650_000], dtype=np.int64)
    arr = tf.pack_ticks(ts, bid, np.array([tf.MAX_SPREAD_PTS + 5000], dtype=np.int64))
    assert arr["spread_pts"][0] == tf.MAX_SPREAD_PTS
    assert arr["flags"][0] & tf.TF_SPREAD_SAT

    ok = tf.pack_ticks(ts, bid, np.array([250], dtype=np.int64))
    assert not (ok["flags"][0] & tf.TF_SPREAD_SAT)


def test_negative_spread_is_rejected():
    ts = np.array([HOUR_US], dtype=np.int64)
    bid = np.array([2_650_000], dtype=np.int64)
    with pytest.raises(ValueError, match="swapped"):
        tf.pack_ticks(ts, bid, np.array([-1], dtype=np.int64))


# ---------------------------------------------------------------------------
# header validation
# ---------------------------------------------------------------------------


def test_rejects_bad_magic(tmp_path):
    p = tmp_path / "XAUUSD-2024-03.bin"
    with tf.TickWriter(p, "XAUUSD") as w:
        w.write(make_ticks(10))
    raw = bytearray(p.read_bytes())
    raw[0] = ord("Z")
    p.write_bytes(raw)
    with pytest.raises(tf.FormatError, match="magic"):
        tf.read_header(p)


def test_rejects_truncated_payload(tmp_path):
    p = tmp_path / "XAUUSD-2024-03.bin"
    with tf.TickWriter(p, "XAUUSD") as w:
        w.write(make_ticks(10))
    raw = p.read_bytes()
    p.write_bytes(raw[:-16])  # header still claims 10 ticks, 9 remain
    with pytest.raises(tf.FormatError, match="header says"):
        tf.read_header(p)


def test_rejects_partial_tick(tmp_path):
    p = tmp_path / "XAUUSD-2024-03.bin"
    with tf.TickWriter(p, "XAUUSD") as w:
        w.write(make_ticks(10))
    p.write_bytes(p.read_bytes()[:-5])
    with pytest.raises(tf.FormatError, match="whole number of ticks"):
        tf.read_header(p)


# ---------------------------------------------------------------------------
# store discovery and verification
# ---------------------------------------------------------------------------


def test_store_files_filters_and_orders(tmp_path):
    for name in [
        "XAUUSD-2024-02.bin",
        "XAUUSD-2024-01.bin",
        "XAUUSD-2024-10.bin",
        "XAGUSD-2024-01.bin",  # other symbol
        "XAUUSDm-2024-01.bin",  # longer symbol, matching prefix
        "XAUUSD-2024-13.bin",  # impossible month
        "notes.txt",
    ]:
        (tmp_path / name).write_bytes(b"")
    got = [p.name for p in tf.store_files(tmp_path, "XAUUSD")]
    assert got == ["XAUUSD-2024-01.bin", "XAUUSD-2024-02.bin", "XAUUSD-2024-10.bin"]


def test_verify_clean_store(tmp_path):
    with tf.TickWriter(tmp_path / "XAUUSD-2024-03.bin", "XAUUSD") as w:
        w.write(make_ticks(1000))
    rep = tf.verify_store(tmp_path, "XAUUSD", gap_threshold_us=60_000_000)
    assert rep.ok
    assert rep.ticks == 1000
    assert rep.gaps == 0
    assert rep.out_of_range == 0


def test_verify_flags_outliers_and_gaps(tmp_path):
    ticks = make_ticks(200)
    ticks["bid_pts"][50] = 99  # decode-scale accident
    ticks["ts_us"][100:] += 300_000_000  # five-minute hole
    with tf.TickWriter(tmp_path / "XAUUSD-2024-03.bin", "XAUUSD") as w:
        w.write(ticks)

    rep = tf.verify_store(tmp_path, "XAUUSD", gap_threshold_us=60_000_000)
    assert not rep.ok
    assert rep.out_of_range == 1
    assert rep.gaps == 1
    assert rep.largest_gap_us >= 300_000_000


def test_verify_spots_gap_across_month_boundary(tmp_path):
    with tf.TickWriter(tmp_path / "XAUUSD-2024-01.bin", "XAUUSD") as w:
        w.write(make_ticks(10, ts0=HOUR_US))
    with tf.TickWriter(tmp_path / "XAUUSD-2024-02.bin", "XAUUSD") as w:
        w.write(make_ticks(10, ts0=HOUR_US + 10_000_000_000))
    rep = tf.verify_store(tmp_path, "XAUUSD", gap_threshold_us=60_000_000)
    assert rep.files == 2
    assert rep.gaps == 1


# ---------------------------------------------------------------------------
# Dukascopy decoder — exercised offline against synthesised .bi5 bytes
# ---------------------------------------------------------------------------


def make_bi5(records) -> bytes:
    """records: (ms_after_hour, ask_raw, bid_raw) triples in instrument points."""
    body = b"".join(dk.RECORD.pack(ms, ask, bid, 1.0, 1.0) for ms, ask, bid in records)
    return lzma.compress(body, format=lzma.FORMAT_ALONE)


def test_decode_hour_happy_path():
    # 2650.450 / 2650.700 at XAUUSD's 1000-point scale.
    recs = [(0, 2_650_700, 2_650_450), (1500, 2_650_900, 2_650_650)]
    arr = dk.decode_hour(make_bi5(recs), "XAUUSD", HOUR)

    assert len(arr) == 2
    assert arr["ts_us"][0] == HOUR_US
    assert arr["ts_us"][1] == HOUR_US + 1_500_000
    assert arr["bid_pts"][0] == 2_650_450
    assert arr["spread_pts"][0] == 250  # $0.25
    assert arr["flags"][0] & tf.TF_BID


def test_decode_hour_empty_input():
    assert len(dk.decode_hour(b"", "XAUUSD", HOUR)) == 0
    assert len(dk.decode_hour(make_bi5([]), "XAUUSD", HOUR)) == 0


def test_decode_hour_sorts_unordered_records():
    recs = [(5000, 2_650_700, 2_650_450), (1000, 2_650_500, 2_650_250)]
    arr = dk.decode_hour(make_bi5(recs), "XAUUSD", HOUR)
    assert arr["ts_us"].tolist() == [HOUR_US + 1_000_000, HOUR_US + 5_000_000]
    assert arr["bid_pts"][0] == 2_650_250


def test_decode_hour_rejects_reversed_ask_bid_order():
    # Every record has bid above ask: the field order is not what we assume.
    recs = [(i * 100, 2_650_000, 2_650_500) for i in range(20)]
    with pytest.raises(dk.DecodeError, match="field order is reversed"):
        dk.decode_hour(make_bi5(recs), "XAUUSD", HOUR)


def test_decode_hour_rejects_implausible_scale():
    # Raw values as if the instrument scaled by 100,000 instead of 1,000:
    # decodes to ~$26.50, far under gold's plausible floor.
    recs = [(i * 100, 26_507, 26_504) for i in range(20)]
    with pytest.raises(dk.DecodeError, match="plausible band"):
        dk.decode_hour(make_bi5(recs), "XAUUSD", HOUR)


def test_decode_hour_rejects_ragged_payload():
    body = b"\x00" * 25  # not a multiple of 20
    raw = lzma.compress(body, format=lzma.FORMAT_ALONE)
    with pytest.raises(dk.DecodeError, match="whole number"):
        dk.decode_hour(raw, "XAUUSD", HOUR)


def test_hour_url_uses_zero_indexed_month():
    # January is 00. Getting this wrong silently ingests the wrong month.
    url = dk.hour_url("XAUUSD", dt.datetime(2024, 1, 5, 9, tzinfo=dt.timezone.utc))
    assert url.endswith("/XAUUSD/2024/00/05/09h_ticks.bi5")
    url = dk.hour_url("XAUUSD", dt.datetime(2024, 12, 31, 23, tzinfo=dt.timezone.utc))
    assert url.endswith("/XAUUSD/2024/11/31/23h_ticks.bi5")


# ---------------------------------------------------------------------------
# generator
# ---------------------------------------------------------------------------


def test_synth_is_reproducible_and_verifies(tmp_path):
    from xau_ingest import synth

    a = synth.generate(tmp_path / "a", months=1, seed=7, verbose=False)
    b = synth.generate(tmp_path / "b", months=1, seed=7, verbose=False)
    assert a == b
    assert sum(a.values()) > 0

    rep = tf.verify_store(tmp_path / "a", "XAUUSD")
    assert rep.ok
    assert rep.non_monotonic == 0
    lo = rep.min_bid_pts / tf.XAUUSD_POINT_DEN
    hi = rep.max_bid_pts / tf.XAUUSD_POINT_DEN
    assert 500 < lo < hi < 10_000


def test_synth_respects_the_weekend_close():
    from xau_ingest import synth

    assert not synth.market_open(dt.datetime(2024, 3, 16, 12, tzinfo=dt.timezone.utc))  # Sat
    assert not synth.market_open(dt.datetime(2024, 3, 17, 12, tzinfo=dt.timezone.utc))  # Sun am
    assert synth.market_open(dt.datetime(2024, 3, 17, 22, tzinfo=dt.timezone.utc))  # Sun open
    assert synth.market_open(dt.datetime(2024, 3, 15, 20, tzinfo=dt.timezone.utc))  # Fri
    assert not synth.market_open(dt.datetime(2024, 3, 15, 21, tzinfo=dt.timezone.utc))  # Fri close


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-q"]))
