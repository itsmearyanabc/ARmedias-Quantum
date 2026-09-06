"""Phase 5: train the meta-label gate and measure whether it is worth anything.

The primary strategy picks the side. This model answers one question per
signal -- "would this one have paid?" -- and the trade is taken only when it
says yes with enough confidence.

The bar this has to clear is not accuracy. It is money. Phase 3 measured the
problem exactly:

    gross edge   ~0.06 USD per trade
    cost         ~0.68 USD per trade

So a gate that merely predicts the label well is worthless. It has to
concentrate the edge by roughly an order of magnitude into the signals it keeps.
This script therefore reports expectancy in USD after costs, not just AUC --
an AUC of 0.60 that leaves expectancy negative is a failure, and saying so is
the whole point of the exercise.

    python -m xau_model.train_meta data/datasets/trendpullback_m15.csv
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd
from sklearn.metrics import roc_auc_score

try:
    import lightgbm as lgb
except ImportError:  # pragma: no cover
    lgb = None

from .purged import purged_kfold_indices, walk_forward_indices

META_COLS = {
    "event_us", "entry_us", "touch_us", "year", "holdout", "side",
    "barrier", "ret_atr", "meta", "uniqueness", "weight",
}


def load(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    df = df.sort_values("entry_us", kind="stable").reset_index(drop=True)
    return df


def feature_columns(df: pd.DataFrame) -> list[str]:
    return [c for c in df.columns if c not in META_COLS]


def usd_per_trade(ret_atr: np.ndarray, atr_usd: float, lots: float,
                  cost_usd: float) -> np.ndarray:
    """Convert an ATR-unit return into USD after costs.

    ret_atr is signed and already side-adjusted by the labeller. One lot is
    100 oz, so 0.01 lots is 1 oz and a 1.0 ATR move is atr_usd dollars on it.
    """
    oz = lots * 100.0
    return ret_atr * atr_usd * oz - cost_usd


def evaluate(name: str, y: np.ndarray, p: np.ndarray, ret_atr: np.ndarray,
             threshold: float, atr_usd: float, lots: float, cost_usd: float) -> dict:
    keep = p >= threshold
    n_keep = int(keep.sum())
    pnl_all = usd_per_trade(ret_atr, atr_usd, lots, cost_usd)
    pnl_keep = pnl_all[keep] if n_keep else np.array([])

    out = {
        "split": name,
        "n": int(len(y)),
        "n_kept": n_keep,
        "kept_frac": float(n_keep / len(y)) if len(y) else 0.0,
        "auc": float(roc_auc_score(y, p)) if len(np.unique(y)) > 1 else float("nan"),
        "base_rate": float(y.mean()),
        "kept_rate": float(y[keep].mean()) if n_keep else float("nan"),
        "exp_all_usd": float(pnl_all.mean()),
        "exp_kept_usd": float(pnl_keep.mean()) if n_keep else float("nan"),
        "net_all_usd": float(pnl_all.sum()),
        "net_kept_usd": float(pnl_keep.sum()) if n_keep else 0.0,
    }
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dataset")
    ap.add_argument("--threshold", type=float, default=0.5,
                    help="keep a signal when p(pay) is at least this")
    ap.add_argument("--folds", type=int, default=5)
    ap.add_argument("--embargo", type=float, default=0.01)
    ap.add_argument("--lots", type=float, default=0.01)
    ap.add_argument("--atr-usd", type=float, default=2.0,
                    help="typical M15 ATR in USD, for converting ATR units to money")
    ap.add_argument("--cost-usd", type=float, default=0.68,
                    help="measured round-turn cost per trade at --lots")
    ap.add_argument("--out", default="")
    args = ap.parse_args(argv)

    if lgb is None:
        print("lightgbm is not installed", file=sys.stderr)
        return 2

    df = load(Path(args.dataset))
    feats = feature_columns(df)

    # The holdout is carved off and not looked at. Section 8 allows exactly one
    # look, at the very end of the project -- not here.
    train_df = df[df["holdout"] == 0].reset_index(drop=True)
    print(f"dataset   {len(df)} rows, {len(train_df)} usable, "
          f"{len(df) - len(train_df)} held out and untouched")
    print(f"features  {len(feats)}")
    print(f"base rate {train_df['meta'].mean():.3f}")

    X = train_df[feats].to_numpy(dtype=np.float64)
    y = train_df["meta"].to_numpy(dtype=int)
    w = train_df["weight"].to_numpy(dtype=np.float64)
    ret = train_df["ret_atr"].to_numpy(dtype=np.float64)
    entry = train_df["entry_us"].to_numpy(dtype=np.int64)
    touch = train_df["touch_us"].to_numpy(dtype=np.int64)

    params = dict(
        objective="binary",
        learning_rate=0.03,
        num_leaves=15,
        min_data_in_leaf=100,
        feature_fraction=0.7,
        bagging_fraction=0.7,
        bagging_freq=1,
        # Gold's meta-labels are ~25% positive and the dataset is small. Heavy
        # regularisation is not timidity here, it is the difference between
        # learning a signal and memorising 9,000 rows.
        lambda_l2=10.0,
        verbosity=-1,
        num_threads=0,
    )

    rows = []
    oof_p = np.full(len(y), np.nan)

    print("\npurged K-fold (embargo "
          f"{args.embargo:.0%}), threshold {args.threshold}")
    for k, (tr, te) in enumerate(
        purged_kfold_indices(entry, touch, args.folds, args.embargo)
    ):
        ds = lgb.Dataset(X[tr], label=y[tr], weight=w[tr])
        model = lgb.train(params, ds, num_boost_round=300)
        p = model.predict(X[te])
        oof_p[te] = p
        r = evaluate(f"fold{k}", y[te], p, ret[te], args.threshold,
                     args.atr_usd, args.lots, args.cost_usd)
        r["n_train"] = int(len(tr))
        rows.append(r)
        print(f"  fold{k}  train {len(tr):5d}  test {len(te):5d}  "
              f"auc {r['auc']:.3f}  kept {r['kept_frac']:.1%}  "
              f"exp_all {r['exp_all_usd']:+.3f}  exp_kept {r['exp_kept_usd']:+.3f}")

    valid = ~np.isnan(oof_p)
    if valid.sum() > 0:
        agg = evaluate("oof", y[valid], oof_p[valid], ret[valid], args.threshold,
                       args.atr_usd, args.lots, args.cost_usd)
        print(f"\nout-of-fold  auc {agg['auc']:.3f}   base {agg['base_rate']:.3f} "
              f"-> kept {agg['kept_rate']:.3f}")
        print(f"             keeps {agg['kept_frac']:.1%} of signals "
              f"({agg['n_kept']} of {agg['n']})")
        print(f"expectancy   all {agg['exp_all_usd']:+.4f} USD/trade   "
              f"kept {agg['exp_kept_usd']:+.4f} USD/trade")
        print(f"net          all {agg['net_all_usd']:+.2f} USD   "
              f"kept {agg['net_kept_usd']:+.2f} USD")

        # The gate. Accuracy that does not become money is not progress.
        ok = agg["exp_kept_usd"] > 0
        print(f"\nPhase 5 gate: expectancy after costs on kept trades > 0  ->  "
              f"{'PASS' if ok else 'FAIL'}")
        if not ok:
            need = args.cost_usd / (args.atr_usd * args.lots * 100.0)
            print(f"             kept trades average {agg['exp_kept_usd']:+.4f} USD; "
                  f"they need a mean of {need:.3f} ATR just to cover cost")

    # Feature importance from a model fit on everything, for reading only --
    # never for selection, which would leak the test folds into the choice.
    full = lgb.train(params, lgb.Dataset(X, label=y, weight=w), num_boost_round=300)
    imp = sorted(zip(feats, full.feature_importance("gain")), key=lambda t: -t[1])
    print("\ntop features by gain")
    for name, g in imp[:8]:
        print(f"  {name:22s} {g:12.1f}")

    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(rows, indent=2))
        full.save_model(str(Path(args.out).with_suffix(".lgb")))
        print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
