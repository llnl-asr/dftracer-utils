"""Series.rank leaves a float NaN unranked, as it does a null (pandas rank)."""

import math

import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame

X = [1.0, float("nan"), 3.0, None, 2.0]


def _utils(x):
    return DataFrame.from_dict({"x": x})["x"]


def _same(got, want):
    assert len(got) == len(want)
    for g, w in zip(got, want):
        if w is None or (isinstance(w, float) and math.isnan(w)):
            assert g is None or (isinstance(g, float) and math.isnan(g))
        else:
            assert g == pytest.approx(w)


def test_proposal_repro_rank_and_pct():
    _same(_utils(X).rank().to_list(), pd.Series(X).rank().tolist())
    _same(_utils(X).rank(pct=True).to_list(), pd.Series(X).rank(pct=True).tolist())


def test_dense_rank_skips_nan():
    _same(_utils(X).rank(method="dense").to_list(), pd.Series(X).rank(method="dense").tolist())


@pytest.mark.parametrize("method", ["average", "min", "max", "dense", "first"])
@pytest.mark.parametrize("ascending", [True, False])
@pytest.mark.parametrize("pct", [False, True])
def test_every_method_and_direction_matches_pandas(method, ascending, pct):
    rng = np.random.default_rng(7)
    x = rng.integers(0, 6, 80).astype(float)
    x[rng.random(80) < 0.2] = np.nan
    want = pd.Series(x).rank(method=method, ascending=ascending, pct=pct).tolist()
    # utils spells pandas "first" as "ordinal"
    got = _utils(x.tolist()).rank(
        method="ordinal" if method == "first" else method, ascending=ascending, pct=pct
    )
    _same(got.to_list(), want)


def test_all_nan_column():
    _same(_utils([float("nan"), float("nan")]).rank(pct=True).to_list(), [None, None])


def test_zero_over_zero_slope_is_unranked():
    with np.errstate(invalid="ignore"):
        slope = (np.array([1.0, 0.0, 3.0]) / np.array([2.0, 0.0, 2.0])).tolist()
    _same(_utils(slope).rank(pct=True).to_list(), pd.Series(slope).rank(pct=True).tolist())


def test_integers_are_unchanged():
    x = [3, 1, 2, 2]
    _same(_utils(x).rank().to_list(), pd.Series(x).rank().tolist())
