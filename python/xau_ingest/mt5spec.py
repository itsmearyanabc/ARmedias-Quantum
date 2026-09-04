"""MT5 symbol spec discovery and server clock offset.

Produces config/symbol_spec.json — the committed ground truth about the
execution venue. Every number in the backtest cost model traces back to this
file, so it is generated once, checked in, and re-generated deliberately (never
read live during a backtest, or results stop being reproducible).

Two things this exists to pin down:

1. **The symbol's real name and contract terms.** Brokers name gold
   XAUUSD, GOLD, XAUUSD.r, XAUUSDm, XAUUSD.s ... and the contract size, minimum
   stop distance and filling modes differ between them. Guessing produces a
   backtest full of trades that could not have been placed.

2. **The server clock offset.** MT5 server time is typically GMT+2/+3 and it
   shifts with US DST. Every session feature depends on getting this right, and
   a strategy with session logic silently breaks twice a year if you assume it
   is constant. We record it with a timestamp so drift is detectable.

Windows only, and requires the MT5 terminal to be installed, running and logged
in:  pip install MetaTrader5
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import statistics
import time
from pathlib import Path

# Symbol names that plausibly mean spot gold against the dollar.
GOLD_PATTERN = re.compile(r"^(XAU.?USD|GOLD)", re.IGNORECASE)

# Fields the cost model and the risk layer actually consume. Recorded
# separately from the full dump so a reviewer can see the load-bearing values
# without reading 60 fields.
CRITICAL_FIELDS = [
    "name",
    "digits",
    "point",
    "trade_contract_size",
    "trade_tick_size",
    "trade_tick_value",
    "trade_tick_value_profit",
    "trade_tick_value_loss",
    "volume_min",
    "volume_max",
    "volume_step",
    "trade_stops_level",
    "trade_freeze_level",
    "trade_mode",
    "filling_mode",
    "expiration_mode",
    "spread",
    "spread_float",
    "swap_mode",
    "swap_long",
    "swap_short",
    "swap_rollover3days",
    "margin_initial",
    "currency_base",
    "currency_profit",
    "currency_margin",
]


def _jsonable(v):
    if isinstance(v, (str, int, float, bool)) or v is None:
        return v
    if isinstance(v, (list, tuple)):
        return [_jsonable(x) for x in v]
    if isinstance(v, dict):
        return {str(k): _jsonable(x) for k, x in v.items()}
    return str(v)


def find_gold_symbols(mt5) -> list[str]:
    symbols = mt5.symbols_get()
    if symbols is None:
        raise RuntimeError(f"symbols_get failed: {mt5.last_error()}")
    return sorted(s.name for s in symbols if GOLD_PATTERN.match(s.name))


def server_utc_offset(mt5, symbol: str, samples: int = 5, pause: float = 0.4) -> dict:
    """Estimate server-clock minus UTC, in hours.

    Sampled rather than read once: a single quote from an illiquid moment can be
    seconds-to-minutes stale, which would round to the wrong hour. Disagreement
    between samples is reported rather than averaged away.
    """
    offsets: list[float] = []
    lags: list[float] = []

    for _ in range(samples):
        tick = mt5.symbol_info_tick(symbol)
        now = time.time()
        if tick is None:
            time.sleep(pause)
            continue
        raw = tick.time - now
        offsets.append(round(raw / 3600.0))
        lags.append(raw / 3600.0 - round(raw / 3600.0))
        time.sleep(pause)

    if not offsets:
        return {"error": "no ticks returned; is the market open and the symbol selected?"}

    agreed = len(set(offsets)) == 1
    return {
        "offset_hours": int(statistics.mode(offsets)),
        "samples": offsets,
        "samples_agree": agreed,
        "residual_hours": round(statistics.median(lags), 4),
        "measured_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "note": (
            "Server time shifts with US DST. Re-measure at least at each DST "
            "transition; the live engine re-checks daily."
        ),
        "warning": None if agreed else "samples disagreed — quotes may be stale, re-run",
    }


def build_spec(mt5, symbol: str, include_account_id: bool) -> dict:
    if not mt5.symbol_select(symbol, True):
        raise RuntimeError(f"symbol_select({symbol}) failed: {mt5.last_error()}")

    info = mt5.symbol_info(symbol)
    if info is None:
        raise RuntimeError(f"symbol_info({symbol}) failed: {mt5.last_error()}")
    full = {k: _jsonable(v) for k, v in info._asdict().items()}

    term = mt5.terminal_info()
    acct = mt5.account_info()

    terminal = {}
    if term is not None:
        t = term._asdict()
        terminal = {
            k: _jsonable(t.get(k))
            for k in ("company", "name", "build", "connected", "trade_allowed", "dlls_allowed")
        }

    account = {}
    if acct is not None:
        a = acct._asdict()
        # Deliberately excludes login and holder name: this file is committed.
        account = {
            k: _jsonable(a.get(k))
            for k in (
                "server",
                "currency",
                "leverage",
                "trade_mode",
                "margin_mode",
                "limit_orders",
                "margin_so_call",
                "margin_so_so",
            )
        }
        if include_account_id:
            account["login"] = a.get("login")

    tick = mt5.symbol_info_tick(symbol)
    live = {}
    if tick is not None:
        live = {
            "bid": tick.bid,
            "ask": tick.ask,
            "spread_price": round(tick.ask - tick.bid, 6),
            "spread_points": round((tick.ask - tick.bid) / info.point) if info.point else None,
            "server_time": tick.time,
        }

    return {
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "symbol": symbol,
        "critical": {k: full.get(k) for k in CRITICAL_FIELDS},
        "server_clock": server_utc_offset(mt5, symbol),
        "snapshot_quote": live,
        "terminal": terminal,
        "account": account,
        "symbol_info_full": full,
    }


def summarise(spec: dict) -> str:
    c = spec["critical"]
    clock = spec.get("server_clock", {})
    lines = [
        f"symbol              {c.get('name')}",
        f"digits / point      {c.get('digits')} / {c.get('point')}",
        f"contract size       {c.get('trade_contract_size')} (oz)",
        f"volume min/step/max {c.get('volume_min')} / {c.get('volume_step')} / {c.get('volume_max')}",
        f"min stop distance   {c.get('trade_stops_level')} points",
        f"freeze level        {c.get('trade_freeze_level')} points",
        f"filling mode mask   {c.get('filling_mode')}",
        f"swap long / short   {c.get('swap_long')} / {c.get('swap_short')}  (mode {c.get('swap_mode')})",
        f"triple swap day     {c.get('swap_rollover3days')}",
        f"current spread      {spec.get('snapshot_quote', {}).get('spread_points')} points",
        f"server offset       UTC{clock.get('offset_hours', '?'):+} h"
        + ("" if clock.get("samples_agree", True) else "   <-- samples disagreed"),
        f"DLL imports allowed {spec.get('terminal', {}).get('dlls_allowed')}",
        f"algo trading        {spec.get('terminal', {}).get('trade_allowed')}",
    ]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Dump the MT5 gold symbol spec and clock offset.")
    ap.add_argument("--symbol", default=None, help="force a symbol instead of auto-detecting")
    ap.add_argument("--out", default="config/symbol_spec.json")
    ap.add_argument("--list", action="store_true", help="list candidate gold symbols and exit")
    ap.add_argument(
        "--include-account-id",
        action="store_true",
        help="include the account login (off by default — this file is committed)",
    )
    args = ap.parse_args(argv)

    try:
        import MetaTrader5 as mt5
    except ImportError:
        print("this needs `pip install MetaTrader5` on 64-bit Windows Python")
        return 2

    if not mt5.initialize():
        print(f"mt5.initialize() failed: {mt5.last_error()}")
        print("Is the MT5 terminal installed, running and logged in?")
        return 1

    try:
        candidates = find_gold_symbols(mt5)
        if not candidates:
            print("no gold-looking symbols found. Check the Market Watch symbol list.")
            return 1

        if args.list:
            print("candidate gold symbols:")
            for name in candidates:
                info = mt5.symbol_info(name)
                size = getattr(info, "trade_contract_size", "?") if info else "?"
                digits = getattr(info, "digits", "?") if info else "?"
                print(f"  {name:<16} contract={size}  digits={digits}")
            return 0

        symbol = args.symbol or candidates[0]
        if len(candidates) > 1 and not args.symbol:
            print(f"note: {len(candidates)} gold symbols found {candidates}")
            print(f"      using {symbol}; pass --symbol to choose another\n")

        spec = build_spec(mt5, symbol, args.include_account_id)

        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(spec, indent=2), encoding="utf-8")

        print(summarise(spec))
        print(f"\nwritten to {out}")
        if not spec.get("terminal", {}).get("dlls_allowed"):
            print(
                "\nDLL imports are disabled in this terminal. The Phase 7 bridge needs them\n"
                "(Tools -> Options -> Expert Advisors -> Allow DLL imports), and some prop\n"
                "firms forbid them entirely — confirm before building the bridge."
            )
        return 0
    finally:
        mt5.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
