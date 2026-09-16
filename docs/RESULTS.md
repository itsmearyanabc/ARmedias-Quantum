# Measured results

Every number here came from the tools in this repo on real Dukascopy ticks.
Nothing is projected, assumed, or taken from a vendor's marketing.

## Data

| store | months | ticks | span | verdict |
|---|---|---|---|---|
| XAUUSD | 120/120 | 445,837,101 | 2015-01-01 .. 2024-12-31 | OK |
| XAGUSD | 120/120 | 134,529,223 | 2015-01-01 .. 2024-12-31 | OK |

Gold: all 435 originally-failed hours recovered. Silver: 32 of 87,600 hours
unrecovered after two passes, 0.04%.

## Phase 3 gate — profit factor after 2x costs

Fourteen strategies, four horizons. Every strategy improves monotonically with
the bar size, which is the cost hypothesis showing up in the data:

| strategy | M15 | H1 | H4 | D1 |
|---|---|---|---|---|
| InsideBarBreak | 0.287 | 0.456 | 0.773 | 0.923 |
| Rsi2Extreme | 0.425 | 0.549 | 0.753 | 0.873 |
| MomentumContinuation | 0.449 | 0.567 | 0.849 | 0.861 |

The only configuration to clear 1.05 is the regime-gated RSI-2 on gold D1.

## Phase 5 gate — does meta-labelling concentrate the edge

No. Out-of-fold AUC 0.513 (TrendPullback) and 0.530 (London), against 0.5 for a
coin flip, with individual folds at 0.479 and 0.494. The ~0.06 USD of gross
edge per trade is spread evenly across signals rather than concentrated in a
subset a gate could select.

## Phase 6 gate — does it survive being charged for the search

| | gold | silver |
|---|---|---|
| best by Sharpe | Rsi2InRange | Rsi2InRange |
| SR per trade | 0.0425 | -0.0812 |
| benchmark (luck across the search) | 0.1626 | 0.2518 |
| **DSR** | **0.053** | **0.000** |
| PBO | 0.274 | 0.012 |
| **verdict** | **FAIL** | **FAIL** |

Both observed Sharpes sit below what zero-edge strategies produce by luck
across a search this size. PBO passing while DSR fails is not a contradiction:
PBO asks whether selecting the in-sample winner picks badly, and it does not.
DSR asks whether the winner beats luck, and it does not. Consistently selecting
something that is not there passes the first test and fails the second.

## CPCV — the distribution behind the single number

126 distinct out-of-sample paths from 252 combinations of 5-of-10 blocks.
Fraction of paths with a positive Sharpe:

| strategy | gold | silver |
|---|---|---|
| RandomEntry (null) | 8% | 0% |
| Rsi2Extreme (ungated) | 25% | 2% |
| InsideBarBreak | 49% | 0% |
| **Rsi2InRange** | **76%** | **38%** |
| Rsi2RangesOnly | 62% | 17% |

This is the most informative view produced so far, and it separates two things
the DSR conflates. CONSISTENCY: on gold the gated strategy is positive in 76%
of resamplings against the null's 8%, and the gate lifts its own ungated parent
from 25% to 76% — that ordering is stable and is not what noise looks like.
MAGNITUDE: the median Sharpe is 0.0433 per trade, which is small enough that
70 trials of searching can produce it by luck, which is exactly what the DSR
of 0.045 says.

Both readings are correct. The effect is real in the sense that it reproduces
across resamplings and across the gate/no-gate comparison; it is not
established in the sense that it beats the search that found it.

Silver ranks the same strategies in the same order — Rsi2InRange first at 38%,
its ungated parent at 2%, the null at 0% — while every absolute number is worse.
Same ordering, thinner margin, consistent with costs rather than with absence.

## The measurement that corrects the optimistic reading

Walk-forward reports Rsi2InRange on silver at PF 1.033 over 101 trades. The
full-period run of the same strategy reports a negative Sharpe over 170. The
difference is the 69 trades walk-forward excludes while each fold warms up, and
those trades are net losers. Walk-forward is the more conservative methodology
and is not wrong -- but quoting only the flattering of two measurements of one
strategy would have been.

## Standing conclusion

No validated edge on either instrument. The regime effect replicates in
direction on silver and fails the cost stress there; on gold it clears the
Phase 3 gate and fails Phase 6. The MT5 bridge ships with an empty strategy
hook and halts by default, which is the correct state until this section says
something different.

Last updated: 2026-09-07
