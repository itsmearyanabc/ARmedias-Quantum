# Custom strategies, the lab, and live evolution

How to add many strategies (written by you or by an AI), rank them honestly,
trade the best one on MT5, and let live results promote or retire it.

```
 write a strategy ──► cpp/strategies/custom/NAME.cpp ──► build (or push: CI builds it)
                                                              │
                     conformance tests: deterministic, causal, well-formed
                                                              │
 lab  ──► every strategy, same data, same costs ──► leaderboard + champion (or NONE)
                                                              │
 MT5 EA, InpStrategy = @champion ──► trades the champion on DEMO, journals every trade
                                                              │
 lab --live journal ──► P(edge) per strategy ──► retire losers / promote proven ──┘
```

## 1. Add a strategy

One file per strategy in `cpp/strategies/custom/`, named after its class:
`MyBreakout.cpp` defines `class MyBreakout`. Then rebuild. Nothing else to
edit: the build finds the file, registers it under that name, and from then
on it is everywhere a strategy name is — the lab, `validate`, the terminal,
and the MT5 bridge (`InpStrategy = MyBreakout`).

`ExampleEmaTrend.cpp` in that folder is the template. The shape:

```cpp
#include "xau/custom.hpp"
#include "xau/indicators.hpp"

namespace {
class MyBreakout final : public xau::Strategy {
public:
    explicit MyBreakout(double lots) : lots_(lots) {}
    const char* name() const noexcept override { return "MyBreakout"; }
    xau::Decision on_bar(const xau::BarContext& c) override { /* ... */ }
private:
    double lots_;
};
}  // namespace

XAU_CUSTOM_STRATEGY(MyBreakout, H1, "One line: what it trades and why.")
```

**No C++ compiler on your PC?** Commit the file to the repository and push. CI
builds it, runs the conformance tests on it, and publishes `xaubridge.dll` and
`lab.exe` with your strategy inside, in the `ARmedias-Quantum` download of
that run (the desktop folder; see `desktop/START-HERE.txt`).

### The rules a strategy must follow

| rule | why |
|---|---|
| Decide only in `on_bar`, only from `c.history` (closed bars), `c.position`, `c.equity` | that is all a live strategy will have; the type offers nothing else |
| No `static` or global variables, no clock, no unseeded randomness | each backtest fold and each restart must get the same strategy |
| Prices are integer points: `c.spec.point_den` per 1.0 (1000 for gold) | the engine compares exact prices |
| Every entry sets a stop (`sl_dist_pts > 0`) | sizing by risk needs one, and the broker holds it if the PC dies |
| `lots_` into `Decision::lots`; 0 hands sizing to the risk layer | position size belongs to the caller |
| Use `xau::Atr`, `xau::Ema` (`xau/indicators.hpp`) or your own state in members | members reset with each fresh instance; statics do not |
| `name()` returns the class name | the lab, the journal and the EA find it by that name |

The conformance test (`cpp/tests/test_strategies.cpp`) runs **every**
registered strategy and fails if one is not deterministic, not causal (its
early trades change when later data is added), throws, or has a name that
does not match its file. CI publishes the DLL only after the tests pass, so a
strategy that fails it never reaches the `ARmedias-Quantum` download. Building
locally, run `ctest` before copying the DLL.

### A prompt for Claude or Codex

Paste this, then the strategy's description (a video transcript, a forum
post, your own notes):

> Write ONE C++20 file implementing this trading strategy for the
> ARmedias-Quantum engine. Copy the structure of
> `cpp/strategies/custom/ExampleEmaTrend.cpp` exactly:
> - `#include "xau/custom.hpp"` and `#include "xau/indicators.hpp"`; no other
>   project headers unless needed from `cpp/core/include/xau/`.
> - Everything in an anonymous namespace except the final line
>   `XAU_CUSTOM_STRATEGY(<ClassName>, <M1|M5|M15|M30|H1|H4|D1>, "<one line>")`.
> - Class `<ClassName> final : public xau::Strategy` with
>   `explicit <ClassName>(double lots)`, `name()` returning `"<ClassName>"`,
>   and `xau::Decision on_bar(const xau::BarContext& c)`.
> - Decide only from `c.history` (closed bars; `c.bar()` is the newest,
>   `c.ago(n)` older ones, `c.has(n)` before using `ago(n)`), `c.position`
>   (`is_open()`, `side`), `c.now_us` (UTC, microseconds).
> - Prices are `xau::Points` (integers, 1000 per 1.0 USD for gold); bars have
>   `open, high, low, close, open_time_us`.
> - Return `xau::Decision::hold()`, `xau::Decision::close("why")`, or
>   `xau::Decision::enter(xau::Side::Long|Short, stop_points, target_points, "why")`
>   with `stop_points > 0`; then set `d.lots = lots_;`.
> - No static or global variables, no randomness, no clock, no I/O, no
>   exceptions. Indicator state goes in members (`xau::Atr atr_{14};`,
>   `xau::Ema ema_{50};`, each `update()`d once per bar).
> - Must compile with `-Wall -Wextra -Wconversion -Wshadow -Werror`: cast
>   between `double` and `xau::Points` explicitly.
> - Do not tune parameters to past results; use the values the description
>   gives, or common defaults.
>
> Name the file `<ClassName>.cpp`.

