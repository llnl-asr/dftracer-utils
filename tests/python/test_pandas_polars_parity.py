"""The pandas and polars spellings on DataFrame / Series / LazyFrame, and the
``dftracer.utils.pandas`` / ``dftracer.utils.polars`` shim modules. Every
alias is checked against the engine op it forwards to, by value."""

import pytest

pa = pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame, Series, col  # noqa: E402
from dftracer.utils import pandas as upd  # noqa: E402
from dftracer.utils import polars as upl  # noqa: E402
from dftracer.utils.enums import DType  # noqa: E402


def _df(mapping):
    return DataFrame.from_arrow(pa.table(mapping))


def _dict(df):
    return df.to_arrow().to_pydict()


def _lst(s):
    return s.to_arrow().to_pylist()


def test_constructors_take_python_data():
    df = DataFrame({"a": [1, 2, 3], "b": ["x", "y", "z"]})
    assert _dict(df) == {"a": [1, 2, 3], "b": ["x", "y", "z"]}
    s = Series([3, 1, 2])
    assert _lst(s) == [3, 1, 2]
    assert isinstance(Series(s._native), Series)


def test_dataframe_properties_and_indexing():
    df = _df({"a": [1, 2, 3], "b": [1.5, None, 3.5]})
    assert df.columns == ["a", "b"]
    assert df.shape == (3, 2)
    assert df.height == 3 and df.width == 2
    assert df.dtypes == [DType.INT64, DType.FLOAT64]
    assert df.schema == {"a": DType.INT64, "b": DType.FLOAT64}
    assert _dict(df[["b", "a"]]) == {"b": [1.5, None, 3.5], "a": [1, 2, 3]}
    assert _dict(df[df["a"] > 1]) == {"a": [2, 3], "b": [None, 3.5]}
    assert _dict(df.limit(1)) == _dict(df.head(1)) == {"a": [1], "b": [1.5]}
    assert _dict(df.head()) == _dict(df)
    assert df.to_dict() == {"a": [1, 2, 3], "b": [1.5, None, 3.5]}


def test_dataframe_pandas_spellings_match_native():
    df = _df({"a": [3, 1, 2, 1], "b": [1.5, None, 3.5, 4.0], "c": ["x", "y", "x", "z"]})
    assert _dict(df.dropna()) == _dict(df.drop_nulls())
    assert _dict(df.fillna(0)) == _dict(df.fill_null(0))
    assert _lst(df.duplicated()) == _lst(df.is_duplicated())
    assert _dict(df.drop("b")) == _dict(df.select("a", "c"))
    assert _dict(df.drop(columns=["a", "b"])) == {"c": ["x", "y", "x", "z"]}
    with pytest.raises(KeyError):
        df.drop("nope")
    assert _dict(df.rename(columns={"a": "A"})) == _dict(df.rename({"a": "A"}))
    assert _dict(df.nlargest(2, "a")) == _dict(df.topk("a", 2))
    assert _dict(df.nsmallest(2, "a")) == _dict(df.topk("a", 2, largest=False))
    assert _dict(df.sort_values("a")) == _dict(df.sort_by("a"))
    assert _dict(df.sort_values(["a", "b"], ascending=[False, True])) == _dict(
        df.sort_by_multi(["a", "b"], descending=[True, False])
    )
    assert _dict(df.gather(Series([0, 0]))) == _dict(df.take(Series([0, 0])))
    idx = df.with_row_index()
    assert idx.columns[0] == "index"
    assert _dict(df.sample(2, random_state=7)) == _dict(df.sample(2, seed=7))


def test_dataframe_assign_cast_and_filter_expr():
    df = _df({"a": [1, 2, 3], "s": ["x", "yy", "zzz"]})
    out = df.assign(twice=col("a") * 2, n=df["s"].str.len())
    assert _dict(out) == {
        "a": [1, 2, 3],
        "s": ["x", "yy", "zzz"],
        "twice": [2, 4, 6],
        "n": [1, 2, 3],
    }
    assert _dict(df.with_columns(twice=col("a") * 2)) == _dict(df.assign(twice=col("a") * 2))
    casted = df.astype({"a": "float64"})
    assert casted.dtypes[0] == DType.FLOAT64
    assert _dict(casted) == {"a": [1.0, 2.0, 3.0], "s": ["x", "yy", "zzz"]}
    assert _dict(df.cast({"a": DType.FLOAT64})) == _dict(casted)
    assert _dict(df.filter(col("a") >= 2)) == _dict(df.filter(df["a"] >= 2))
    assert _dict(df.filter(col("s").contains("z"))) == {"a": [3], "s": ["zzz"]}


