# SPDX-License-Identifier: MIT
"""The pandas missing-data names inside ``apply`` functions, ``DataFrame.eval``
strings, plans and expressions: a NaN of a float column is missing, as pandas
reads it. A function is traced when it is a plain expression and lowered from
its source when it holds control flow; both give the pandas answer."""

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series, col  # noqa: E402

from .common import skip_below_pandas3  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

NAN = float("nan")
FLOATS = [1.5, NAN, 3.0, NAN, -2.5]


def _na(values):
    return ["na" if v is None or (isinstance(v, float) and v != v) else v for v in values]


@pytest.mark.parametrize("dtype", ["float64", "float32"])
def test_traced_lambda_with_isna_and_notna(dtype):
    skip_below_pandas3()
    ours = Series.from_list(FLOATS).cast(dtype)
    ref = pd.Series(FLOATS, dtype=dtype)
    assert ours.apply(lambda x: x.isna()).to_list() == ref.isna().tolist()
    assert ours.apply(lambda x: x.isnull()).to_list() == ref.isna().tolist()
    assert ours.apply(lambda x: x.notna()).to_list() == ref.notna().tolist()
    assert ours.apply(lambda x: x.notnull()).to_list() == ref.notna().tolist()


@pytest.mark.parametrize("dtype", ["float64", "float32"])
def test_traced_lambda_with_fillna(dtype):
    skip_below_pandas3()
    ours = Series.from_list(FLOATS).cast(dtype)
    ref = pd.Series(FLOATS, dtype=dtype)
    assert ours.apply(lambda x: x.fillna(0)).to_list() == ref.fillna(0).tolist()


@pytest.mark.parametrize("dtype", ["float64", "float32"])
def test_source_lowered_lambda_with_if(dtype):
    skip_below_pandas3()
    ours = Series.from_list(FLOATS).cast(dtype)
    ref = pd.Series(FLOATS, dtype=dtype)
    assert (
        ours.apply(lambda x: 0 if x.isna() else x).to_list() == ref.where(ref.notna(), 0).tolist()
    )
    assert (
        ours.apply(lambda x: x if x.notna() else 9).to_list() == ref.where(ref.notna(), 9).tolist()
    )
    assert ours.apply(lambda x: x.fillna(1) if x.isna() else x).to_list() == ref.fillna(1).tolist()


def test_source_lowered_fillna_with_a_column_value():
    skip_below_pandas3()
    df = DataFrame({"a": FLOATS, "b": [10.0, 20.0, 30.0, 40.0, 50.0]})
    out = df.eval("a.fillna(b)")
    ref = pd.DataFrame({"a": FLOATS, "b": [10.0, 20.0, 30.0, 40.0, 50.0]}).eval("a.fillna(b)")
    assert out.to_list() == ref.tolist()


def test_df_eval_isna():
    skip_below_pandas3()
    values = [1.0, NAN, None, 4.0]
    ours = DataFrame({"x": values}).eval("x.isna()")
    ref = pd.DataFrame({"x": values}).eval("x.isna()")
    assert ours.to_list() == ref.tolist()
    ours = DataFrame({"x": values}).eval("x.notna() & (x > 1)")
    ref = pd.DataFrame({"x": values}).eval("x.notna() & (x > 1)")
    assert _na(ours.to_list()) == _na(ref.tolist())


def test_is_none_tests_only_the_nulls():
    df = DataFrame({"x": [1.0, NAN, None]})
    assert df.eval("x is None").to_list() == [False, False, True]


@pytest.mark.parametrize("dtype", ["float64", "float32"])
def test_lazy_filter_with_isna_streams(dtype):
    skip_below_pandas3()
    data = {"a": FLOATS + [None], "i": [1, 2, 3, 4, 5, 6]}
    df = DataFrame(data)
    df = df.with_column("a", df["a"].cast(dtype))
    ref = pd.DataFrame(data)
    ref["a"] = ref["a"].astype(dtype)
    kept = df.lazy().filter(col("a").isna()).collect()
    assert kept["i"].to_list() == ref[ref["a"].isna()]["i"].tolist()
    kept = df.lazy().filter(col("a").notna()).collect()
    assert kept["i"].to_list() == ref[ref["a"].notna()]["i"].tolist()
    # The row-wise node runs through the streaming collector, in small morsels.
    plan = df.lazy().filter(col("a").isna())
    assert plan.collect()["i"].to_list() == [2, 4, 6]


