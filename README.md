# XAUUSD Algorithmic Trading Terminal

[![ci](https://github.com/itsmearyanabc/ARmedias-Quantum/actions/workflows/ci.yml/badge.svg)](https://github.com/itsmearyanabc/ARmedias-Quantum/actions/workflows/ci.yml)

C++20 core, ImGui/ImPlot terminal, MT5 bridge. Gold intraday (M15–H1), sized for
a prop-firm challenge.

**The plan is [`docs/PLAN.md`](docs/PLAN.md).** Read section 0 before anything
else — it explains why "highest accuracy" is the wrong target and what this
optimises for instead.

---

## Status: Phases 0-2 built, Phase 1 gate met

| Phase | | |
|---|---|---|
| 0 | Toolchain and data foundation | in progress |
| 1 | Backtest engine and cost model | **gate met** — pybind11 outstanding |
| 2 | Terminal MVP | partial — see below |
| 3 | Rule baselines | not started |
| 4 | Feature engine and labels | not started |
| 5 | Meta-labeling model | not started |
| 6 | Validation suite | not started |
| 7 | Risk, prop solver, MT5 bridge | not started |
| 8 | Demo forward test (6+ weeks) | not started |
| 9 | Challenge | not started |

### Phase 0 checklist

- [x] Repo, CMake/vcpkg skeleton, CI
- [x] Binary tick format (`cpp/core/include/xau/types.hpp` ↔ `python/xau_ingest/tickfmt.py`)
- [x] C++ mmap tick reader, store, verifier
- [x] Synthetic tick generator (test data, and the Phase 4 random-walk null)
- [x] Dukascopy fetch + decode (written, tested offline, **not yet run**)
- [x] MT5 spec dump script (written, **needs MT5 installed to run**)
- [x] Python test suite — 25 tests green
- [x] **CI green on MSVC and GCC** — configure, build, tests, gate
- [x] **Throughput gate: 722M ticks/s (11.6 GB/s)** against a 20M/s target —
      memory-bandwidth-bound, as designed. Caveat: that is the *reader* doing
      trivial work; a real strategy will pull it well down, which is why the
      gate is set conservatively.
- [ ] C++ toolchain installed **locally** — CI builds it, but you need MSVC to
      run `xauterm` on this machine
- [ ] Real tick history ingested and verified
- [ ] `config/symbol_spec.json` generated and committed

### Phase 1 checklist

All three gate criteria met, on both MSVC and GCC.

- [x] Event-driven, tick-resolution engine (`cpp/core/src/engine.cpp`)
- [x] Cost model: tick spread, directional slippage, latency, commission, swap
- [x] MT5 semantics: lot ladder, `stops_level`, contract-size P&L
- [x] `Strategy` interface — the same object the live engine will drive
- [x] **Gate: reconciles to a hand calculation to the cent**, both sides
- [x] **Gate: null model loses exactly the predicted round-turn cost**, with
      zero sampling error by construction
- [x] **Gate: a decade backtests in 2.2 s (Windows) / 1.4 s (Linux)** against a
      20 s requirement — 134.9M and 211.3M ticks/s
- [x] Cross-platform determinism: 381 trades, expectancy −0.1324 USD,
      *identical* on MSVC and GCC
- [ ] pybind11 bindings so Python research calls this same engine

The fill asymmetries that make the model honest, each with a test:

| Behaviour | Why |
|---|---|
| Stops fill **through** gaps | The market never traded at the stop price it jumped over |
| Limits **never** slip | A resting limit fills at its own price; crediting the gap invents profit |
| Spread comes from the tick | A constant spread is the commonest source of fake edge on gold |
| Latency replays the stream forward | The price moves underneath the order |
| Stop inside the spread is rejected | A long fills on the ask and its stop triggers on the bid |
| Both levels hit in one tick → the stop wins | A backtest must not resolve its own ambiguity in its favour |

### Phase 2 checklist

Built ahead of Phase 1 on request, so the trade-display panels had no engine
to draw from at the time. That engine now exists; wiring the greyed-out panels
to it is the next terminal work.

- [x] `BarSeries` — time bars from ticks, with closed and forming kept separate
- [x] ImGui + ImPlot via FetchContent (no vcpkg needed)
- [x] Win32/DX11 host, docking, layout persisted to `xauterm.ini`
- [x] Dark instrument-panel theme
- [x] Chart: candlesticks, auto-fit Y, hover OHLC readout, line fallback when
      candles go sub-pixel
- [x] Session shading (London, London/NY overlap)
- [x] Volume and spread sub-plots with linked X axes
- [x] Market panel — last, spread, **spread percentile**, session
- [x] Store browser — click a month to jump the chart there
- [x] Log panel
- [x] Compiles clean under MSVC `/W4 /WX` in CI
- [ ] **Never actually run** — CI builds it but cannot open a window
- [ ] Trade markers, equity/drawdown, blotter, backtest runner *(need Phase 1)*
- [ ] Gate: step through a backtest and inspect every trade *(needs Phase 1)*

Run it once you have a compiler:

```bash
cmake --preset msvc-release && cmake --build --preset msvc-release
```

```bash
./build/msvc-release/bin/xauterm.exe data/ticks/XAUUSD XAUUSD
```

---

## Prerequisites

**A C++ toolchain — not yet installed on this machine.** Visual Studio 2022
Build Tools with the Desktop C++ workload; it bundles MSVC x64, the Windows SDK,
CMake and Ninja in one install:

```bash
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

MSVC specifically, rather than MinGW: the Phase 7 bridge is a DLL loaded by the
MT5 terminal, and matching its ABI avoids a class of problems that is miserable
to debug.

**Python 3.11+, 64-bit, from python.org** — not the Microsoft Store build, whose
sandboxed `site-packages` interferes with native modules and DLL loading.

```bash
python -m pip install -r python/requirements.txt
```

---

## Build and test

```bash
cmake --preset msvc-release && cmake --build --preset msvc-release && ctest --preset msvc-release
```

```bash
python -m pytest python/tests -q
```

## Generate test data and measure the gate

No download needed — the synthetic generator produces a realistic session
envelope (intensity, spread and volatility by session) over a driftless random
walk:

```bash
cd python && python -m xau_ingest.synth --months 12 --out ../data/ticks/XAUUSD
```

```bash
cd python && python -m xau_ingest.tickfmt ../data/ticks/XAUUSD --symbol XAUUSD
```

Measure what Python costs on the event-driven path, which is the baseline the
C++ engine has to beat (see PLAN.md §3):

```bash
cd python && python -m bench.bench_python_scan
```

```bash
./build/msvc-release/bin/bench_tick_store data/ticks/XAUUSD XAUUSD 20000000
```

Run a backtest and check the Phase 1 gate — a decade of ticks in under 20 s,
expressed as the rate that implies it:

```bash
./build/msvc-release/bin/bench_backtest data/ticks/XAUUSD XAUUSD 15000000
```

Both exit non-zero below their floor, so CI enforces the gates rather than
relying on anyone remembering to look.

## Ingest real history

Downloads from Dukascopy — tens of thousands of requests for a decade, so run it
deliberately. Raw files are cached under `data/raw/`, making re-runs free.

```bash
cd python && python -m xau_ingest.dukascopy --start 2015-01 --end 2024-12 --out ../data/ticks/XAUUSD --verify
```

Start with a single month to confirm the decode before committing to the full pull:

```bash
cd python && python -m xau_ingest.dukascopy --start 2024-01 --end 2024-01 --out ../data/ticks/XAUUSD --verify
```

## Discover the broker's symbol spec

Requires MT5 installed, running and logged in.

```bash
cd python && python -m xau_ingest.mt5spec --list
```

```bash
cd python && python -m xau_ingest.mt5spec --out ../config/symbol_spec.json
```

The account login is excluded by default, because that file is committed.

---

## Layout

```
cpp/core/     libxaucore — tick store, and later features, strategy, risk.
              Zero external dependencies: this gets linked into the MT5 bridge
              DLL, where every dependency is a deployment risk.
cpp/tests/    Unit tests on a ~60-line harness, so a fresh machine needs only a
              compiler and CMake. Catch2 arrives with vcpkg in Phase 1.
cpp/bench/    Throughput gates. These are acceptance criteria, not curiosities.
python/       Ingest, and later training and statistics — the I/O-bound and
              one-off work that gains nothing from C++ (see PLAN.md section 3).
data/         Gitignored. Ticks, raw cache, models, journal.
```

## Two things to sort out early

1. **Ask your prop firm whether EAs with DLL imports are permitted.** It decides
   the Phase 7 bridge design, and there is a fallback (named pipe / ZeroMQ) that
   costs ~100µs and is irrelevant at M15 — but the ABI should be shaped knowing
   which one you are building for.

2. **There is a git repository at `C:\`.** Someone ran `git init` at the drive
   root. This project has its own repo and is unaffected, but a `git add -A` in
   the wrong shell there would try to stage the entire drive.