def test_dataframe_reshape_spellings():
    long = _df({"k": ["a", "a", "b"], "on": ["x", "y", "x"], "v": [1, 2, 3]})
    assert _dict(long.pivot(index="k", on="on", values="v")) == _dict(long.pivot("k", "on", "v"))
    assert _dict(long.pivot_table(values="v", index="k", columns="on", aggfunc="sum")) == _dict(
        long.pivot("k", "on", "v", "sum")
    )
    wide = _df({"id": [1, 2], "x": [10, 20], "y": [30, 40]})
    assert _dict(wide.unpivot(index="id", on=["x", "y"])) == _dict(wide.unpivot("id", ["x", "y"]))
    assert _dict(wide.melt(id_vars="id", value_vars=["x", "y"])) == _dict(
        wide.unpivot("id", ["x", "y"])
    )
    assert _dict(wide.groupby("id", "count")) == _dict(wide.group_by("id", "count"))


def test_series_properties_and_pandas_spellings():
    s = _df({"v": [3, None, 1, 3]})["v"]
    assert s.dtype == DType.INT64
    assert s.size == 4 and s.shape == (4,) and s.len() == 4
    assert s.to_list() == [3, None, 1, 3] == s.tolist()
    assert _lst(s.isna()) == [False, True, False, False] == _lst(s.is_null())
    assert _lst(s.notna()) == [True, False, True, True] == _lst(s.is_not_null())
    assert s.is_null(1) is True and s.is_null(0) is False
    assert _lst(s.isin([3])) == [True, False, False, True] == _lst(s.is_in([3]))
    assert _lst(s.dropna()) == _lst(s.drop_nulls()) == [3, 1, 3]
    assert _lst(s.fill_null(0)) == _lst(s.fillna(0)) == [3, 0, 1, 3]
    assert s.n_unique() == s.nunique()
    assert s.prod() == s.product()
    assert s.argmin() == s.idxmin() == s.arg_min()
    assert s.argmax() == s.idxmax() == s.arg_max()
    assert _lst(s.cumprod()) == _lst(s.cum_prod())
    assert _lst(s.cum_sum()) == _lst(s.cumsum())
    assert _lst(s.cum_max()) == _lst(s.cummax())
    assert _lst(s.cum_min()) == _lst(s.cummin())
    assert _lst(s.sort_values()) == _lst(s.sort())
    assert _lst(s.sort_values(ascending=False)) == _lst(s.sort(descending=True))
    assert _lst(s.nlargest(2)) == _lst(s.top_k(2))
    assert _lst(s.nsmallest(2)) == _lst(s.bottom_k(2))
    assert _lst(s.arg_sort()) == _lst(s.argsort())
    assert _lst(s.gather(Series([0, 2]))) == _lst(s.take(Series([0, 2]))) == [3, 1]
    assert _lst(s.head()) == _lst(s)
    assert _lst(s.shift(periods=1)) == _lst(s.shift(1))
    assert _lst(s.sample(2, random_state=3)) == _lst(s.sample(2, seed=3))


def test_series_statistics_and_windows():
    s = Series([1.0, 2.0, 4.0, 8.0])
    assert s.var() == s.variance() == s.var(ddof=1)
    assert s.var(ddof=0) == s.variance(sample=False)
    assert s.std(ddof=0) == s.stddev(sample=False)
    with pytest.raises(ValueError):
        s.var(ddof=2)
    assert s.skew() == s.skewness()
    assert s.kurt() == s.kurtosis()
    assert _lst(s.rolling(2).mean()) == _lst(s.rolling(2, "mean")) == _lst(s.rolling_mean(2))
    assert _lst(s.rolling(2).sum()) == _lst(s.rolling_sum(2))
    assert _lst(s.rolling(2).std()) == _lst(s.rolling_std(2))
    assert _lst(s.rolling(3).quantile(0.5)) == _lst(s.rolling_quantile(3, 0.5))
    assert _lst(s.ewm(0.5).mean()) == _lst(s.ewm_mean(0.5))
    assert _lst(s.ewm(0.5).std()) == _lst(s.ewm_std(0.5))
    assert _lst(s.rank(ascending=False)) == _lst(s.rank(descending=True))
    assert _lst(s.clip(lower=2, upper=4)) == _lst(s.clip(2, 4)) == [2.0, 2.0, 4.0, 4.0]
    assert _lst(s.clip(lower_bound=2, upper_bound=4)) == [2.0, 2.0, 4.0, 4.0]
    with pytest.raises(TypeError):
        s.clip(lower=2)
    with pytest.raises(NotImplementedError):
        s.round(2)


