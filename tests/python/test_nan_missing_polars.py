# SPDX-License-Identifier: MIT
"""NaN as a value under the polars names.

``is_null``, ``fill_null`` and ``drop_nulls`` see only the nulls; ``is_nan``,
``is_not_nan``, ``fill_nan`` and ``drop_nans`` see only the NaNs, on a Series,
an expression, a DataFrame and a LazyFrame. Every case runs on polars and on
ours over the same data."""

import math

import pytest

pl = pytest.importorskip("polars")

from dftracer.utils import DataFrame, LazyFrame, Series, col  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

NAN = float("nan")
FLOATS = [1.5, NAN, None, 3.0, NAN, -2.5]
INTS = [1, None, 3, 4]
STRINGS = ["a", None, "c"]


def _norm(values):
    """A list with NaN as the text "nan", so a NaN and a null tell apart."""
    out = []
    for v in values:
        if v is None:
            out.append(None)
        elif isinstance(v, float) and math.isnan(v):
            out.append("nan")
        else:
            out.append(v)
    return out


def _frame_data():
    return {
        "a": [1.0, NAN, 3.0, None, NAN],
        "b": [1, 2, None, 4, 5],
        "c": [NAN, NAN, None, None, 1.0],
    }


def _our_frame():
    return DataFrame(_frame_data())


def _cols(frame):
    return {c: _norm(frame[c].to_list()) for c in frame.columns}


def test_polars_null_and_nan_are_apart():
    s = Series.from_list(FLOATS)
    p = pl.Series(FLOATS, dtype=pl.Float64, nan_to_null=False)
    assert s.is_null().to_list() == p.is_null().to_list()
    assert s.is_not_null().to_list() == p.is_not_null().to_list()
    assert s.is_nan().to_list() == p.is_nan().to_list()
    assert s.is_not_nan().to_list() == p.is_not_nan().to_list()
    assert _norm(s.fill_null(0.0).to_list()) == _norm(p.fill_null(0.0).to_list())
    assert _norm(s.drop_nulls().to_list()) == _norm(p.drop_nulls().to_list())


@pytest.mark.parametrize("value", [0.0, 9.5, 3, None])
def test_polars_series_fill_nan(value):
    s = Series.from_list(FLOATS)
    p = pl.Series(FLOATS, dtype=pl.Float64, nan_to_null=False)
    assert _norm(s.fill_nan(value).to_list()) == _norm(p.fill_nan(value).to_list())


def test_polars_series_drop_nans():
    s = Series.from_list(FLOATS)
    p = pl.Series(FLOATS, dtype=pl.Float64, nan_to_null=False)
    assert _norm(s.drop_nans().to_list()) == _norm(p.drop_nans().to_list())
    assert _norm(s.drop_nans().drop_nulls().to_list()) == [1.5, 3.0, -2.5]


def test_polars_nan_ops_on_other_types():
    for values in (INTS, STRINGS):
        s = Series.from_list(values)
        p = pl.Series(values)
        assert s.drop_nans().to_list() == p.drop_nans().to_list()
    assert Series.from_list(INTS).fill_nan(0).to_list() == pl.Series(INTS).fill_nan(0).to_list()
    assert Series.from_list(INTS).is_nan().to_list() == pl.Series(INTS).is_nan().to_list()


def test_polars_empty_series():
    empty = Series.from_list([], "float64")
    assert empty.fill_nan(1.0).to_list() == []
    assert empty.drop_nans().to_list() == []
    assert empty.is_not_nan().to_list() == []


def test_polars_labelled_series_keeps_its_labels_through_drop_nans():
    s = Series.from_list([1.0, NAN, 3.0, None]).dropna()
    assert s.to_list() == [1.0, 3.0]
    kept = Series.from_list([1.0, NAN, 3.0, None]).drop_nans()
    assert _norm(kept.to_list()) == [1.0, 3.0, None]


def _both_expr(df, ours, theirs):
    """The expression eagerly, as a plan, and in polars."""
    eager = _norm(df.apply(ours).to_list())
    lazy = _norm(df.lazy().with_column("r", ours).collect()["r"].to_list())
    ref = _norm(df.to_polars().select(theirs.alias("r"))["r"].to_list())
    assert eager == ref
    assert lazy == ref


def test_polars_expr_names():
    df = DataFrame({"a": FLOATS})
    _both_expr(df, col("a").is_not_nan(), pl.col("a").is_not_nan())
    _both_expr(df, col("a").is_nan(), pl.col("a").is_nan())
    _both_expr(df, col("a").fill_nan(5.0), pl.col("a").fill_nan(5.0))
    _both_expr(df, col("a").fill_nan(None), pl.col("a").fill_nan(None))
    _both_expr(df, col("a").fill_null(5.0), pl.col("a").fill_null(5.0))


def test_polars_expr_drop_nulls_and_drop_nans_in_a_select():
    df = DataFrame({"a": FLOATS})
    p = df.to_polars()
    assert _norm(df.select(col("a").drop_nulls().alias("a"))["a"].to_list()) == _norm(
        p.select(pl.col("a").drop_nulls().alias("a"))["a"].to_list()
    )
    assert _norm(df.select(col("a").drop_nans().alias("a"))["a"].to_list()) == _norm(
        p.select(pl.col("a").drop_nans().alias("a"))["a"].to_list()
    )


def _polars_frame():
    return pl.DataFrame(_frame_data())


@pytest.mark.parametrize("value", [0.0, 2, None])
def test_polars_frame_fill_nan(value):
    ours = _our_frame().fill_nan(value)
    ref = _polars_frame().fill_nan(value)
    assert _cols(ours) == _cols(ref)


@pytest.mark.parametrize("subset", [None, "a", ["a", "c"], ["b"]])
def test_polars_frame_drop_nans(subset):
    ours = _our_frame().drop_nans(subset)
    ref = _polars_frame().drop_nans(subset)
    assert _cols(ours) == _cols(ref)


def test_polars_frame_nan_ops_with_a_string_column():
    data = {"f": [1.0, NAN, 3.0], "s": ["x", "y", "z"], "i": [1, 2, 3]}
    ours = DataFrame(data).fill_nan(0.0)
    ref = pl.DataFrame(data).fill_nan(0.0)
    assert _cols(ours) == _cols(ref)
    assert _cols(DataFrame(data).drop_nans()) == _cols(pl.DataFrame(data).drop_nans())


@pytest.mark.parametrize("value", [0.0, None])
def test_polars_lazyframe_fill_nan(value):
    ours = _our_frame().lazy().fill_nan(value).collect()
    ref = _polars_frame().lazy().fill_nan(value).collect()
    assert _cols(ours) == _cols(ref)


def test_polars_lazyframe_drop_nans():
    ours = _our_frame().lazy().drop_nans().collect()
    ref = _polars_frame().lazy().drop_nans().collect()
    assert _cols(ours) == _cols(ref)
    assert isinstance(_our_frame().lazy().drop_nans(), LazyFrame)


def test_polars_lazyframe_nan_ops_chain_with_the_null_ops():
    ours = _our_frame().lazy().fill_nan(None).drop_nulls().collect()
    ref = _polars_frame().lazy().fill_nan(None).drop_nulls().collect()
    assert _cols(ours) == _cols(ref)
