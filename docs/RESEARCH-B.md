# Research round B — pre-registration

Written and committed **before any of it was run on real data.** The commit
that adds this file is the timestamp. Anything decided after seeing a result
gets recorded below as a deviation, with the reason, or it does not happen.

## Why there is a round B

`RESULTS.md` closed round A with no validated edge. Two findings shape what
comes next:

1. **Cost, not signal.** Every round-A rule is a bracket trade held for hours,
   with roughly 0.06 USD of gross edge against 0.68 USD of cost. Every one
   improves monotonically as the bar gets longer. The direction to test is
   longer holds, not a fifteenth intraday variation.
2. **The one pass got financing for free.** On D1 the strategy is called at
   00:00 UTC, so `flat_by_hour = 21` never fires and positions sit through the
   rollover. Both default symbol specs charged **zero swap**. The only Phase 3
   pass (gold D1 `Rsi2InRange`) and the Phase 6 analysis were priced as if
   overnight gold financing cost nothing. The engine also billed 9 swap nights
   a week instead of 7, which nobody noticed while the rate was zero.

## What changed in the machinery (before any run)

- **Financing.** MT5's interest-on-current-price swap mode: an annual rate on
  the position's current notional, 1/360 per night, Mon–Fri rollovers only,
  triple on Wednesday. Research default: **long −4.5% a year, short −1.0%**.
  These are an assumption, not a measurement (USD policy rate averaged over
  2015–2024 plus a typical retail markup), until `config/symbol_spec.json`
  carries the broker's figures. The 2× cost stress doubles financing along
  with spread and slippage.
- **Holdout plumbing.** `--from/--to` on `run_baselines`, `validate` and
  `evaluate`. `BacktestConfig::trade_from_us` lets a strategy warm up on
  earlier bars while forbidding any position before the holdout begins.
- **`evaluate`.** One strategy, split by side, with the long side set against
  the drift that holding gold for the same number of days earned. On an
  instrument that more than doubled, a trend rule's P&L is mostly beta unless
  shown otherwise.

## The holdout

Round A used 2015-01-01 .. 2024-12-31. **Everything from 2025-01-01 onward is
untouched** and is the holdout. Development runs pass `--to 2025-01-01`. Each
hypothesis that clears stage 1 touches the holdout exactly once, from a frozen
commit, with `--from 2024-01-01 --trade-from 2025-01-01` so a twelve-month
lookback is warm on day one.

Known contamination, stated rather than hidden: the broad path of gold since
2015, including a strong 2025, is public knowledge to whoever reads this,
the author included. No pre-registration can remove that. The drift control
exists because of it.

## Hypotheses

Parameters are the published ones. **None is tuned.** Lookbacks are in
calendar days, because a UTC-midnight D1 series carries a two-hour Sunday stub
each week, so a bar count isn't a fixed span of time.

| id | strategy | bars | rule |
|---|---|---|---|
| H0 | `Rsi2InRange` (round A) | D1 | unchanged; re-priced with financing; holdout test of the existing candidate |
| H1 | `TimeSeriesMomentum` | D1 | sign of the trailing 365-day return; reviewed every 28 days; 4-ATR(20) disaster stop; no target (Moskowitz, Ooi & Pedersen 2012) |
| H2 | `DonchianTrend` | D1 | close beyond the prior 77-day extreme (55 trading days); exit on the opposite 28-day extreme (20 trading days); 2-ATR(20) stop (Turtle System 2) |
| H3 | `AsiaDrift` | H1 | long from the first bar close at 23:00 or 00:00 UTC on nights into Mon–Fri, out at 07:00 UTC; one entry a night; 3-ATR(24) disaster stop; never crosses the rollover |

H1 and H2 are both trend bets and correlated. They count as two trials anyway,
because they are two.

**Trial count for the deflated Sharpe: 73** (round A's 70 plus H1–H3). H0 is
not a new trial; it is the same hypothesis re-priced and then confirmed.

## Decision rules

Gold is primary. Silver is a mechanism check, not a second chance.

### Stage 1 — development window, `--to 2025-01-01`

A hypothesis passes stage 1 only if **all** of these hold:

| gate | rule | tool |
|---|---|---|
| G1 cost | PF > 1.05 at 2× costs (spread, slippage **and financing**), at least 30 trades | `evaluate` |
| G2 not beta | H1/H2: long gross beyond drift > 0 **and** short-side gross ≥ 0. H3: long gross beyond drift > 0 | `evaluate` |
| G3 search | best by Sharpe in `validate --trials 73` at its timeframe, with DSR > 0.95 and PBO < 0.30 | `validate` |
| G4 mechanism | net > 0 on XAGUSD over the same window at 1× costs | `evaluate` |

The trade minimum is 30, not `run_baselines`' 100, because a rule reviewed
monthly produces 10–20 trades a year. The deflated Sharpe already charges for
a short sample, so the smaller count is not a loophole.

H0 is held to G1 and G2 only. It already failed G3 in round A, and the holdout
is a cleaner test than re-running a search it has already lost.

### Stage 2 — holdout, `--from 2024-01-01 --trade-from 2025-01-01`

Run once per stage-1 survivor, and once for H0 whatever stage 1 says. A
holdout passes only if:

- net > 0 at 1× costs, **and**
- long gross beyond drift > 0, **and**
- SR per trade is not below that strategy's CPCV 5th percentile from stage 1
  (PLAN §15).

### What counts as an edge

**Stage 1 and stage 2 both passed, on the same frozen commit.** Anything
less is reported as "no validated edge", with the numbers. Even a pass only
earns Phase 7's P(pass challenge) solve and Phase 8's six-week demo; it is not
a licence to fund anything.

## How to run it

```bash
# data: the dev window plus the holdout, both metals
cd python && python -m xau_ingest.dukascopy --start 2015-01 --end 2026-09 --out ../data/ticks/XAUUSD --verify
cd python && python -m xau_ingest.dukascopy --symbol XAGUSD --start 2015-01 --end 2024-12 --out ../data/ticks/XAGUSD --verify

# stage 1, per hypothesis (D1 for H0-H2, H1 bars for H3)
./build/gcc-release/bin/evaluate data/ticks/XAUUSD XAUUSD --strategy TimeSeriesMomentum --tf D1 --to 2025-01-01
./build/gcc-release/bin/validate data/ticks/XAUUSD XAUUSD --tf D1 --trials 73 --to 2025-01-01
./build/gcc-release/bin/evaluate data/ticks/XAGUSD XAGUSD --strategy TimeSeriesMomentum --tf D1 --to 2025-01-01

# stage 2, survivors only, once
./build/gcc-release/bin/evaluate data/ticks/XAUUSD XAUUSD --strategy TimeSeriesMomentum --tf D1 --from 2024-01-01 --trade-from 2025-01-01
```

## Deviations

None yet.
