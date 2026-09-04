# XAUUSD Algorithmic Trading Terminal — Build Plan

**Instrument:** XAUUSD (spot gold CFD) on MetaTrader 5
**Horizon:** Intraday, M15–H1 entries, flat overnight and over weekends
**Account context:** Prop firm challenge (FTMO-style rules)
**Implementation:** C++20 core + Dear ImGui/ImPlot terminal, Python for data ingest and model training
**Status:** Phase 0 and Phase 1 gates met. Phase 2 (terminal) partially built; Phase 3 (rule baselines) built but its gate **not evaluated** — that needs real tick history, not the synthetic null. Green on MSVC and GCC in CI. See [README](../README.md) for the live checklist.

---

## 0. The honest frame — read this before anything else

The stated goal was "highest accuracy possible." That is the wrong optimisation target, and chasing it is the single most common way retail algo projects die. Three reasons:

1. **Accuracy is trivially gameable.** Set a tiny take-profit and a huge stop and you get a 92% win rate with a catastrophic tail. The metric goes up; the account goes to zero.
2. **What pays is expectancy after costs:** `E = p·W − (1−p)·L`. A 42% win rate at 3R beats a 75% win rate at 0.3R, comfortably.
3. **For a prop challenge the objective is not even Sharpe.** It is `P(reach +8% before touching −5% daily or −10% total)`. That is a *first-passage probability*, maximised by a very different risk profile than profit maximisation. See §1.

So the actual target this project optimises for:

> **Out-of-sample, cost-inclusive, statistically-deflated edge, sized to survive the drawdown constraints.**

### Realistic expectations, stated up front

| Metric | Genuinely good intraday gold model | Red flag |
|---|---|---|
| Directional accuracy (vol-scaled triple-barrier labels) | 53–56% | > 65% |
| Profit factor after real costs | 1.15–1.35 | > 2.5 |
| Deflated Sharpe (out-of-sample) | 0.6–1.2 | > 2.5 |
| Fraction of tested ideas that survive validation | ~5–10% | "the first one worked" |

If a backtest reports numbers in the right-hand column, assume leakage until §8's checklist proves otherwise. It is almost always leakage.

**Where "accuracy" *does* legitimately matter:** in the meta-labeling stage (§6), where a classifier answers one narrow question — *given the entry rule just fired, will this specific trade hit TP before SL?* There, accuracy maps directly to money. That is the correct place to push it hard, and this plan is built around that idea.

---

## 1. Prop-firm rules as a constraint problem

