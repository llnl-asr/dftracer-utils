# SPDX-License-Identifier: MIT
"""The names the coverage gate (scripts/api_coverage.py) found missing and the
engine already had the op for: each is checked against the library it comes
from on the same data."""

import datetime

import pytest

pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame, Series, col  # noqa: E402
from dftracer.utils._enums import DType  # noqa: E402


def _expr_frame():
    return DataFrame({"a": [1, 2, None, 4], "b": [4, 2, 2, None]}), {
        "a": [1, 2, None, 4],
        "b": [4, 2, 2, None],
    }


def _ours(df, expr):
    return df.select(expr.alias("r"))["r"].to_list()


class TestExprMethods:
    @pytest.mark.parametrize("name", ["lt", "le", "gt", "ge"])
    def test_comparisons_match_polars(self, name):
        pl = pytest.importorskip("polars")
        df, data = _expr_frame()
        pdf = pl.DataFrame(data)
        for other in (2, "b"):
            ours = getattr(col("a"), name)(col(other) if other == "b" else other)
            theirs = getattr(pl.col("a"), name)(pl.col(other) if other == "b" else other)
            assert _ours(df, ours) == pdf.select(theirs.alias("r"))["r"].to_list()

    @pytest.mark.parametrize("name", ["add", "sub", "mul"])
    def test_arithmetic_matches_polars(self, name):
        pl = pytest.importorskip("polars")
        df, data = _expr_frame()
        pdf = pl.DataFrame(data)
        for other in (3, "b"):
            ours = getattr(col("a"), name)(col(other) if other == "b" else other)
            theirs = getattr(pl.col("a"), name)(pl.col(other) if other == "b" else other)
            assert _ours(df, ours) == pdf.select(theirs.alias("r"))["r"].to_list()

    def test_truediv_and_neg_match_polars(self):
        pl = pytest.importorskip("polars")
        df, data = _expr_frame()
        pdf = pl.DataFrame(data)
        assert (
            _ours(df, col("a").truediv(2))
            == pdf.select(pl.col("a").truediv(2).alias("r"))["r"].to_list()
        )
        assert _ours(df, col("a").neg()) == pdf.select(pl.col("a").neg().alias("r"))["r"].to_list()

    def test_logic_matches_polars(self):
        pl = pytest.importorskip("polars")
        data = {"p": [True, False, True, None], "q": [True, True, False, False]}
        df, pdf = DataFrame(data), pl.DataFrame(data)
        for name in ("and_", "or_"):
            assert (
                _ours(df, getattr(col("p"), name)(col("q")))
                == pdf.select(getattr(pl.col("p"), name)(pl.col("q")).alias("r"))["r"].to_list()
            )
        assert (
            _ours(df, col("p").not_()) == pdf.select(pl.col("p").not_().alias("r"))["r"].to_list()
        )


class TestSeriesNames:
    def test_polars_spellings_match_polars(self):
        pl = pytest.importorskip("polars")
        data = [1.5, None, float("nan"), 4.0]
        ours, theirs = Series(data).rename("v"), pl.Series("v", data)
        assert ours.alias("w").name == theirs.alias("w").name == "w"
        assert ours.limit(2).to_list() == [1.5, None] == theirs.limit(2).to_list()
        assert ours.clone().to_list()[0] == theirs.clone().to_list()[0] == 1.5
        assert ours.has_nulls() is True and theirs.has_nulls() is True
        assert Series([1, 2]).has_nulls() is False and pl.Series([1, 2]).has_nulls() is False
        assert ours.is_empty() is False and Series([]).is_empty() is True
        assert ours.first() == theirs.first() == 1.5
        assert ours.last() == 4.0 == theirs.last()
        assert Series([]).first() is None and Series([]).last() is None
        assert ours.is_not_nan().to_list() == theirs.is_not_nan().to_list()

    def test_pandas_spellings_match_pandas(self):
        pd = pytest.importorskip("pandas")
        one, many = Series([7]), Series([7, 8])
        assert one.squeeze() == pd.Series([7]).squeeze() == 7
        assert many.squeeze() is many
        assert many.T is many and many.transpose() is many
        assert many.dtypes == DType.INT64
        assert many.convert_dtypes().to_list() == many.infer_objects().to_list() == [7, 8]

    def test_diff_periods_match_pandas(self):
        pd = pytest.importorskip("pandas")
        data = [1, None, 3, 7, 12, 20]
        ours, theirs = Series(data), pd.Series(data, dtype="Int64")
        for n in (1, 2, -1, 3):
            got = ours.diff(n).to_list()
            ref = [None if v is pd.NA else int(v) for v in theirs.diff(n).tolist()]
            assert got == ref, n
        assert ours.diff(periods=2).to_list() == ours.diff(2).to_list()

    def test_dt_names_match_pandas(self):
        pd = pytest.importorskip("pandas")
        when = [datetime.datetime(2020, 1, 1, 3, 4, 5), None, datetime.datetime(2021, 7, 15)]
        ours = Series.from_arrow(pa.array(when, pa.timestamp("us")))
        theirs = pd.Series(pa.array(when, pa.timestamp("us")).to_pandas())
        want_day = [None if v != v else v for v in theirs.dt.day_name().tolist()]
        want_month = [None if v != v else v for v in theirs.dt.month_name().tolist()]
        assert ours.dt.day_name().to_list() == want_day
        assert ours.dt.month_name().to_list() == want_month