def test_series_str_accessor():
    s = Series([" Read ", "pread64", "x"])
    assert _lst(s.str.lower()) == _lst(s.to_lowercase()) == _lst(s.str.to_lowercase())
    assert _lst(s.str.upper()) == _lst(s.to_uppercase())
    assert _lst(s.str.strip()) == _lst(s.str_strip()) == _lst(s.str.strip_chars())
    assert _lst(s.str.len()) == _lst(s.str_len_chars()) == _lst(s.str.len_chars())
    assert _lst(s.str.len_bytes()) == _lst(s.str_len_bytes())
    assert _lst(s.str.contains("ead")) == [True, True, False]
    assert _lst(s.str.contains("EAD", case=False)) == [True, True, False]
    assert _lst(s.str.contains("^p", regex=True)) == [False, True, False]
    assert _lst(s.str.startswith("p")) == _lst(s.str.starts_with("p")) == [False, True, False]
    assert _lst(s.str.endswith("64")) == _lst(s.str.ends_with("64"))
    assert _lst(s.str.match("p")) == [False, True, False]
    assert _lst(s.str.fullmatch("x")) == [False, False, True]
    assert _lst(s.str.find("ead")) == _lst(s.str_find("ead"))
    assert _lst(s.str.replace("e", "E")) == _lst(s.str_replace_all("e", "E"))
    assert _lst(s.str.replace("e", "E", n=1)) == _lst(s.str_replace("e", "E"))
    assert _lst(s.str.replace_all("e", "E")) == _lst(s.str_replace_all("e", "E"))
    assert _lst(s.str.slice(1, 3)) == _lst(s.str_slice(1, 2)) == ["Re", "re", ""]
    assert _lst(s.str.pad(8, fillchar="-")) == _lst(s.str_pad_start(8, "-"))
    assert _lst(s.str.pad(8, side="right")) == _lst(s.str.pad_end(8))
    assert _lst(s.str.zfill(8)) == _lst(s.str_zfill(8))


def test_lazyframe_polars_spellings():
    df = _df({"a": [3, 1, 2], "b": [1, 2, 3], "s": ["x", "y", "z"]})
    lf = df.lazy()
    assert _dict(lf.with_columns(c=col("a") + col("b"), d=col("a") * 2).collect()) == _dict(
        df.assign(c=col("a") + col("b"), d=col("a") * 2)
    )
    assert _dict(lf.select(["a", "s"]).collect()) == _dict(df.select("a", "s"))
    assert _dict(lf.drop("b").collect()) == _dict(df.drop("b"))
    with pytest.raises(KeyError):
        lf.drop("nope")
    assert _dict(lf.rename({"a": "A"}).collect()) == _dict(df.rename({"a": "A"}))
    assert _dict(lf.rename(["x", "y", "z"]).collect()) == _dict(
        df.rename({"a": "x", "b": "y", "s": "z"})
    )
    assert _dict(lf.limit(2).collect()) == _dict(df.head(2))
    assert _dict(lf.sort("a").collect()) == _dict(df.sort_by("a"))
    assert _dict(lf.sort(["a", "b"], descending=True).collect()) == _dict(
        df.sort_by_multi(["a", "b"], descending=True)
    )
    assert _dict(lf.sort_values("a", ascending=False).collect()) == _dict(df.sort_by("a", True))
    assert _dict(lf.gather([2, 0]).collect()) == _dict(df.take(Series([2, 0])))
    assert _dict(lf.filter(col("s").is_in(["x", "z"])).collect()) == {
        "a": [3, 2],
        "b": [1, 3],
        "s": ["x", "z"],
    }


