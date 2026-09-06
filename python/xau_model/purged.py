"""Purged K-fold with embargo, and the walk-forward split.

This is the file that decides whether every number downstream is real.

Financial labels overlap in time. A trade opened at 10:00 and held for four
hours shares its outcome with one opened at 12:00. Plain K-fold puts the first
in train and the second in test, and the model scores brilliantly by having
already seen most of the answer. The fix is two-part, and both parts matter:

  PURGE    drop any training row whose label window [entry, touch] overlaps the
           test fold's span. This removes the direct leak.

  EMBARGO  additionally drop training rows for a short period AFTER the test
           fold. Serial correlation means a row starting just after the test
           window still carries information about it, and purging alone does
           not catch that.

Lopez de Prado, Advances in Financial Machine Learning, ch. 7.
"""

from __future__ import annotations

import numpy as np


def purged_kfold_indices(
    entry_us: np.ndarray,
    touch_us: np.ndarray,
    n_splits: int = 5,
    embargo_frac: float = 0.01,
):
    """Yield (train_idx, test_idx) with overlapping labels purged.

    entry_us / touch_us define each row's label window. Rows must be sorted by
    entry_us -- the folds are contiguous in time, which is the only split that
    means anything for a time series.
    """
    n = len(entry_us)
    if n == 0 or n_splits < 2:
        return

    order = np.argsort(entry_us, kind="stable")
    if not np.array_equal(order, np.arange(n)):
        raise ValueError("rows must be sorted by entry_us before splitting")

    embargo_n = int(n * embargo_frac)
    bounds = np.linspace(0, n, n_splits + 1).astype(int)

    for k in range(n_splits):
        lo, hi = bounds[k], bounds[k + 1]
        if hi <= lo:
            continue
        test_idx = np.arange(lo, hi)

        # The test fold occupies this stretch of wall-clock time. Any training
        # label whose own window overlaps it is contaminated.
        test_start = entry_us[lo]
        test_end = touch_us[lo:hi].max()

        train_mask = np.ones(n, dtype=bool)
        train_mask[lo:hi] = False

        # Purge: a training label that ends after the test starts AND begins
        # before the test ends overlaps it. Both conditions are needed -- a
        # label wholly before or wholly after is fine.
        overlaps = (touch_us >= test_start) & (entry_us <= test_end)
        train_mask &= ~overlaps

        # Embargo: also drop the rows immediately following the test fold.
        if embargo_n > 0:
            train_mask[hi : min(n, hi + embargo_n)] = False

        train_idx = np.flatnonzero(train_mask)
        if len(train_idx) == 0:
            continue
        yield train_idx, test_idx


def walk_forward_indices(entry_us: np.ndarray, train_months: int = 12,
                         test_months: int = 3):
    """Anchored-then-rolling walk forward, in calendar time rather than row count.

    Row-count folds silently give 2015 (thin) and 2024 (dense) different amounts
    of wall-clock history, which makes fold-to-fold comparison meaningless.
    """
    if len(entry_us) == 0:
        return
    us_per_month = 30 * 24 * 3600 * 1_000_000
    t0, t1 = entry_us.min(), entry_us.max()

    train_us = train_months * us_per_month
    test_us = test_months * us_per_month

    start = t0
    while start + train_us + test_us <= t1:
        tr_end = start + train_us
        te_end = tr_end + test_us
        train_idx = np.flatnonzero((entry_us >= start) & (entry_us < tr_end))
        test_idx = np.flatnonzero((entry_us >= tr_end) & (entry_us < te_end))
        if len(train_idx) > 0 and len(test_idx) > 0:
            yield train_idx, test_idx
        start += test_us