class TestFrameNames:
    def _frame(self):
        return DataFrame({"a": [1, 2, 3], "b": ["x", "y", "z"]})

    def test_clone_clear_iter_columns(self):
        pl = pytest.importorskip("polars")
        ours, theirs = self._frame(), pl.DataFrame({"a": [1, 2, 3], "b": ["x", "y", "z"]})
        assert ours.clone().to_dict() == theirs.clone().to_dict(as_series=False)
        assert ours.clear().shape == theirs.clear().shape == (0, 2)
        assert ours.clear().columns == ["a", "b"]
        assert [c.to_list() for c in ours.iter_columns()] == [
            c.to_list() for c in theirs.iter_columns()
        ]
        with pytest.raises(ValueError, match="n=0"):
            ours.clear(1)
        assert list(ours.collect_schema()) == list(theirs.collect_schema()) == ["a", "b"]

    def test_pandas_container_spellings(self):
        ours = self._frame()
        assert ours.convert_dtypes().to_dict() == ours.infer_objects().to_dict() == ours.to_dict()

    def test_drop_takes_axis_like_pandas(self):
        pd = pytest.importorskip("pandas")
        ours = self._frame()
        pdf = pd.DataFrame({"a": [1, 2, 3], "b": ["x", "y", "z"]})
        for axis in (1, "columns"):
            assert ours.drop("a", axis=axis).columns == list(pdf.drop("a", axis=axis).columns)
        assert ours.drop(["a"], axis=1).columns == ["b"]
        assert ours.drop("a", axis=None).columns == ["b"]
        for bad in (0, "index", True, 2, "rows"):
            with pytest.raises(ValueError, match="axis must be"):
                ours.drop("a", axis=bad)


class TestLazyFrameNames:
    def _plan(self):
        return DataFrame({"a": [1, 2, 3], "b": [1.5, 2.5, None]}).lazy()

    def test_schema_names_match_polars(self):
        pl = pytest.importorskip("polars")
        ours = self._plan()
        theirs = pl.LazyFrame({"a": [1, 2, 3], "b": [1.5, 2.5, None]})
        assert ours.width == len(theirs.collect_schema()) == 2
        assert list(ours.collect_schema()) == list(theirs.collect_schema()) == ["a", "b"]
        assert ours.dtypes == [DType.INT64, DType.FLOAT64]
        assert len(theirs.collect_schema().dtypes()) == 2

    def test_rows_and_identity(self):
        pl = pytest.importorskip("polars")
        ours = self._plan()
        theirs = pl.LazyFrame({"a": [1, 2, 3], "b": [1.5, 2.5, None]})
        assert ours.lazy() is ours and ours.clone() is ours
        assert ours.first().collect().to_dict() == theirs.first().collect().to_dict(as_series=False)
        assert ours.last().collect().to_dict() == theirs.last().collect().to_dict(as_series=False)
        assert ours.clear().collect().shape == theirs.clear().collect().shape == (0, 2)
        with pytest.raises(ValueError, match="n=0"):
            ours.clear(2)
        assert ours.pipe(lambda lf, k: lf.head(k), 1).collect().shape == (1, 2)