def test_pandas_shim_module(tmp_path):
    df = upd.DataFrame({"k": [1, 2, 2], "v": [10, 20, 30]})
    assert isinstance(df, DataFrame)
    right = upd.DataFrame({"k": [2], "name": ["two"]})
    merged = upd.merge(df, right, how="left", on="k")
    assert _dict(merged) == {"k": [1, 2, 2], "v": [10, 20, 30], "name": [None, "two", "two"]}
    assert _dict(upd.merge(df, right, how="left")) == _dict(merged)
    both = upd.concat([df, df])
    assert both.height == 6
    assert _dict(upd.get_dummies(df, "k")) == _dict(df.to_dummies("k"))
    s = upd.Series([1, None])
    assert _lst(upd.isna(s)) == [False, True] and _lst(upd.notnull(s)) == [True, False]
    path = str(tmp_path / "t.parquet")
    import pyarrow.parquet as pq

    pq.write_table(df.to_arrow(), path)
    assert _dict(upd.read_parquet(path, columns=["v"])) == {"v": [10, 20, 30]}
    with pytest.raises(ValueError):
        upd.concat([])


def test_polars_shim_module(tmp_path):
    df = upl.DataFrame({"k": [1, 2, 2], "v": [10, 20, 30]})
    out = df.lazy().filter(upl.col("v") > upl.lit(15)).select("k").collect()
    assert _dict(out) == {"k": [2, 2]}
    assert isinstance(out, upl.DataFrame)
    path = str(tmp_path / "t.parquet")
    import pyarrow.parquet as pq

    pq.write_table(df.to_arrow(), path)
    lf = upl.scan_parquet(path)
    assert isinstance(lf, upl.LazyFrame)
    assert _dict(lf.sort("v", descending=True).limit(1).collect()) == {"k": [2], "v": [30]}
    assert _dict(upl.read_parquet(path)) == _dict(df)
    assert upl.concat([df, df], how="diagonal").height == 6
    lazy_cat = upl.concat([df.lazy(), df.lazy()])
    assert isinstance(lazy_cat, upl.LazyFrame)
    assert _dict(lazy_cat.collect()) == _dict(df.concat(df))
    with pytest.raises(ValueError, match="vertical"):
        upl.concat([df.lazy(), df.lazy()], how="diagonal")


def test_unique_subset_eager_and_lazy():
    df = _df({"k": [1, 1, 2, 2, 3], "v": [1, 2, 3, 3, 4]})
    assert _dict(df.unique()) == {"k": [1, 1, 2, 3], "v": [1, 2, 3, 4]}
    by_k = {"k": [1, 2, 3], "v": [1, 3, 4]}
    assert _dict(df.unique("k")) == by_k
    assert _dict(df.unique(subset=["k"])) == by_k
    assert _dict(df.drop_duplicates(subset="k")) == by_k
    assert _dict(df.distinct("k")) == by_k
    lazy = df.lazy().unique(subset="k")
    assert "unique [k]" in lazy.explain()
    assert _dict(lazy.collect()) == by_k
    assert _dict(lazy.collect(morsel_rows=2)) == by_k
    assert _dict(df.lazy().drop_duplicates("v").collect()) == {"k": [1, 1, 2, 3], "v": [1, 2, 3, 4]}
    with pytest.raises(KeyError):
        df.unique("nope")
    with pytest.raises(ValueError):
        df.lazy().unique("nope")


