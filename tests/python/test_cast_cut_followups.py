"""fillna for bool and string columns, native bool/string constants, NaN text, SIMD cast to bool, and cut options."""

import time

import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame

BINS = [0, 80, 100, 120, 1e9]


def _frame(**cols):
    return DataFrame.from_dict(cols)


def _cut_series(values):
    return _frame(x=values)["x"], _frame(b=BINS)["b"]


# ---- fillna for bool and string


def test_fillna_bool_column():
    s = _frame(x=[0.0, None, 3.0])["x"].astype("bool")
    assert s.fillna(True).to_list() == [False, True, True]
    assert s.fillna(False).to_list() == [False, False, True]


def test_fillna_string_column():
    s = _frame(x=["a", None, "c"])["x"]
    assert s.fillna("x").to_list() == ["a", "x", "c"]


def test_fillna_wrong_kind_is_an_error():
    with pytest.raises(Exception):
        _frame(x=["a", None])["x"].fillna(1)


# ---- bool and string constants built natively


def test_bool_and_string_constants_on_a_null_first_frame():
    d = _frame(x=[1.0, None, 3.0, None]).with_columns(flag=True, name="k", n=2)
    out = d.to_pandas()
    assert out["flag"].tolist() == [True] * 4
    assert out["name"].tolist() == ["k"] * 4
    assert out["n"].tolist() == [2] * 4


def test_string_constant_on_a_frame_with_no_rows():
    d = _frame(x=[1.0]).limit(0)
    out = d.with_columns(name="k").to_pandas()
    assert len(out) == 0 and "name" in out.columns


def test_five_million_row_string_constant_builds_without_a_python_list():
    d = DataFrame.from_dict({"x": np.arange(5_000_000, dtype="float64").tolist()})
    t0 = time.perf_counter()
    out = d.with_columns(name="k")
    elapsed = time.perf_counter() - t0
    col = out["name"]
    assert len(col) == 5_000_000
    assert col[0] == "k" and col[4_999_999] == "k"
    assert elapsed < 5.0


# ---- NaN and infinity text


def test_nan_and_infinity_text():
    x = [1.5, float("nan"), float("inf"), float("-inf")]
    assert _frame(x=x)["x"].astype("string").to_list() == ["1.5", "nan", "inf", "-inf"]
    assert _frame(x=x)["x"].astype(str).to_list() == ["1.5", "nan", "inf", "-inf"]


# ---- SIMD number to bool agrees with numpy/pandas


@pytest.mark.parametrize("n", [0, 1, 63, 64, 65, 1000, 4097])
@pytest.mark.parametrize("kind", ["int64", "int32", "float64", "float32"])
def test_number_to_bool_matches_numpy(n, kind):
    rng = np.random.default_rng(n)
    x = rng.integers(-2, 3, n).astype(kind)
    if kind.startswith("float") and n:
        x[:: max(1, n // 7)] = np.nan
    got = DataFrame.from_pandas(pd.DataFrame({"x": x}))["x"].astype("bool").to_list()
    want = [None if (isinstance(v, float) and np.isnan(v)) else bool(v) for v in x.tolist()]
    assert got == want


# ---- cut options


def test_cut_default_unchanged():
    x, b = _cut_series([80.0, 100.0, 120.0, 95.0])
    assert x.cut(b).to_list() == [2, 3, 4, 2]


def test_cut_right_inner_equals_pandas():
    vals = [80.0, 100.0, 120.0, 95.0]
    x, b = _cut_series(vals)
    want = pd.cut(pd.Series(vals), BINS, labels=False).tolist()
    assert want == [0, 1, 2, 1]
    assert x.cut(b, right=True, outer=False).to_list() == want


def test_cut_outside_range_is_null_with_inner_bins():
    x, b = _cut_series([0.0, 2e9])
    assert x.cut(b, right=True, outer=False).to_list() == [None, None]


def test_cut_right_with_outer_bins():
    x, b = _cut_series([80.0, 0.0, 2e9])
    assert x.cut(b, right=True).to_list() == [1, 0, 5]


@pytest.mark.parametrize("right", [True, False])
def test_cut_random_matches_pandas(right):
    rng = np.random.default_rng(3)
    vals = rng.integers(-20, 140, 300).astype(float).tolist()
    x, b = _cut_series(vals)
    want = pd.cut(pd.Series(vals), BINS, right=right, labels=False)
    got = x.cut(b, right=right, outer=False).to_list()
    assert got == [None if pd.isna(v) else int(v) for v in want]


def test_cut_null_value_gives_null_bin():
    x = _frame(x=[80.0, None, 95.0])["x"]
    assert x.cut(_frame(b=BINS)["b"], right=True, outer=False).to_list() == [0, None, 1]
