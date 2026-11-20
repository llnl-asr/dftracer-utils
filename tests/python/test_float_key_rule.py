# SPDX-License-Identifier: MIT
"""One float key rule across the engine: -0.0 is 0.0 and every NaN is one
value (as pandas and Polars key a float), in unique, nunique, merge, isin and
mode."""

import math

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

NAN = float("nan")
NEG_NAN = -NAN
VALUES = [0.0, -0.0, NAN, NEG_NAN, 1.0, 1.0]


def _same(a, b):
    return [("nan" if isinstance(x, float) and math.isnan(x) else x) for x in a] == [
        ("nan" if isinstance(x, float) and math.isnan(x) else x) for x in b
    ]


def test_unique_and_nunique():
    ref, got = pd.Series(VALUES), Series.from_list(VALUES)
    assert _same(got.unique(maintain_order=True).to_list(), list(ref.unique()))
    assert got.nunique() == ref.nunique(dropna=False) == 3


def test_isin_with_nan_and_signed_zero():
    ref = pd.Series(VALUES).isin([NAN, -0.0]).tolist()
    assert Series.from_list(VALUES).is_in([NAN, -0.0]).to_list() == ref


def test_mode_is_positive_zero():
    value = Series.from_list([-0.0, 0.0, -0.0, 2.0]).mode().to_list()[0]
    assert value == 0.0 and math.copysign(1.0, value) == 1.0


def test_merge_matches_zero_and_nan_keys():
    left = {"k": [0.0, NAN, 1.0], "x": [1, 2, 3]}
    right = {"k": [-0.0, NEG_NAN, 1.0], "y": [10, 20, 30]}
    ref = pd.DataFrame(left).merge(pd.DataFrame(right), on="k")
    got = DataFrame(left).join(DataFrame(right), on="k")
    assert got.num_rows == len(ref) == 3
    assert got["y"].to_list() == ref["y"].tolist()