def test_reduce_family_eager_lazy_and_grouped():
    df = _df({"k": ["x", "y", "x"], "s": ["a", None, "c"], "a": [1, 2, 3], "b": [2.0, 4.0, 6.5]})
    assert _dict(df.sum()) == {"a": [6], "b": [12.5]}
    assert _dict(df.max()) == {"a": [3], "b": [6.5]}
    assert _dict(df.mean()) == {"a": [2.0], "b": [12.5 / 3]}
    assert _dict(df.count()) == {"k": [3], "s": [2], "a": [3], "b": [3]}
    assert _dict(df.group_by("k").sum()) == {"k": ["x", "y"], "a": [4, 2], "b": [8.5, 4.0]}
    assert _dict(df.group_by("k").count()) == {
        "k": ["x", "y"],
        "s": [2, 0],
        "a": [2, 1],
        "b": [2, 1],
    }
    assert _dict(df.group_by("k").reduce("min")) == {"k": ["x", "y"], "a": [1, 2], "b": [2.0, 4.0]}
    assert _dict(df.group_by("k").size()) == {"k": ["x", "y"], "size": [2, 1]}
    # The whole frame as one group takes any aggregate, corr included.
    whole = df.group_by().agg(col("b").corr(col("a")).alias("c"), col("a").sum().alias("sa"))
    assert _dict(whole)["sa"] == [6]
    assert abs(_dict(whole)["c"][0] - 0.9979487157886735) < 1e-12
    assert _dict(df.group_by(None, "sum:a", "max:b")) == {"sum_a": [6], "max_b": [6.5]}
    # Lazy: a streaming group-by.
    lazy = df.lazy().group_by("k").sum()
    assert lazy.schema() == ["k", "a", "b"]
    assert _dict(lazy.collect(morsel_rows=1)) == _dict(df.group_by("k").sum())
    assert _dict(df.lazy().mean().collect()) == _dict(df.mean())
    assert _dict(df.lazy().group_by(None, "sum:a").collect()) == {"sum_a": [6]}
    assert _dict(df.lazy().group_by("k").size().collect()) == _dict(df.group_by("k").size())
    with pytest.raises(ValueError, match="second column"):
        df.reduce("corr")
    with pytest.raises(ValueError, match="second column"):
        df.group_by("k").reduce("corr")


def test_pandas_agg_forms():
    df = _df({"k": ["x", "y", "x"], "s": ["a", None, "c"], "a": [1, None, 3], "b": [2.0, 4.0, 6.5]})
    # axis=0, whole frame: a name, a list, a dict, keywords, expressions.
    assert _dict(df.agg("sum")) == {"a": [4], "b": [12.5]}
    assert _dict(df.agg(["sum", "max"])) == {
        "a_sum": [4],
        "b_sum": [12.5],
        "a_max": [3],
        "b_max": [6.5],
    }
    assert _dict(df.agg({"a": ["sum", "max"], "b": "mean"})) == {
        "a_sum": [4],
        "a_max": [3],
        "b": [12.5 / 3],
    }
    assert _dict(df.agg(a_max=("a", "max"), n=("b", "count"))) == {"a_max": [3], "n": [3]}
    assert _dict(df.aggregate(total=col("a").sum())) == {"total": [4]}
    # Grouped: the same forms on the group-by object.
    gb = df.group_by("k")
    assert _dict(gb.agg("min")) == {"k": ["x", "y"], "a": [1, None], "b": [2.0, 4.0]}
    assert _dict(gb.agg({"a": "sum", "b": ["min", "max"]})) == {
        "k": ["x", "y"],
        "a": [4, 0],
        "b_min": [2.0, 4.0],
        "b_max": [6.5, 4.0],
    }
    named = _dict(gb.agg(total=("a", "sum"), c=col("b").corr(col("a"))))
    assert named["total"] == [4, 0]
    # An empty group has no min, mean or variance: null, not the seed.
    assert _dict(gb.mean())["a"] == [2.0, None]
    assert _dict(gb.var())["b"] == [10.125, None]
    # axis=1: one value per row across the numeric columns, nulls skipped.
    assert df.agg("sum", axis=1).to_list() == [3.0, 4.0, 9.5]
    assert df.agg("mean", axis=1).to_list() == [1.5, 4.0, 4.75]
    assert df.agg("min", axis=1).to_list() == [1.0, 4.0, 3.0]
    assert df.agg("max", axis=1).to_list() == [2.0, 4.0, 6.5]
    assert df.agg("count", axis=1).to_list() == [2, 1, 2]
    ints = _df({"a": [1, None], "b": [3, 5]})
    assert ints.agg("min", axis=1).to_list() == [1, 5]
    assert ints.agg("sum", axis=1).to_list() == [4, 5]
    with pytest.raises(ValueError, match="sum, mean, min, max and count"):
        df.agg("var", axis=1)
    # The pandas names whose engine form is a sketch are refused, not approximated.
    with pytest.raises(ValueError, match="sketch"):
        df.agg("median")
    with pytest.raises(ValueError, match="sketch"):
        gb.agg({"a": "nunique"})
    # Lazy: the same forms, each a streaming group-by step.
    lf = df.lazy()
    assert _dict(lf.agg(["sum", "max"]).collect()) == _dict(df.agg(["sum", "max"]))
    assert _dict(
        lf.group_by("k").agg({"a": "sum"}, total=("b", "sum"), n=col("a").sum()).collect()
    ) == {
        "k": ["x", "y"],
        "a": [4, 0],
        "total": [8.5, 4.0],
        "n": [4, 0],
    }
    # A two-column aggregate streams too (the spec string carries `by`).
    assert _dict(lf.group_by("k").agg(col("b").corr(col("a")).alias("r")).collect()) == _dict(
        df.group_by("k").agg(col("b").corr(col("a")).alias("r"))
    )
    # A Bool column casts to a number (the count above leans on it).
    assert df["a"].is_not_null().astype("int64").to_list() == [1, 0, 1]
    assert df["a"].is_not_null().astype("float64").to_list() == [1.0, 0.0, 1.0]
    with pytest.raises(ValueError, match="String column"):
        df.group_by("k").agg(col("s").mean().alias("m"))