## 2. Rank them: the lab

```
lab data/ticks/real/XAUUSD XAUUSD --out lab
```

Every strategy runs on its own bar length over the same ticks with the same
costs (spread from the ticks, slippage, latency, commission, swap). Each is
then scored on:

- profit factor, net, drawdown and Sharpe per trade;
- **CPCV**: the Sharpe over all 252 ways of choosing half the history, so a
  single lucky stretch cannot pass for a stable edge;
- **DSR** (deflated Sharpe): the probability its Sharpe beats the best that
  *N* zero-edge strategies would show by luck, where *N* is every strategy
  ever tried on this data;
- **PBO**: across all strategies, how often the in-sample winner falls below
  median out of sample.

A strategy **passes** when it has at least 100 trades, makes money after
costs, has DSR > 0.95, and still has profit factor ≥ 1.05 at doubled spread
and slippage. A **champion** is named only if one passes and PBO < 0.30.
Output, in `--out`:

| file | what |
|---|---|
| `leaderboard_XAUUSD.csv` | every strategy, every figure, PASS/FAIL and why |
| `champion_XAUUSD.txt` | the champion's name and bar length, or `name=` empty with the reason |
| `trials_XAUUSD.csv` | **the trial ledger** — every custom strategy version ever ranked |

**Expect NONE most of the time.** That is the lab working. With hundreds of
strategies on one history, the best of them always looks good; the ledger
makes sure that looking good *by the standard of hundreds of tries* is what
it takes. Editing a strategy and re-ranking it is a new trial — a changed
file has a new hash — so tuning until it passes raises its own bar. Deleting
a strategy does not delete its trial. Do not delete or hand-edit the ledger.
Pass `--prior-trials` if other configurations were tried on the same data
outside the lab (the default, 73, is this project's count before the lab).

Other flags: `--only A,B` (rank a subset; the ledger still counts them),
`--jobs N` (threads; default all cores), `--from/--to YYYY-MM-DD`,
`--champion-file PATH` (also write the champion where the EA reads it).

## 3. Trade the champion on MT5

Set `InpStrategy = @champion` on the EA (see `mt5/README.md`) and point the
lab at the EA's files folder:

```
lab data/ticks/real/XAUUSD XAUUSD --out lab ^
    --champion-file "%APPDATA%\MetaQuotes\Terminal\<id>\MQL5\Files\xau_champion.txt"
```

The EA reads the file at start and every minute. When it names a different
strategy, the EA switches **only when it is flat** (no position, no working
order) and **never while halted**. An empty champion means trade nothing. All
of the bridge's limits, the kill file and the demo-only gate apply to whatever
the champion is.

## 4. Let it evolve: live results

The EA appends one line per closed position to
`MQL5\Files\xau_trades_<symbol>_<login>.csv`: the strategy, its net result
after every cost, and the balance. Run

```
lab --live "...\MQL5\Files\xau_trades_XAUUSD_<login>.csv" --symbol XAUUSD --out lab ^
    --champion-file "...\MQL5\Files\xau_champion.txt"
```

For each strategy in the journal it keeps a Bayesian belief about its true
return per trade (`xau/bayes.hpp`): Normal returns, Normal-Inverse-Gamma
prior, updated exactly with every trade. The prior is **skeptical**: zero
edge, worth ten trades of evidence, so a strategy has to earn belief with
results. From that belief:

| figure | meaning |
|---|---|
| `P(edge)` | probability its true expected return per trade is above zero |
| `P(best)` | probability it is the best of those in the journal (posterior sampling) |
| **RETIRE** | ≥ 20 trades and P(edge) < 0.10 — probably a loser |
| **PROVEN** | ≥ 40 trades and P(edge) ≥ 0.90 |

Then: the PROVEN strategy most likely to be best becomes the champion; if
nothing is proven and the current champion is retired, the champion becomes
NONE; otherwise the champion is left alone. Flags: `--min-live`,
`--promote-above`, `--retire-below`.

To run it unattended, schedule it daily (Windows Task Scheduler, after the
market's daily close). The loop is then: the EA trades the champion and logs
each trade, `lab --live` updates the beliefs and the champion file, the EA
follows it when flat. A person is needed for two things only: adding
strategies, and resuming after a halt.

### Getting live evidence for several strategies

`lab --live` can only promote strategies with live trades. To test several
candidates at once, give each its **own demo account** (one MetaTrader
terminal per account), with `InpStrategy` set to the candidate's name. They
cannot share an account on the same symbol: the bridge treats any position
that is not its own as foreign, and halts. Each account writes its own
journal; pass them all, `lab --live A.csv --live B.csv ...`.

## What is verified, and what is not

- **Verified** (CI, Windows and Linux): registration of custom strategies,
  the conformance tests on every strategy, the Bayesian arithmetic
  (`test_bayes.cpp`, checked against independent values), and the lab end to
  end on synthetic ticks, where it must name no champion.
- **Not verified here:** the lab on real ticks (the data download is blocked
  from this environment), and the EA's journal and `@champion` code, which
  have not been compiled — MetaEditor runs only on Windows. Compile the EA,
  then run it with `InpDryRun = true` and read the Experts log first.
- **Not claimed:** that any strategy has an edge. The lab measures whether
  the evidence separates a strategy from luck; on this project's data so far,
  none has been separated.