Typical FTMO-style structure (confirm exact numbers against your firm's terms before Phase 7):

| Rule | Typical value | Engineering implication |
|---|---|---|
| Profit target | +8% Phase 1, +5% Phase 2 | Absorbing barrier (win) |
| Max daily loss | −5% of initial balance, **equity-based**, resets 00:00 server time | Hard intraday floor — **must include floating P&L**, not just closed |
| Max overall loss | −10%, static or trailing-from-peak | Absorbing barrier (lose) |
| Min trading days | 0–4 | Forces minimum activity |
| Consistency rule | Best day ≤ 30–45% of total profit | Kills lottery-ticket trades; forces many small wins |
| News restriction | Some firms: no entries ±2 min around high-impact | We blackout ±15 min anyway |
| Weekend holding | Often prohibited | Matches our flat-overnight design |
| Prohibited | HFT, tick scalping, latency arb, martingale, group hedging | M15–H1 design is clean on all counts |

### Consequence: risk is a solver, not a multiplier

Every cycle the risk module recomputes available budget rather than applying a fixed percent:

```
daily_budget_left   = daily_loss_limit   − realised_today − floating_loss
overall_budget_left = overall_loss_limit − (reference_peak − current_equity)

risk_per_trade = min(
    base_risk_frac,
    daily_budget_left   / k,     # k = trades you must be able to lose today
    overall_budget_left / m      # m = trades you must be able to lose in the challenge
)
```

If `daily_budget_left` drops below one unit of risk → **no new entries, flatten, halt until next server day.** This is a hard rule, not a suggestion, and it is the difference between failing a challenge and merely having a bad day.

### Optimal risk fraction is solvable, not guessable

Maximising `P(+8% before −10%, with a −5% daily gate)` for a strategy with a measured edge is a first-passage problem. It has a distinctly non-obvious answer: **larger risk raises variance faster than it raises drift, and therefore *lowers* pass probability.** For realistic gold intraday edges the optimum typically lands around **0.25–0.5% risk per trade with ≤2 concurrent positions** — well below what most people use.

We will not guess this. Phase 7 builds a Monte Carlo over the *measured* edge distribution and solves for the risk fraction that maximises pass probability, then reports the probability itself so the decision to attempt a challenge is made on evidence.

---

## 2. Architecture

Two principles dominate everything else:

> **1. The backtester and the live engine run the same compiled `Strategy` code.**
> **2. Python never reimplements anything the C++ core does — it calls into it.**

Signal generation has exactly one code path, in C++. The only difference between backtest and live is which `Broker` implementation is plugged in behind a common interface. Backtest-vs-live divergence is the #1 killer of deployed strategies, and here it is prevented structurally: it is literally the same machine code making the decision.

```
┌──────────────────────────────────────────────────────────────────────┐
│                  xauterm   (Dear ImGui + ImPlot, C++)                │
│   chart · blotter · equity · risk gauges · optimizer · journal       │
└──────────────────────────┬───────────────────────────────────────────┘
                           │  direct memory reads / lock-free ring buffer
┌──────────────────────────▼───────────────────────────────────────────┐
│                     libxaucore   (C++20)                             │
│                                                                       │
│  ┌───────────┐  ┌──────────┐  ┌────────────┐  ┌──────────────────┐   │
│  │ TickStore │→ │ BarBuild │→ │  Features  │→ │    Strategy      │   │
│  │ mmap'd    │  │ time/$/  │  │  price     │  │  (pure logic)    │   │
│  │ binary    │  │ imbalance│  │  micro     │  │  bars → intent   │   │
│  └───────────┘  └──────────┘  │  cross     │  └────────┬─────────┘   │
│                                │  session   │           │             │
│                                └────────────┘           ▼             │
│                                              ┌──────────────────────┐ │
│                                              │  Risk / Prop solver  │ │
│                                              │  sizing + guards     │ │
│                                              └────────┬─────────────┘ │
│                          ┌────────────────────────────┴───┐           │
│                          ▼                                ▼           │
│                 ┌────────────────┐              ┌──────────────────┐  │
│                 │ BacktestBroker │              │    MT5Broker     │  │
│                 │  tick replay   │              │  extern "C" ABI  │  │
│                 │  spread/slip   │              │                  │  │
│                 └────────────────┘              └────────┬─────────┘  │
└──────────┬──────────────────────────────────────────────┼────────────┘
           │ pybind11                                      │ #import DLL
           ▼                                               ▼
 ┌─────────────────────────┐                  ┌───────────────────────┐
 │  Python  (research)     │                  │  XauBridgeEA.mq5      │
 │  ingest · LightGBM ·    │                  │  thin: OnTick → DLL   │
 │  Optuna · SHAP ·        │                  │  → OrderSend()        │
 │  DSR / PBO · plots      │                  └───────────┬───────────┘
 └─────────────────────────┘                              ▼
                                                 MetaTrader 5 terminal
```

The risk layer runs inside the backtest too. A backtest that ignores the daily-loss halt is not a backtest of the strategy you will actually run.

### Repo layout

```
trading bot by AR/
├── CMakeLists.txt                  # CMake 3.25+, presets, MSVC 2022 x64
├── CMakePresets.json
├── vcpkg.json                      # manifest mode — pinned dependency versions
├── cpp/
│   ├── core/                       # libxaucore — no GUI, no Python, no MT5
│   │   ├── include/xau/
│   │   │   ├── tick_store.hpp      # mmap'd binary tick reader
│   │   │   ├── bar_builder.hpp     # time / dollar-volume / tick-imbalance bars
│   │   │   ├── features/           # price.hpp micro.hpp cross.hpp session.hpp
│   │   │   ├── strategy.hpp        # THE Strategy interface
│   │   │   ├── broker.hpp          # abstract; Backtest | MT5 implementations
│   │   │   ├── costs.hpp           # spread / slippage / latency / swap model
│   │   │   ├── risk/               # sizing.hpp prop_solver.hpp guards.hpp
│   │   │   ├── model/              # LightGBM C-API inference wrapper
│   │   │   ├── calendar.hpp        # event blackouts, sessions, server-time offset
│   │   │   └── journal.hpp         # SQLite trade journal
│   │   ├── src/
│   │   └── tests/                  # Catch2 — including golden-file regressions
│   ├── term/                       # xauterm — the terminal UI
│   │   ├── panels/                 # chart, blotter, equity, risk, optimizer, log
│   │   ├── render/                 # ImGui + ImPlot + DX11 backend, candle item
│   │   └── main.cpp
│   ├── bridge/
│   │   ├── mt5_dll/                # extern "C" POD-only ABI for MQL5
│   │   └── pybind/                 # pybind11 module `xaucore`
│   └── bench/                      # google-benchmark: replay throughput
├── mql5/
│   └── XauBridgeEA.mq5             # thin EA: ticks in, order intents out
├── python/
│   ├── ingest/                     # dukascopy, fred, calendar, cot → tick store
│   ├── training/                   # lightgbm + optuna → model.txt
│   ├── validation/                 # purged CV, CPCV, DSR, PBO (calls xaucore)
│   └── research/                   # notebooks — never imported by anything
├── data/                           # gitignored
│   ├── ticks/XAUUSD/YYYY-MM.bin    # mmap'd binary tick store
│   ├── models/                     # exported LightGBM .txt
│   └── journal.sqlite
├── config/
│   ├── settings.yaml               # symbol, timeframe, sessions, features
│   ├── prop_rules.yaml             # firm DD / target / consistency rules
│   └── .env                        # MT5 login, Telegram token (gitignored)
└── docs/PLAN.md
```

---

## 3. Why C++ — and where it honestly does not help

C++ is the right call here, but for a specific reason worth being precise about, because "it's faster" is not by itself a good enough justification for the added complexity.

### The real justification

The validation methods in §8 that actually protect you from overfitting — Combinatorial Purged CV, Deflated Sharpe, PBO — require **hundreds to thousands of full backtest paths**. That turns a per-pass cost into a per-experiment cost, and the per-experiment cost decides whether you run the validation after every change or only when you can spare a night.

**Measured**, both sides, on the real tick format doing the same trivial per-tick work (running sum, min/max, one comparison):

| Replay mode | Throughput | Measured where |
|---|---|---|
| Python, per-tick event loop | ~1.0–1.25M ticks/s | i5-1334U — `python/bench/bench_python_scan.py` |
| numpy, vectorised | ~84M ticks/s | same machine — *but see below* |
| **C++ `TickStore` scan** | **722M ticks/s, 11.6 GB/s** | CI — `cpp/bench/bench_tick_store` |

That is **~600× on pure iteration**, and at 11.6 GB/s it is memory-bandwidth-bound rather than compute-bound, which was the design target for the store.

Two caveats that matter more than the headline:

- The C++ figure is a **reader** benchmark doing trivial work. A real strategy — features, barrier checks, order simulation — will pull it well down, plausibly into 20–100M ticks/s. That is exactly why the Phase 0 gate is a conservative **20M/s** rather than whatever the reader happens to manage.
- **Python does not degrade the same way**, because it is already dominated by interpreter overhead rather than by the work itself: adding a token EMA and two barrier checks moved it from 1.00 to 1.25M ticks/s. So expect the practical gap, once the Phase 1 engine actually does something, to land around **20–100×**.

A 1000-path CPCV over ~300M ticks on 12 cores: **~5.6 hours** in Python, **~35 seconds** at the measured reader rate, a few minutes at the conservative gate rate. Any of those is the difference between validating after every change and validating once.

**Why not just vectorise in numpy?** Because whether a stop is hit depends on the state the previous tick left. That sequential dependency is exactly what vectorisation removes. numpy is fine for the fast screening pass in §9 and useless for the event-driven engine.

> *Revision history, because this number has moved twice and both moves mattered.* Rev 1 claimed ~50k ticks/s for Python and "days to weeks" per experiment — far too pessimistic about Python. Rev 2 corrected Python to ~1M ticks/s but compared it against the 20M/s **gate** instead of measured C++, yielding "~16×" — too pessimistic about C++. The table above is measured on both sides. The decision was never in doubt; the honest size of the margin was.

### Where C++ earns its keep

| Component | Why |
|---|---|
| Tick replay | ~300M–1B ticks for 10 years of XAUUSD. Python ~1M ticks/s; C++ 20–50M ticks/s. |
| Feature computation | Rolling stats over tick streams, order-flow imbalance — tight loops over contiguous memory |
| CPCV / walk-forward | Embarrassingly parallel; one engine per thread over a shared read-only mmap |
| Parameter sweeps | 10k configs overnight instead of never |
| The terminal UI | 60 fps with a million candles, reading engine memory with zero serialisation |
| Live execution | Sub-millisecond decision, inside the MT5 terminal process via DLL |

### Where C++ does NOT help — keep these in Python

| Component | Why not |
|---|---|
| Model training | LightGBM is already C++ internally; the Python binding is a thin wrapper. Zero gain, large cost. |
| Hyperparameter search | Optuna's overhead is negligible next to the objective evaluation (which *is* C++, via pybind11) |
| Data ingest | I/O bound. Dukascopy fetch, FRED, calendar scraping — Python libraries exist and work. |
| Statistical validation | scipy/statsmodels for DSR/PBO — runs on trade lists, not tick streams. Milliseconds either way. |
| Exploratory research | Iteration speed beats run speed. A notebook calling `xaucore` gets both. |
| Plotting for reports | matplotlib/plotly. The *live* charts are ImPlot; report charts are Python. |

Rebuilding LightGBM's training loop or Optuna in C++ would cost weeks and buy nothing. Resist it.

### Tick store format

Simple, mmap-friendly, cache-friendly. Prices as integers to keep replay deterministic and comparisons exact:

```cpp
#pragma pack(push, 1)
struct Tick {              // 16 bytes
    int64_t  ts_us;        // UTC microseconds since epoch
    int32_t  bid_pts;      // price in points; 1 pt = 0.01 USD
    uint16_t spread_pts;   // ask = bid + spread
    uint16_t flags;        // MT5 tick flags: bid/ask/last/volume updated
};
#pragma pack(pop)
static_assert(sizeof(Tick) == 16);
```

Partitioned one file per `symbol/YYYY-MM`. **Sizing, corrected from measurement:** the synthetic generator calibrated to a realistic session envelope produces ~2.3M ticks/month, i.e. ~28M/year; real Dukascopy XAUUSD is likely 30–80M/year in recent years. So 10 years is roughly **300M–1B ticks, or 5–16 GB** at 16 B/tick — not the 40–60 GB an earlier draft claimed. That fits the 174 GB free on this machine with room to spare, alongside a compressed raw `.bi5` cache of a few GB.

Delta-encoding within 64 KB chunks would reach ~8 B/tick, but at these volumes there is no reason to; the ingest reports actual counts, so replace the estimate with a measurement before optimising anything.

Sequential replay over mmap'd memory is bandwidth-bound, not compute-bound. That is the design target.

### Performance targets (these are acceptance criteria, not aspirations)

| Operation | Target |
|---|---|
| Tick replay, single core | ≥ 20M ticks/s |
| Full 10-year single-strategy backtest | < 20 s |
| 1,000-path CPCV, 12 threads | < 15 min |
| Parameter sweep, 10k configs | overnight |
| Terminal redraw, 500k candles loaded | 60 fps |
| Live decision, bar close → `OrderSend` | < 1 ms in-DLL |

Benchmarks live in `cpp/bench/` and run in CI. A regression here is a build failure, not a "we'll look at it later."

---

## 4. Data

### 4.1 Sources

| Source | What for | Notes |
|---|---|---|
| **Prop firm's MT5 server** | Symbol specs, live prices, *your actual spread* | History depth on prop servers is shallow (weeks–months of M1). Pull it anyway — it is the ground truth about your execution venue. |
| **Dukascopy** | Historical bid/ask ticks back to ~2003 | Free, high quality, millisecond stamps. This is the backtest backbone. `.bi5` = LZMA-compressed; decompress in the Python ingest step, write our binary format. |
| **FRED** | `DFII10` (10y **real** yield), `DGS10`, `DTWEXBGS` | Real yields are the single strongest macro driver of gold. |
| **MT5 / yfinance** | DXY, XAGUSD, ES/SPX, VIX, USDJPY | Many brokers carry these as CFDs — prefer same-venue timestamps. |
| **Economic calendar** | High-impact US/EU events, scheduled times | MT5's `calendar_*` API if the broker exposes it, else Trading Economics. Store **scheduled** times. |
| **CFTC COT** | Managed-money net gold futures positioning | Weekly, released Friday for Tuesday's data — respect the 3-day lag strictly. |

### 4.2 The three timestamp traps

These cause more silent backtest inflation than any modelling mistake:

1. **Broker server time ≠ UTC.** Usually GMT+2/+3, and it *shifts with US DST*. Store everything in UTC internally; convert only at the edges. Derive the offset from `symbol_info_tick().time` vs system UTC and re-check daily — otherwise session logic silently breaks twice a year.
2. **A bar's timestamp is its OPEN.** An M15 bar stamped `14:00` covers `14:00–14:15`. You can only act on it at `14:15`. Off-by-one here is the most common lookahead bug in retail backtests, and it typically inflates results by 2–5×. Encode this in the `BarBuilder` type system so it cannot be got wrong: a `ClosedBar` is a distinct type from a `FormingBar`, and `Strategy` only accepts the former.
3. **Macro data gets revised.** FRED returns the *current* value of a series, not what was known at the time. Use ALFRED vintages, or restrict yourself to never-revised series (market prices).

---

## 5. Feature engineering

**Rule for every feature:** computable at bar close from data available at that instant, and stationary enough to generalise. No raw price levels — they are non-stationary and the model will memorise 2020 instead of learning structure.

### Price & volatility (from XAUUSD itself)
- Log returns over 1 / 3 / 6 / 12 / 24 / 96 bars
- Realised vol: Parkinson and Garman-Klass estimators, ATR(14), vol-of-vol
- **Vol regime as a percentile rank** over a trailing 60 days — rank, never level
- ATR-normalised distance to: EMA20/50/200, session VWAP, prior-day H/L/C, Asia-session range edges
- Range compression: NR7, short-ATR / long-ATR ratio, Bollinger bandwidth percentile
- Fractionally-differentiated price (`d ≈ 0.3–0.5`) — retains memory while achieving stationarity

### Microstructure (from ticks — this is where the C++ tick store pays off)
- **Spread level and spread percentile** — strongly predictive of both execution cost and regime
- Tick count and uptick/downtick imbalance — a legitimate order-flow proxy without L2 data
- Amihud illiquidity proxy, Roll spread estimator, quote intensity

Most retail systems skip these because computing them over billions of ticks in Python is impractical. Ours is not.

### Cross-asset (the actual drivers of gold)
- **US 10y real yield change (`DFII10`)** — the dominant macro driver
- DXY return, plus *rolling beta of gold to DXY*
- Gold/silver ratio z-score
- VIX level and change; ES return (risk-on/off)
- **Correlation-regime feature:** gold flips between "safe haven" (positive to VIX) and "anti-dollar" (negative to DXY) behaviour. Knowing which regime you are in is itself a real, tradeable feature — and one most retail models miss entirely.

### Calendar & session
- Session one-hot in UTC: Asia / London / London-NY overlap / NY-late
- Minutes to next high-impact event, minutes since last
- London AM/PM fix proximity (10:30 / 15:00 London) — gold has genuine structure here
- Day-of-week, NFP day (first Friday), FOMC day, month-end / quarter-end
- COT managed-money net z-score (weekly, correctly lagged)

### Hygiene rules
- No centred rolling windows. Ever.
- Every scaler, imputer and encoder fitted **inside** the CV fold, never on the full sample.
- No target-derived normalisation.
- Feature count kept modest (~40–80). Gold at M15 does not have the sample size to support 500 features without overfitting.

---

## 6. Bars, labels, and meta-labeling

### Bars

Start with **M15 time bars** for interpretability. Then test **dollar-volume bars** and **tick-imbalance bars**: they sample by information flow rather than by the clock, producing returns far closer to IID, which materially improves any ML on top. Gold's activity is extremely session-skewed — dead in late Asia, violent at NY open — so this matters more here than in most markets. All three bar types are built by the same `BarBuilder` from the same tick stream.

### Triple-barrier labelling

```
upper    = entry + tp_mult · ATR
lower    = entry − sl_mult · ATR
vertical = entry_time + max_hold          # e.g. 32 bars ≈ 8h on M15
label    = +1 / −1 / 0, by whichever barrier is touched first
```

Vol-scaled barriers are essential. A fixed 30-point target means something completely different on a 0.6% day than on a 2.5% day, and fixed barriers make the model learn volatility rather than direction. Barrier resolution runs against **ticks**, not bars — bar-level resolution cannot tell you which barrier was hit first within a bar, and guessing biases results.

### Meta-labeling — the core of this design

Two stages, and this is where "highest accuracy" becomes a coherent goal:

1. **Primary model decides the side.** Can be a plain rule (e.g. break of the Asia range in the direction of the daily trend). It is allowed to be mediocre — its job is **recall**, generating a candidate set.
2. **Secondary ML model decides whether to take the bet, and how big.** Trained *only* on the primary's signals, it answers one binary question: *will this trade hit TP before SL?*

Why this beats asking a model to predict raw direction:

- Predicting raw next-move direction on M15 gold is close to impossible (~51% ceiling).
- Predicting *whether a specific well-defined setup will work* is a much better-posed problem with genuine signal (typically 58–68% achievable).
- The output is a probability, which maps directly onto position sizing.
- Precision is what you can push hard, and precision is what costs you money when it is wrong.

**Sample weighting is mandatory:** weight by label uniqueness (overlapping label windows otherwise double-count and inflate confidence), by absolute return magnitude, and apply time decay so recent regimes matter more.

---

## 7. Models — strict order of attack

Do not skip ahead. Each step exists to make the next one interpretable.

1. **Cost-only null model.** Random entries, exact cost model, same risk layer. Establishes the negative baseline and tells you exactly how much edge the costs eat.
2. **Rule baselines — no ML at all.** These are the bar to beat, all implemented as C++ `Strategy` subclasses:
   - London-open opening-range breakout (07:00–08:00 UTC range, ATR-filtered)
   - Asia-range fade during Asia, Asia-range breakout at London open
   - Trend continuation: pullback to EMA in the direction of the daily trend
   - Volatility-compression breakout (NR7 / Bollinger squeeze)

   Many gold edges that actually survive are **session-structural, not statistical.** It is entirely possible a rule baseline is the final product and the ML only gates it.
3. **LightGBM meta-labeler** on top of the best 1–2 baselines. Train in Python; export `model.txt`; load in C++ via the LightGBM C API (`LGBM_BoosterCreateFromModelfile`) so inference is native and identical in backtest and live. SHAP for interpretability — which matters when you need to decide whether a live drawdown is normal or a broken model.
4. **Only if step 3 works:** regime switching (HMM or vol/correlation quantile buckets), and optionally a small sequence model exported to ONNX and run through ONNX Runtime in C++.

**Deep learning is not where the edge is at this data scale.** Do not start there. A transformer on 40k M15 bars will overfit magnificently and teach you nothing.

Hyperparameters via **Optuna, with the objective evaluated by `xaucore` through pybind11** over purged CV — the search runs in Python, the work happens in C++. *(Built in Phase 1. The binding suite mirrors two C++ gate tests figure for figure, so if Python ever drifts onto a different code path both suites fail.)* Hard, *recorded* trial cap (~100). The trial count feeds the deflated-Sharpe correction in §8; if you do not count your trials honestly, you cannot know whether your result is real.

---

## 8. Validation — the part that decides whether any of this is real

This is where most retail algo projects fail invisibly. Non-negotiables:

- **Purged K-fold with embargo.** Labels overlap in time (a trade opened at 10:00 with an 8h hold overlaps one opened at 12:00). Plain K-fold leaks badly. Purge training samples whose label window overlaps the test window, then embargo a further ~1% of the sample after each test fold.
- **Walk-forward, anchored *and* rolling.** Train 12mo → test 3mo → step 3mo. Report the *distribution* across folds, never just the aggregate.
- **Combinatorial Purged CV (CPCV)** to generate hundreds of backtest paths and a *distribution* of Sharpe rather than one number. A single Sharpe is not evidence. **This is the method C++ makes affordable** — parallel across threads, one engine instance per thread over the shared mmap.
- **Deflated Sharpe Ratio and PBO.** DSR corrects the observed Sharpe for number of trials, sample length, skew and kurtosis. Report DSR, not SR. Probability of Backtest Overfitting should be < 0.5, ideally < 0.3.
- **Untouched holdout.** Carve off the most recent 12 months on day one. Look at it exactly once, at the very end. If you look twice, it is no longer a holdout and you have lost your only unbiased estimate.
- **Cost stress test.** Must stay profitable at **2× measured spread and 2× slippage**. If it dies at 1.5×, it was a cost-arbitrage illusion, not an edge.
- **Regime slices.** Report separately across 2013–15 (bear), 2019–20 (COVID vol), 2022 (rate shock), 2023–25 (central-bank-buying bull). A strategy that only works in one regime is a bet on that regime returning.

### Leakage checklist — automated as Catch2 tests, run in CI

- [ ] **Shift the whole feature matrix forward one bar.** Performance should degrade *smoothly*. A collapse means you were using same-bar information.
- [ ] **Shuffle labels within folds** → performance must fall to the cost-only baseline.
- [ ] **Run the full pipeline on a synthetic random walk** with matched vol structure → must find no edge. If it finds one, the harness itself is broken and every result so far is void.
- [ ] No feature correlates > 0.95 with the forward return.
- [ ] Every scaler / imputer / encoder is fitted inside the fold.
- [ ] Calendar features use **scheduled** event times, not revised release data.
- [ ] Entry price is never the signal bar's close — always the next bar's open or worse.
- [ ] Symbol specs and cost parameters are constants from a snapshot, not fitted to the test period.

---

## 9. Backtest engine (C++)

**Custom event-driven engine, tick-resolution.** MT5 semantics (lot sizing on a 100 oz contract, `trade_stops_level`, freeze level, filling modes, swap) are specific enough that generic frameworks quietly mis-model them — and "quietly" is the problem.

Two execution modes sharing one `Strategy`:
- **Bar-close mode** for fast screening: decisions on closed bars, fills modelled from the tick stream.
- **Full tick mode** for anything you would actually trade: every tick walked, stops and targets resolved in true sequence.

### The fill model must include

| Effect | Why it matters on gold |
|---|---|
| **Tick-level spread**, time-varying | Gold's spread blows out 5–20× around NFP/CPI/FOMC and at the 21:00–22:00 UTC rollover. A constant spread assumption is the most common source of fake edge. |
| **Slippage** = base + k·σ_recent + event multiplier | 0.5–3 points typical; 10–50 on news |
| **Latency** 100–300 ms | Price moves between signal and fill — replay the tick stream forward by the latency before filling |
| **Stops fill at the worst side of a gap** | Never at the stop price when the market jumps through it |
| **`trade_stops_level`** | Broker minimum stop distance — violating orders are *rejected*. Model it, or you will backtest trades that cannot exist. |
| **Commission** | Prop gold is often commission-free with a wider spread — measure yours |
| **Swap** | Punitive on gold; another reason to stay intraday |
| **Requote / rejection probability** in high-vol windows | Your news-time fills are worse than you think |

### Validating the engine itself

An unvalidated backtester is a random number generator with good typography. Before trusting any result:

- Reconcile an always-long buy-and-hold to the cent against a hand calculation.
- The cost-only null model must show negative expectancy of the *predicted* magnitude, not just "negative".
- **Golden-file regression:** a fixed strategy over a fixed date range must produce a byte-identical trade list across builds, compilers and thread counts. This test is what keeps a refactor from silently changing your results.

> **Built and measured (Phase 1).** Buy-and-hold reconciles to a hand calculation on both sides. The null model — random entries on a deliberately *flat* price — loses exactly one spread, two slippages and one commission per trade, with zero sampling error by construction; that is a direct test that the fill model and the cost model agree with each other, not a statistical one. Throughput: **a decade of ticks in 2.2 s on MSVC and 1.4 s on GCC** against the 20 s requirement.
>
> On determinism, be precise about what is and is not yet proven. Run-to-run determinism inside one binary is tested. Across compilers, both produce the same 381 trades and the same −0.1324 USD expectancy from the same run, which is strong evidence. A true golden-file regression — a committed expected trade list, compared byte for byte — is still outstanding, and until it exists a refactor could still change results without anything failing.

---

## 10. The terminal (`xauterm`)

A native, docked, dark-theme trading terminal. Same binary serves **Research mode** (backtest replay, optimisation) and **Live mode** (demo or funded), because looking at live and backtest through different lenses is how you fail to notice they disagree.

### Why Dear ImGui + ImPlot

| Option | Verdict |
|---|---|
| **Dear ImGui + ImPlot** ✅ | Immediate-mode, single static binary, MIT. Panels read engine memory directly — **zero serialisation**. ImPlot handles millions of points at 60 fps. The docking branch gives a genuine multi-panel terminal layout. This is what a lot of trading desks use for internal tools. |
| Qt6 | Better form widgets, much heavier, licensing to think about, charts need custom OpenGL work to match ImPlot's throughput. Overkill for a single-user tool. |
| Web (React + TradingView Lightweight Charts) | Prettiest charts by far, but requires serialising every update across a socket, a second toolchain, and a second deployment. Wrong trade for a desktop terminal that must show live risk without a frame drop. |

Candlestick rendering is a custom ImPlot item (~80 lines; ImPlot ships an official example). DX11 backend on Windows.

### Panels

**Chart** — candlesticks, multi-timeframe, indicator overlays, **trade markers** (entry/exit arrows, SL/TP lines, MFE/MAE shading per trade), crosshair with OHLC readout, pan/zoom over the full history. Click a trade in the blotter → chart jumps to it. *This panel will find more bugs than any metric you compute.*

**Equity & drawdown** — equity curve, underwater plot, rolling Sharpe, per-fold walk-forward bands. In live mode, the backtest-expected curve is overlaid on the realised one — divergence is visible immediately.

**Risk gauges** — in live mode this is the visual centrepiece: daily loss budget consumed, overall loss budget consumed, distance to profit target, consistency-rule headroom. Big, colour-coded, unmissable. If you can see one thing across the room, it should be this.

**Blotter** — open positions, pending orders, live P&L, per-position risk.

**Journal** — every intent, order, fill and rejection with the full feature vector that produced it, filterable and sortable. Diagnosing a live problem without the inputs is guesswork.

**Backtest runner** — pick strategy, date range, cost profile; live progress bar; results the moment it finishes (which, at 20 s for 10 years, means you actually iterate).

**Optimizer** — parameter sweep heatmaps, CPCV Sharpe *distribution* histogram with the DSR threshold drawn on it, PBO readout. Seeing the distribution rather than a single number is what stops you fooling yourself.

**Market** — current spread and its percentile, session clock in both UTC and server time, countdown to the next high-impact event, blackout status.

**Log** — the structured log stream, filterable by level and subsystem.

### Determinism and reproducibility

Because backtest and live share compiled code, reproducibility is a hard requirement, not a nicety:

- Prices as integer points throughout; P&L accumulated in integer cents where possible
- `/fp:precise`, never fast-math; no FMA contraction differences between builds
- No dependence on hash-map iteration order — sorted containers or explicit ordering
- RNG seeded and passed explicitly, never global
- Every backtest result stamped with git SHA, config hash, data range and cost profile
- The golden-file test from §9 guards all of the above

---

## 11. Risk and execution

### Sizing

```
risk_$  = equity × risk_frac                       # risk_frac from the prop solver, §1
sl_dist = sl_mult × ATR                            # in price terms
lots    = risk_$ / (sl_dist × contract_size)       # contract_size = 100 oz for XAUUSD
lots    = clamp(round_to(lots, volume_step), volume_min, volume_max)
```

Then apply (a) the prop constraint solver from §1, and (b) a **volatility-targeting overlay** scaling `risk_frac` inversely to recent realised vol, so P&L variance stays stable across regimes. Without vol targeting, a strategy sized for 2024 gold will be sized wrong for a 2020-style vol spike.

### Hard controls — all mandatory before a single live order

- Daily equity floor → flatten and disable until the next server day
- Overall equity floor → flatten, **permanently halt, require manual re-arm**
- Max concurrent positions (2), max trades per day
- **News blackout:** no new entries T−15m to T+15m around high-impact events, plus any firm-specific window
- Session gate: entries only in configured windows; **force flat 30 min before Friday close**
- Max-spread gate: skip the entry if spread exceeds its Nth percentile
- **Consistency governor:** as today's profit approaches X% of cumulative profit, reduce size — this directly serves the prop consistency rule, and most bots ignore it until it fails them
- **Kill switch:** a file on disk, a terminal button, *and* a Telegram command; checked every cycle

Every guard is a C++ predicate with its own unit test, and the whole guard stack runs identically in backtest and live.

### The MT5 bridge

MT5 has no C++ API. The clean, low-latency route is a **thin MQL5 Expert Advisor that imports our DLL**:

```
OnTick()  →  xau_push_tick(...)          # EA hands the tick to the DLL
          →  DLL detects new closed bar
          →  Strategy + Features + Risk run natively
          →  returns OrderIntent (POD struct)
          →  EA calls OrderSend() / OrderModify()
          →  xau_report_result(MqlTradeResult)   # DLL journals and reconciles
```

The EA stays under ~250 lines and contains **no trading logic whatsoever**. All decisions happen in the same compiled code the backtest ran.

Practical constraints, in order of how likely they are to bite:

- **Confirm your prop firm allows EAs with DLL imports before committing to this design.** Some do not. Fallback: pure-MQL5 EA plus a named-pipe or ZeroMQ link to the C++ process — costs ~50–200 µs, which is irrelevant at M15. Design the ABI so either transport works.
- The terminal user must enable *Tools → Options → Expert Advisors → Allow DLL imports*.
- The DLL must be **64-bit, MSVC-built** to match the MT5 terminal ABI.
- MQL5 marshals C ABI only — `extern "C"`, POD structs, no C++ name mangling, **no exceptions crossing the boundary** (catch everything, return error codes).
- `xauterm` attaches to a shared-memory ring buffer the DLL publishes, so the UI never blocks the trading thread.

### Order management

- Act **only on a new closed bar** — never on the forming bar. Enforced by the type system (§4.2).
- Unique `magic` number and comment tag on every order, so our positions are never confused with manual ones.
- Explicit `type_filling` from `symbol_info.filling_mode`; sensible `deviation`; retry with backoff on `REQUOTE` / `PRICE_OFF` / `PRICE_CHANGED`; hard-fail and alert on `NO_MONEY` / `INVALID_STOPS`.
- **Idempotency:** write the intent to SQLite *before* `OrderSend`, reconcile after. On restart, replay intents against `PositionsTotal()` / history deals. **Broker state is always the truth** — never trust internal state over the broker.
- **Reconciliation every cycle.** Any divergence → alert and refuse to trade until resolved. A bot that trades while confused about its own positions is how accounts die.
- Terminal watchdog: heartbeat, auto-reconnect, alert if down > 60 s.

---

## 12. Operations

- `xauterm` and the MT5 terminal launched at logon; watchdog process restarts either on death.
- Structured JSON logs (spdlog) + SQLite journal recording every intent, order, fill and rejection **with the full feature vector** that produced it.
- **Telegram alerts** (reusing your existing bot experience): fills, rejections, drawdown warnings at 50% / 75% of the daily budget, kill-switch trips, periodic heartbeat.
- **Live-vs-backtest drift monitor.** Replay the backtest engine over the live period and compare fill prices, spreads and signal counts. This is the earliest possible warning that something is wrong, and it fires long before the P&L tells you.
- **Feature drift:** PSI / KS tests on live feature distributions vs training. Alert on drift, auto-halt on extreme drift.
- **Weekly review:** realised Sharpe against the CPCV distribution. If live falls below the 5th percentile for 4 consecutive weeks, halt and re-examine.

---

## 13. Stack and build

### C++ (the core)

```
C++20, MSVC 2022 x64 (must match the MT5 terminal ABI), CMake 3.25+ with presets,
vcpkg in manifest mode for pinned, reproducible dependencies.

ui:        imgui (docking branch), implot, DirectX 11 backend
model:     LightGBM C API (inference); optionally onnxruntime
data:      mio or platform mmap, zlib/lzma for Dukascopy .bi5
util:      spdlog, nlohmann_json or simdjson, fmt, SQLiteCpp
parallel:  std::execution / oneTBB thread pool
test:      Catch2, google-benchmark
bindings:  pybind11
```

### Python (ingest, training, statistics)

```
Python 3.11/3.12, 64-bit from python.org — NOT the Microsoft Store build
             (its sandboxed site-packages causes problems with native modules
              and DLL loading). Use a venv either way.

core:    numpy, pandas, polars, pyarrow, duckdb
ml:      scikit-learn, lightgbm, optuna, shap
stats:   scipy, statsmodels, arch
data:    requests, fredapi, custom dukascopy fetcher
mt5:     MetaTrader5 (for spec discovery and one-off history pulls only)
test:    pytest, hypothesis
viz:     matplotlib, plotly
```

**Note on `mlfinlab`:** the López de Prado utilities (purged CV, triple barrier, CPCV, DSR) now sit behind a paid licence. We implement them ourselves — the heavy parts in C++, the statistics in Python. Roughly 400 lines total, and writing them means we actually understand the assumptions we are relying on.

---

## 14. Phased roadmap with kill gates

Each phase ends at a gate. **If a gate fails, stop and reconsider — do not proceed hoping the next phase fixes it.** The gates are this plan's most valuable component.

### Phase 0 — Toolchain and data foundation (5–7 days)
- MSVC 2022 + CMake + vcpkg manifest skeleton, CI building on push
- Binary tick store format; Python Dukascopy ingest → `.bin`; C++ mmap reader
- MT5 connect (Python), discover the gold symbol name (varies: `XAUUSD`, `GOLD`, `XAUUSD.r`, `XAUUSDm`), dump the **full symbol spec** to a committed `symbol_spec.json`; detect and log the server↔UTC offset
- **Gate:** 10 years of ticks in the store, gap- and outlier-checked; C++ reader sustains ≥ 20M ticks/s in `cpp/bench/`

### Phase 1 — Backtest engine and cost model (10–14 days)
- Event-driven engine, MT5-accurate broker sim, full cost model, `Strategy` interface, pybind11 bindings
- **Gate:** reconciles to a hand-calculated buy-and-hold to the cent; the null model shows the predicted negative expectancy; 10-year backtest < 20 s; golden-file test green

### Phase 2 — Terminal MVP (7–10 days)
- ImGui/ImPlot shell with docking; chart with candles + trade markers; equity/drawdown; blotter; log; backtest runner
- **Gate:** you can visually step through a backtest and inspect every individual trade on the chart

Built early on purpose: this panel is a debugging instrument, and it will catch engine bugs that no summary statistic reveals.

### Phase 3 — Rule baselines (7 days)
- The four session/structure strategies from §7, walk-forward tested

> **Built (Phase 3).** All four baselines, the session/indicator primitives and
> the walk-forward runner are implemented and green on both compilers. The gate
> itself is **not evaluated**: the only data on hand is the synthetic driftless
> random walk built as the §8 null, where all four correctly lose money (best PF
> 0.689 at 2× costs). Running the gate needs real Dukascopy history — until then
> `run_baselines` prints a banner and declines to give a verdict.
- **Gate:** at least one shows PF > 1.05 after **2× costs**. **If none do, stop.** The problem is the cost/timeframe combination, not the model — revisit the horizon before investing weeks in ML. This gate has saved more projects than any other.

### Phase 4 — Feature engine and labels (7–10 days)
- C++ feature pipeline (price/micro/cross/session), tick-resolution triple-barrier labels, sample weights
- **Gate:** every automated leakage test in §8 passes

### Phase 5 — Meta-labeling model (10–14 days)
- LightGBM trained in Python on the best baseline's signals; exported and loaded in C++; Optuna over purged CV via pybind11
- **Gate:** identical predictions Python-side and C++-side on a fixed sample; material out-of-sample improvement over Phase 3

### Phase 6 — Validation suite and optimizer panel (7 days)
- Parallel CPCV runner, DSR, PBO, cost stress, regime slices; heatmaps and Sharpe-distribution histograms in the terminal
- **Gate:** DSR > 0, PBO < 0.3, survives 2× costs

### Phase 7 — Risk, prop solver, MT5 execution bridge (10–14 days)
- Constraint solver, vol targeting, all guards; `extern "C"` ABI + `XauBridgeEA.mq5`; order manager, reconciler; Monte Carlo pass probability
- **Gate:** P(pass challenge) > 60% at the solved risk fraction; clean reconciliation over a week of demo ticks; kill switch tested and proven

### Phase 8 — Demo forward test (minimum 6 weeks — not compressible)
- Full live stack on a demo account with the firm's exact rules configured
- **Gate:** ≥ 40 trades; live-vs-backtest drift within tolerance; zero unresolved reconciliation incidents; no ops outages

### Phase 9 — Challenge
- Run at the solved risk fraction, all guards armed

**Realistic total: 16–20 weeks.** The C++ core and terminal add roughly 5–6 weeks over a Python-only build, and buy back validation quality that a Python build cannot practically achieve. Phase 8 cannot be shortened — six weeks of forward testing is the cheapest insurance available.

---

## 15. Kill criteria — agree to these now, while it is cheap

Writing these down *before* becoming emotionally invested is the highest-return item in this document.

- Phase 3 gate fails → intraday M15 gold at prop costs may not be viable. Revisit the timeframe before spending weeks on ML.
- DSR ≤ 0 after honest trial counting → there is no edge. Do not deploy.
- Demo Sharpe below the 5th percentile of the CPCV distribution → do not fund the challenge.
- Any unresolved reconciliation divergence → do not trade until it is understood.
- Live drawdown exceeding the worst CPCV path → halt and re-examine; do not "wait it out".

---

## 16. Open items to confirm before Phase 7

- **Does your prop firm permit EAs with DLL imports?** This decides the bridge design — resolve it early, it is cheap to ask and expensive to discover late.
- Exact rule set: daily loss on balance or equity? Trailing or static max DD? Consistency percentage? News restrictions?
- Whether the firm's MT5 server exposes the economic calendar API
- Measured spread profile on the firm's server across sessions and event windows
- Commission structure on gold