def test_groupby_transforms_match_pandas():
    pd = pytest.importorskip("pandas")
    t = pa.table(
        {"k": ["y", "x", "x", "y", "x"], "v": [5, 1, None, 7, 3], "s": ["b", "a", "c", "a", "b"]}
    )
    df = DataFrame.from_arrow(t)
    gb = df.group_by("k")
    pg = t.to_pandas().groupby("k")

    def same(ours, ref):
        got = ours.to_arrow().to_pydict()
        for name, values in got.items():
            exp = ref[name] if isinstance(ref, pd.DataFrame) else ref
            exp_list = [None if pd.isna(x) else x for x in exp.tolist()]
            assert (
                values == pytest.approx(exp_list)
                if all(isinstance(x, float) for x in exp_list if x is not None)
                else values == exp_list
            ), name

    same(gb.cumsum(), pg["v"].cumsum())
    same(gb.cummax(), pg["v"].cummax())
    same(gb.cummin(), pg["v"].cummin())
    same(gb.cumcount(), pg.cumcount())
    same(gb.shift(1), pg.shift(1))
    same(gb.shift(-1), pg.shift(-1))
    same(gb.diff(), pg["v"].diff())
    same(gb.pct_change(), pg["v"].pct_change(fill_method=None))
    same(gb.rank(), pg["v"].rank())
    same(gb.rank("min"), pg["v"].rank(method="min"))
    same(gb.rank("dense", ascending=False), pg["v"].rank(method="dense", ascending=False))
    same(gb.rank("first"), pg["v"].rank(method="first"))
    same(gb.ngroup(), pg.ngroup())
    same(gb.head(1), pg.head(1))
    same(gb.tail(1), pg.tail(1))
    same(gb.nth(1), pg.nth(1))
    same(gb.nth(-1), pg.nth(-1))
    same(df.group_by(["k", "s"]).cumcount(), t.to_pandas().groupby(["k", "s"]).cumcount())
    # The lazy group-by builds the same plan.
    lazy = df.lazy().group_by("k")
    assert lazy.cumsum().collect().to_arrow().to_pydict() == gb.cumsum().to_arrow().to_pydict()
    assert lazy.head(1).collect().to_arrow().to_pydict() == gb.head(1).to_arrow().to_pydict()
    assert "frame_op dftu.frame.window" in lazy.rank().explain()
    same(gb.rank("max"), pg["v"].rank(method="max"))
    with pytest.raises(ValueError, match="method"):
        gb.rank("median")
    with pytest.raises(ValueError, match="no keys"):
        df.group_by().ngroup()

    # Ties within a group: min and dense share a rank, first does not.
    tied = pa.table({"k": ["x", "y", "x", "y", "x"], "b": [1.0, 5.0, 2.0, 5.0, 0.5]})
    tdf = DataFrame.from_arrow(tied)
    tg = tdf.group_by("k")
    tp = tied.to_pandas().groupby("k")["b"]
    for method in ("average", "min", "max", "dense", "first"):
        for ascending in (True, False):
            got = tg.rank(method, ascending=ascending).to_arrow().to_pydict()["b"]
            assert got == tp.rank(method=method, ascending=ascending).tolist(), (
                method,
                ascending,
            )
    assert tg.rank("min").to_arrow().to_pydict()["b"] == [2.0, 1.0, 3.0, 1.0, 1.0]
    assert (
        tdf.lazy().group_by("k").rank("dense").collect().to_arrow().to_pydict()["b"]
        == tg.rank("dense").to_arrow().to_pydict()["b"]
    )