def test_expr_isna_is_a_row_wise_node_in_a_with_column():
    skip_below_pandas3()
    df = DataFrame({"a": FLOATS})
    ref = pd.Series(FLOATS)
    out = df.lazy().with_column("m", col("a").isna()).collect()
    assert out["m"].to_list() == ref.isna().tolist()
    assert df.apply(col("a").isna()).to_list() == ref.isna().tolist()


def test_expr_fillna_on_integer_and_string_columns_keeps_the_type():
    df = DataFrame({"i": [1, None, 3], "s": ["a", None, "c"]})
    eager = df.apply(col("i").fillna(0))
    assert eager.to_list() == [1, 0, 3]
    assert eager.dtype == df["i"].dtype
    eager_s = df.apply(col("s").fillna("z"))
    assert eager_s.to_list() == ["a", "z", "c"]
    assert eager_s.dtype == df["s"].dtype
    lazy = (
        df.lazy()
        .with_column("i2", col("i").fillna(0))
        .with_column("s2", col("s").fillna("z"))
        .collect()
    )
    assert lazy["i2"].to_list() == [1, 0, 3]
    assert lazy["s2"].to_list() == ["a", "z", "c"]
    assert lazy["i2"].dtype == df["i"].dtype
    assert lazy["s2"].dtype == df["s"].dtype


def test_series_dropna_and_fillna_on_an_empty_float_column():
    empty = Series.from_list([], "float64")
    assert empty.dropna().to_list() == []
    assert empty.fillna(0.0).to_list() == []


def test_frame_dropna_axis_1_on_an_empty_float_frame():
    skip_below_pandas3()
    ours = DataFrame({"a": Series.from_list([], "float64"), "b": Series.from_list([], "int64")})
    ref = pd.DataFrame({"a": pd.Series([], dtype="float64"), "b": pd.Series([], dtype="int64")})
    assert list(ours.dropna(axis=1).columns) == list(ref.dropna(axis=1).columns)
    assert list(ours.dropna(axis=1, how="all").columns) == list(
        ref.dropna(axis=1, how="all").columns
    )
    for kwargs in ({"how": "any"}, {"thresh": 0}, {"thresh": 1}):
        assert list(ours.dropna(axis=1, **kwargs).columns) == list(
            ref.dropna(axis=1, **kwargs).columns
        )
    assert len(ours.dropna()) == len(ref.dropna()) == 0


def test_frame_fillna_and_dropna_without_a_nan_share_the_data():
    df = DataFrame({"a": [1.0, 2.0, 3.0], "b": [1, 2, 3]})
    for out in (df.fillna(0.0), df.dropna(), df.fill_nan(0.0), df.drop_nans()):
        assert out["a"].to_list() == [1.0, 2.0, 3.0]
        assert out.index_names() == []
        assert out["a"].encoding == 0
    s = Series.from_list([1.0, 2.0, 3.0])
    assert s.fill_nan(0.0).to_list() == [1.0, 2.0, 3.0]
    assert s.drop_nans().to_list() == [1.0, 2.0, 3.0]


def test_expr_drop_nulls_in_with_column_follows_polars():
    pl = pytest.importorskip("polars")
    data = {"a": [1.0, None, 3.0]}
    with pytest.raises(Exception) as theirs:
        pl.DataFrame(data).with_columns(pl.col("a").drop_nulls().alias("b"))
    assert "length" in str(theirs.value).lower() or "shape" in type(theirs.value).__name__.lower()
    with pytest.raises(Exception) as ours:
        DataFrame(data).with_column("b", col("a").drop_nulls())
    assert "length" in str(ours.value).lower() or "shape" in str(ours.value).lower()
    kept = DataFrame(data).select(col("a").drop_nulls().alias("a"))
    assert kept["a"].to_list() == [1.0, 3.0]


def test_expr_fillna_with_a_string_on_a_float_column_raises():
    df = DataFrame({"a": FLOATS})
    with pytest.raises((RuntimeError, ValueError)):
        df.apply(col("a").fillna("z"))


def test_series_fillna_with_a_string_on_a_numeric_column_raises():
    with pytest.raises((RuntimeError, ValueError, TypeError)):
        Series.from_list([1, None, 3]).fillna("z")
    with pytest.raises((RuntimeError, ValueError, TypeError)):
        Series.from_list([1.0, None, 3.0]).fillna("z")


def test_frame_fill_null_with_a_string_fills_only_the_string_columns():
    df = DataFrame({"f": [1.0, None], "i": [1, None], "s": ["a", None]})
    out = df.fill_null("z")
    assert out["f"].to_list() == [1.0, None]
    assert out["i"].to_list() == [1, None]
    assert out["s"].to_list() == ["a", "z"]
