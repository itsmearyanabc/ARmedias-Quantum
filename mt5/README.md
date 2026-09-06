# MT5 bridge

Two halves: `xaubridge.dll` (built from `cpp/mt5`) holds every decision, and
`XauBridgeEA.mq5` reports market state and carries out what comes back.

The EA decides nothing. That is the point — the thing that trades live is then
the same code that was backtested. Reimplementing a strategy in MQL5 means
maintaining two versions of it, and the day they disagree is the day you learn
the backtest described a program you are not running.

## Install

1. Build the DLL:
   `cmake --build build/msvc-release --target xaumt5dll`
2. Copy `build/msvc-release/bin/xaubridge.dll` to `MQL5/Libraries/`
3. Copy `mt5/XauBridgeEA.mq5` to `MQL5/Experts/`, compile in MetaEditor
4. Tools → Options → Expert Advisors → **Allow DLL imports**

## Before it can trade

`InpDryRun` defaults to **true**: decisions are logged, no orders are sent.

The DLL's strategy hook is empty and returns NONE on every tick. That is
deliberate — nothing has cleared Phase 6, so there is nothing whose live
behaviour would be justified. The guards are live regardless: daily loss,
drawdown from peak, spread blowout, broken feed, position drift, kill file.

Order of operations before real money:

1. A strategy clears Phase 6 (DSR > 0.95, PBO < 0.3)
2. Wire it into the DLL's strategy hook
3. Demo, dry run off, six weeks minimum, ≥ 40 trades
4. Reconciliation clean, drift within tolerance
5. Only then, live

## The kill file

Set `InpKillFile` to a path. Creating that file halts everything within a
second — no debugger, no terminal restart, works while you are away from the
machine. Deleting it does NOT resume: that needs `xau_resume`, deliberately,
because a system that re-arms itself after hitting a loss limit does not have
a loss limit.

## ABI

`XAU_BRIDGE_ABI_VERSION` in `xau_bridge.h` must match `XAU_ABI_EXPECTED` in the
EA. MQL5 marshals structs by layout with no type checking: a field of the wrong
width does not fail, it silently shifts every field after it, and a `lots` value
read as a `price` places an order a thousand times too large. The EA checks the
version on init and refuses to run on a mismatch. Bump it on any struct or
signature change.