def test_pandas_semantics_rulings():
    # merge: no key joins on EVERY shared column (so k and v here, as pandas
    # does); collisions off the key are suffixed on both sides with
    # suffixes=("_x", "_y").
    left = DataFrame({"k": [1, 2], "v": [10, 20]})
    right = DataFrame({"k": [2, 3], "v": [200, 300]})
    assert _dict(left.merge(right)) == {"k": [], "v": []}
    assert _dict(left.merge(DataFrame({"k": [2], "w": [7]}))) == {"k": [2], "v": [20], "w": [7]}
    assert _dict(left.merge(right, on="k")) == {"k": [2], "v_x": [20], "v_y": [200]}
    assert _dict(left.merge(right, on="k", suffixes=("_l", "_r"), how="outer")) == {
        "k": [1, 2, 3],
        "v_l": [10, 20, None],
        "v_r": [None, 200, 300],
    }
    with pytest.raises(ValueError):
        DataFrame({"a": [1]}).merge(DataFrame({"b": [1]}))
    # join keeps the polars shape: one right-side suffix.
    assert _dict(left.join(right, on="k")) == {"k": [2], "v": [20], "v_right": [200]}

    # filter: a mask selects rows; items / like / regex select columns.
    df = DataFrame({"ab": [1, 2], "ac": [3, 4], "d": [5, 6]})
    assert _dict(df.filter(items=["d", "ab"])) == {"d": [5, 6], "ab": [1, 2]}
    assert _dict(df.filter(like="a")) == {"ab": [1, 2], "ac": [3, 4]}
    assert _dict(df.filter(regex="c$")) == {"ac": [3, 4]}
    assert _dict(df.filter(df["d"] > 5)) == {"ab": [2], "ac": [4], "d": [6]}
    with pytest.raises(TypeError):
        df.filter(like="a", regex="b")
    with pytest.raises(TypeError):
        df.filter()

    # Series.compare diffs two Series; is_unique is a bool; mode is a Series.
    a = Series([1, 2, 3, 4])
    b = Series([1, 9, 3, 8])
    assert _dict(a.compare(b)) == {"index": [1, 3], "self": [2, 4], "other": [9, 8]}
    assert _dict(a.compare(b, keep_shape=True)) == {
        "index": [0, 1, 2, 3],
        "self": [None, 2, None, 4],
        "other": [None, 9, None, 8],
    }
    assert _dict(a.compare(b, keep_shape=True, keep_equal=True)) == {
        "index": [0, 1, 2, 3],
        "self": [1, 2, 3, 4],
        "other": [1, 9, 3, 8],
    }
    with pytest.raises(TypeError):
        a.compare(1)
    assert a.is_unique is True
    assert Series([1, 1]).is_unique is False
    assert Series([None, None, 1]).is_unique is False
    assert _lst(Series([1, 1, 2]).unique_mask()) == [False, False, True]
    assert _lst(Series([5, 1, 9, 1, 9]).mode()) == [1]
    assert _lst(a.ops.series.compare(0, 2)) == [False, False, True, True]

    # value_counts keeps its frame shape and takes the pandas arguments.
    s = Series([1, 1, 2, 3, 3, 3])
    vc = s.value_counts()
    assert _dict(vc) == {"value": [3, 1, 2], "count": [3, 2, 1]}
    assert _dict(s.value_counts(ascending=True)) == {"value": [2, 1, 3], "count": [1, 2, 3]}
    assert _dict(s.value_counts(normalize=True)) == {
        "value": [3, 1, 2],
        "proportion": [0.5, 1 / 3, 1 / 6],
    }
