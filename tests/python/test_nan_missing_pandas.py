# SPDX-License-Identifier: MIT
"""NaN as missing data under the pandas names.

``isna``, ``isnull``, ``notna``, ``notnull``, ``fillna`` and ``dropna`` treat a
NaN of a float column as missing, as pandas 3 does, on a Series, a DataFrame and
an expression. Every case runs on pandas and on ours over the same data."""

import math

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series, col  # noqa: E402

from .common import skip_below_pandas3  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

NAN = float("nan")
FLOATS = [1.5, NAN, None, 3.0, NAN, -2.5]
INTS = [1, None, 3, 4]
STRINGS = ["a", None, "c"]
BOOLS = [True, None, False]


def _norm(values):
    """A list with NaN as the text "nan", so a NaN and a null tell apart."""
    out = []
    for v in values:
        if v is None or v is pd.NA:
            out.append(None)
        elif isinstance(v, float) and math.isnan(v):
            out.append("nan")
        else:
            out.append(v)
    return out


def _pd_series(values):
    return (
        pd.Series(values, dtype="float64")
        if any(isinstance(v, float) for v in values)
        else pd.Series(values)
    )


# ---- pandas names -----------------------------------------------------------


@pytest.mark.parametrize(
    "values", [FLOATS, INTS, STRINGS, BOOLS], ids=["float", "int", "str", "bool"]
)
@pytest.mark.parametrize("name", ["isna", "isnull", "notna", "notnull"])
def test_pandas_missing_masks(values, name):
    skip_below_pandas3()
    ours = getattr(Series.from_list(values), name)()
    ref = getattr(_pd_series(values), name)()
    assert ours.to_list() == ref.tolist()


def test_pandas_masks_all_nan_and_empty():
    skip_below_pandas3()
    assert Series.from_list([NAN, NAN]).isna().to_list() == [True, True]
    assert Series.from_list([NAN, NAN]).notna().to_list() == [False, False]
    assert Series.from_list([1.0, 2.0]).isna().to_list() == [False, False]
    empty = Series.from_list([], "float64")
    assert empty.isna().to_list() == []
    assert empty.notna().to_list() == []


@pytest.mark.parametrize("fill", [0.0, 7.5, -1])
def test_pandas_fillna_series(fill):
    skip_below_pandas3()
    ours = Series.from_list(FLOATS).fillna(fill)
    ref = _pd_series(FLOATS).fillna(fill)
    assert _norm(ours.to_list()) == _norm(ref.tolist())
    assert ours.null_count == 0


def test_pandas_fillna_series_other_types():
    skip_below_pandas3()
    assert Series.from_list(INTS).fillna(0).to_list() == pd.Series(INTS).fillna(0).tolist()
    assert (
        Series.from_list(STRINGS).fillna("z").to_list() == pd.Series(STRINGS).fillna("z").tolist()
    )


def test_pandas_dropna_series():
    skip_below_pandas3()
    ours = Series.from_list(FLOATS).dropna()
    ref = _pd_series(FLOATS).dropna()
    assert ours.to_list() == ref.tolist()
    assert list(ours.index) == list(ref.index) if hasattr(ours, "index") else True
    assert Series.from_list(INTS).dropna().to_list() == pd.Series(INTS).dropna().tolist()
    assert Series.from_list([1.0, 2.0]).dropna().to_list() == [1.0, 2.0]


def _frame_data():
    return {
        "a": [1.0, NAN, 3.0, None, NAN],
        "b": [1, 2, None, 4, 5],
        "c": [NAN, NAN, None, None, 1.0],
    }


def _our_frame():
    return DataFrame(_frame_data())


def _pd_frame():
    return pd.DataFrame(_frame_data())


def _cols(frame):
    """The columns as lists, a null and a NaN both "na": pandas reads a None in
    an integer column as a NaN, so the two cannot tell apart there."""

    def na(values):
        return ["na" if v == "nan" or v is None else v for v in _norm(values)]

    if isinstance(frame, DataFrame):
        return {c: na(frame[c].to_list()) for c in frame.columns}
    return {c: na(frame[c].tolist()) for c in frame.columns}


def test_pandas_frame_isna_notna():
    skip_below_pandas3()
    for name in ("isna", "isnull", "notna", "notnull"):
        ours = getattr(_our_frame(), name)()
        ref = getattr(_pd_frame(), name)()
        assert _cols(ours) == _cols(ref), name


@pytest.mark.parametrize(
    "kwargs",
    [
        {},
        {"how": "all"},
        {"subset": ["a"]},
        {"subset": ["a", "b"]},
        {"thresh": 2},
        {"how": "any", "subset": "c"},
    ],
)
def test_pandas_frame_dropna_rows(kwargs):
    skip_below_pandas3()
    ours = _our_frame().dropna(**kwargs)
    ref = _pd_frame().dropna(**kwargs)
    assert _cols(ours) == _cols(ref)


@pytest.mark.parametrize(
    "kwargs", [{"axis": 1}, {"axis": 1, "how": "all"}, {"axis": 1, "thresh": 4}]
)
def test_pandas_frame_dropna_columns(kwargs):
    skip_below_pandas3()
    ours = _our_frame().dropna(**kwargs)
    ref = _pd_frame().dropna(**kwargs)
    assert list(ours.columns) == list(ref.columns)


def test_pandas_frame_dropna_keeps_a_frame_without_missing_values():
    skip_below_pandas3()
    data = {"a": [1.0, 2.0], "b": [1, 2]}
    assert _cols(DataFrame(data).dropna()) == _cols(pd.DataFrame(data).dropna())


def test_pandas_frame_fillna():
    skip_below_pandas3()
    ours = _our_frame().fillna(0)
    ref = _pd_frame().fillna(0)
    assert _cols(ours) == _cols(ref)
    ours = _our_frame().fillna({"a": 9.0, "c": -1.0})
    ref = _pd_frame().fillna({"a": 9.0, "c": -1.0})
    assert _cols(ours) == _cols(ref)


def test_pandas_expr_names():
    skip_below_pandas3()
    df = DataFrame({"a": FLOATS})
    ref = _pd_series(FLOATS)
    assert df.apply(col("a").isna()).to_list() == ref.isna().tolist()
    assert df.apply(col("a").isnull()).to_list() == ref.isna().tolist()
    assert df.apply(col("a").notna()).to_list() == ref.notna().tolist()
    assert df.apply(col("a").notnull()).to_list() == ref.notna().tolist()
    assert _norm(df.apply(col("a").fillna(0.0)).to_list()) == _norm(ref.fillna(0.0).tolist())


def test_pandas_expr_names_in_a_plan():
    skip_below_pandas3()
    df = DataFrame({"a": FLOATS})
    ref = _pd_series(FLOATS)
    out = (
        df.lazy().with_column("m", col("a").isna()).with_column("f", col("a").fillna(0.0)).collect()
    )
    assert out["m"].to_list() == ref.isna().tolist()
    assert _norm(out["f"].to_list()) == _norm(ref.fillna(0.0).tolist())
