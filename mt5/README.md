# MT5 bridge

Two halves: `xaubridge.dll` (built from `cpp/mt5`) holds every decision, and
`XauBridgeEA.mq5` reports market state, carries out what comes back, and
reports what happened.

The EA decides nothing. The DLL runs the strategy through `xau::LiveSession`,
the same bar assembler and Strategy contract the backtest engine uses, and
places orders under the engine's own entry rules. `test_bridge.cpp` drives the
DLL's C ABI tick by tick through a simulated broker and requires the trades to
match `BacktestEngine` exactly: entry and exit times, prices and reasons, across
five strategies.

> **Read this first.** No strategy in this project has passed its validation
> gates (`docs/RESULTS.md`, `docs/RESEARCH-B.md`). The EA therefore refuses to
> run on a real account unless you set `InpAllowRealAccount`, and the plan's
> next step is a demo trial of six weeks or more. Running it on real money now
> means trading an idea the project's own tests rejected.

## Install

1. Build the DLL on Windows (MSVC, x64):
   `cmake --preset msvc-release && cmake --build --preset msvc-release --target xaumt5dll`
   — or download the `mt5-bridge` artifact from a green CI run. The DLL carries
   its own C runtime: no Visual C++ redistributable is needed.
2. Copy `xaubridge.dll` to `<data folder>\MQL5\Libraries\`
   (File → Open Data Folder in MetaTrader).
3. Copy `mt5/XauBridgeEA.mq5` to `<data folder>\MQL5\Experts\` and compile it in
   MetaEditor (F7). It must compile with **0 errors**.
4. Tools → Options → Expert Advisors → tick **Allow DLL imports**, and switch on
   **Algo Trading** in the toolbar.
5. Open a chart of the gold symbol on a **demo** account, drag the EA on, set
   the inputs below, tick *Allow DLL imports* in its dialog.

On start the EA checks the DLL's ABI version and the size of every shared
struct against its own, and refuses to run on any mismatch.

## Inputs

| input | default | meaning |
|---|---|---|
| `InpStrategy` | *(empty)* | Registry name, e.g. `TimeSeriesMomentum`, `DonchianTrend`, `AsiaDrift`, `LondonOpeningRange`. Empty = run the guards only, never enter. |
| `InpTimeframe` | D1 | The bar length the strategy was tested on: D1 for TimeSeriesMomentum and DonchianTrend, H1 for AsiaDrift, M15 for most others. |
| `InpFixedLots` | 0.01 | Trade exactly this size. Set 0 to size by risk instead. |
| `InpRiskPct` | 0 | With fixed lots 0: % of equity lost if the stop is hit (max 5). Needs a strategy that sets a stop. |
| `InpWarmupDays` | 400 | History fed in at start-up so long lookbacks are warm on the first live bar. |
| `InpMaxDailyLossPct` | 4 | Halt and flatten at this loss from the first equity of the broker day. |
| `InpMaxDrawdownPct` | 8 | Halt and flatten at this drawdown. |
| `InpInitialBalance` | 0 | Set to the challenge's starting balance to measure drawdown from it statically, as funded-trader firms do. 0 = trailing from the equity peak. |
| `InpMaxSpread` | 1.00 | No new entry while the spread is wider than this (price units). |
| `InpMaxLots` | 0.10 | Hard ceiling on gross open lots on the symbol; above it the bridge halts. |
| `InpMaxQuoteAgeSec` | 10 | No new entry on a quote older than this while the session is open. |
| `InpKillFile` | `XAU_STOP.txt` | Create this file (in `MQL5\Files`) to halt everything. |
| `InpMagic` | 990101 | Identifies this EA's positions and orders. Each chart needs its own: a second chart with the same symbol and magic is refused at start. |
| `InpDeviationPts` | 30 | Largest slippage accepted on a market order, in broker points. |
| `InpDryRun` | false | Log orders instead of sending them. |
| `InpAllowRealAccount` | false | The EA refuses a non-demo account unless this is set. |
| `InpServerDst` | US | The broker server's daylight-saving rule: US (most GMT+2/+3 brokers), EU, or none. Used to convert warm-up history to UTC with the offset in force at each bar, not today's. |
| `InpTesterGmtOffset` | 2 | Strategy Tester only: the server's winter UTC offset in hours (the tester has no UTC clock). |

## What it does, tick by tick

1. Reports the quote, account equity, this EA's position and every other
   position on the symbol (*foreign*), the broker trading day and whether the
   session is open. Times are converted to UTC once, here. "Now" is the
   broker's clock — its latest quote time carried forward by the PC's
   monotonic counter — so a PC clock that is off does not make every quote
   look stale.
2. The DLL runs its guards, then the strategy on the closed bar, and returns
   one of: nothing, BUY/SELL (size, stop, target), CLOSE, or FLATTEN_AND_HALT.
3. The EA executes it. **Stops and targets ride on the order**, so the broker
   holds the protection even if MetaTrader, the EA or the DLL goes away. After
   a fill with slippage they are moved onto the fill price, as the backtest
   measures them; if the broker refuses the move, the ones sent with the order
   stand.
4. The EA reports the result. Until it does, the DLL sends nothing new: one
   decision can never become two orders. A failed **entry** is not retried —
   its bar has passed. A failed **close** is retried each second, five attempts
   in all, then the bridge halts and flattens: an exit is never dropped.

A one-second timer re-checks the kill file and any order whose result never
came back, so both work with the market closed. The chart shows a status panel
and **HALT** / **RESUME** buttons.

## Safety controls

| condition | effect |
|---|---|
| daily loss limit reached | **halt + flatten** |
| drawdown limit reached (peak, or initial balance) | **halt + flatten** |
| kill file present (checked every second) | **halt + flatten** |
| kill file's folder does not exist (the switch could never fire) | **starts halted**; resume refused until it exists |
| HALT button | **halt + flatten** |
| zero or inverted quote | **halt + flatten** |
| a position on the symbol that is not this EA's | **halt + flatten** |
| more of our positions than allowed, or gross lots over the ceiling | **halt + flatten** |
| order result never reported (30 s) | **halt + flatten** |
| a placed order of ours still working after 30 s | **halt + flatten** |
| a close that does not get through in 5 attempts | **halt + flatten** |
| internal error in the DLL | **halt + flatten** (fails closed) |
| spread wider than the limit | no new entry; closing still allowed |
| quote older than the limit while the session is open | no new entry |
| an order of ours still working | no new entry |
| a stop inside the spread, or closer to bid/ask than the broker's stops level | that entry refused |
| risk sizing with no tick value from the broker | that entry refused |

A halt **persists**: it is written to
`MQL5\Files\xau_state_<symbol>_<login>_<magic>.txt` together with the day's
starting equity and the equity peak, so restarting MetaTrader or re-attaching
the EA neither clears a halt nor hands back a fresh daily allowance. A write
that fails (disk full, file locked) is retried every second, and the status
panel says `WARNING state NOT persisted` until it lands.

An unreadable state file starts the EA halted and is left untouched. RESUME
reads it again: refused while it is still unreadable; repaired, its anchors are
used; deleted, the EA starts with fresh anchors — deleting it is the operator's
decision to start the day's allowance over.

Only a person resumes: the **RESUME** button asks for confirmation, and is
refused while the kill file exists. Deleting the kill file alone does not
resume.

FLATTEN closes **every** position on the symbol, not only this EA's, and
deletes this EA's working orders. Do not trade the same symbol by hand on the
account the EA runs on.

Risk sizing (`InpFixedLots = 0`) uses the broker's tick value, which is in the
account's currency, so a EUR or GBP account risks the percentage it was told.

In the Strategy Tester nothing persists: each pass starts with no state file,
so one pass's halt or equity peak cannot decide the next.

## What is verified, and what is not

- **Verified** (in CI, on Windows and Linux): the DLL's guards, persistence,
  order lifecycle (rejected, placed and stuck orders, retried closes), warm-up,
  and trade-for-trade parity with the backtest engine, with stops re-anchored
  on the fill — `cpp/tests/test_bridge.cpp`. On Windows CI also loads the
  shipped `xaubridge.dll`, checks its ABI and struct sizes, and fails if it
  depends on the Visual C++ runtime.
- **Not verified here:** `XauBridgeEA.mq5` itself. MetaEditor runs only on
  Windows and is not part of CI, so the EA has not been compiled or run by the
  automated checks. Compile it, then run it on a demo account with
  `InpDryRun = true` first and read the Experts log before letting it trade.

Order of operations before real money (`docs/PLAN.md` §14):

1. A strategy passes stage 1 and stage 2 of `docs/RESEARCH-B.md`
2. Demo account, dry run off, six weeks minimum, at least 40 trades
3. Reconciliation clean, live-vs-backtest drift within tolerance
4. Only then a real account, by setting `InpAllowRealAccount`

## ABI

`XAU_BRIDGE_ABI_VERSION` in `xau_bridge.h` must match `XAU_ABI_EXPECTED` in the
EA, and the EA compares every struct's size with the DLL's. MQL5 marshals
structs by layout with no type checking, and packs at 1 byte where C aligns a
double to 8 — so every shared struct orders its fields 8-byte, 4-byte, then
bytes, with a size that is a multiple of 8, and `bridge.cpp` pins every offset
with `static_assert`. Bump the version on any struct or signature change.
