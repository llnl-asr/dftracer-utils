"""The polars spellings: the expression column ops (``cum_sum``,
``rolling_mean``, ``sort``, ``rank``, ``over``, ``.str`` / ``.dt``), an
aggregate's ``over``, and the frame names, each checked against polars on
the same data, eagerly and as a plan."""

import math

import pytest

pa = pytest.importorskip("pyarrow")
pl = pytest.importorskip("polars")

from dftracer.utils import DataFrame, Series, col  # noqa: E402


def _clean(values):
    return [None if isinstance(v, float) and math.isnan(v) else v for v in values]


def _frame():
    return DataFrame(
        {
            "k": ["a", "b", "a", "b", "a", "b"],
            "x": [3.0, 1.0, None, 4.0, 2.0, 6.0],
            "n": [1, 2, 3, 4, 5, 6],
            "s": ["ab", "b", None, "dd", "e", "ff"],
            "t": [0, 3_600_000_000, 90_000_000_000, 1, 2, 86_400_000_000 * 40],
        }
    )


def _both(df, ours, theirs, approx=False):
    """The expression eagerly, as a plan, and in polars: one answer."""
    eager = _clean(df.apply(ours).to_list())
    lazy = _clean(df.lazy().with_column("r", ours).collect()["r"].to_list())
    ref = _clean(df.to_polars().select(theirs.alias("r"))["r"].to_list())
    if approx:
        assert eager == pytest.approx(ref) and lazy == pytest.approx(ref)
    else:
        assert eager == ref, (eager, ref)
        assert lazy == ref, (lazy, ref)


def test_expression_column_ops_match_polars():
    df = _frame()
    x, px = col("x"), pl.col("x")
    _both(df, x.cum_sum(), px.cum_sum())
    _both(df, x.cum_prod(), px.cum_prod())
    _both(df, x.cum_max(), px.cum_max())
    _both(df, x.cum_min(), px.cum_min())
    _both(df, x.cum_count(), px.cum_count())
    _both(df, x.shift(1), px.shift(1))
    _both(df, x.shift(-2), px.shift(-2))
    _both(df, x.diff(), px.diff())
    _both(df, x.pct_change(), px.pct_change())
    _both(df, x.rank(), px.rank())
    _both(df, x.rank("min", descending=True), px.rank("min", descending=True))
    _both(df, x.rank("dense"), px.rank("dense"))
    _both(df, x.forward_fill(), px.forward_fill())
    _both(df, x.backward_fill(), px.backward_fill())
    _both(df, x.fill_null(0.0), px.fill_null(0.0))
    _both(df, x.interpolate(), px.interpolate())
    _both(df, x.rolling_sum(2), px.rolling_sum(2))
    _both(df, x.rolling_mean(2), px.rolling_mean(2))
    _both(df, x.rolling_min(2), px.rolling_min(2))
    _both(df, x.rolling_max(2), px.rolling_max(2))
    _both(df, x.rolling_std(2), px.rolling_std(2), approx=True)
    _both(df, x.rolling_var(2), px.rolling_var(2), approx=True)
    _both(df, x.rolling_median(2), px.rolling_median(2))
    # ewm follows pandas on a null row (the value repeats); polars gives null.
    ewm = _clean(df.apply(x.ewm_mean(0.5)).to_list())
    assert ewm == pytest.approx(df.to_pandas()["x"].ewm(alpha=0.5).mean().tolist(), nan_ok=True)
    _both(df, x.sort(), px.sort(nulls_last=True))
    _both(df, x.sort(descending=True), px.sort(descending=True, nulls_last=True))
    _both(df, x.reverse(), px.reverse())
    _both(df, x.is_duplicated(), px.is_duplicated())
    _both(df, x.is_unique(), px.is_unique())
    _both(df, x.is_nan(), px.is_nan())
    _both(df, x.is_finite(), px.is_finite())
    _both(df, x.log10(), px.log10(), approx=True)
    _both(df, x.log1p(), px.log1p(), approx=True)
    # An op inside arithmetic, and one op fed to another.
    _both(df, x.cum_sum() * 2 + x.shift(1), px.cum_sum() * 2 + px.shift(1))
    _both(df, (x * 2).cum_sum().shift(1), (px * 2).cum_sum().shift(1))
    _both(df, x.cum_sum().rolling_mean(2), px.cum_sum().rolling_mean(2))
    # A column op inside a plan filter.
    got = df.lazy().filter(col("x").rank() > 2).collect()["n"].to_list()
    assert got == df.to_polars().filter(pl.col("x").rank() > 2)["n"].to_list()
    with pytest.raises(ValueError):
        x.rank("bogus")
    with pytest.raises(ValueError):
        x.rolling_sum(0)


def test_over_matches_polars():
    df = _frame()
    x, px = col("x"), pl.col("x")
    _both(df, x.cum_sum().over("k"), px.cum_sum().over("k"))
    _both(df, x.rank().over("k"), px.rank().over("k"))
    _both(df, x.shift(1).over("k"), px.shift(1).over("k"))
    _both(df, x.forward_fill().over("k"), px.forward_fill().over("k"))
    _both(df, x.diff().over("k"), px.diff().over("k"))
    _both(df, x.cum_max().over("k") - x, px.cum_max().over("k") - px)
    # An aggregate's over: the group value on every row of its group.
    _both(df, x.sum().over("k"), px.sum().over("k"))
    _both(df, x - x.mean().over("k"), px - px.mean().over("k"))
    _both(df, col("n").max().over(["k"]), pl.col("n").max().over(["k"]))
    with pytest.raises(TypeError, match="over"):
        (x + 1).over("k")
    with pytest.raises(TypeError, match="group-wise"):
        x.sort().over("k")


def test_str_and_dt_namespaces_match_polars():
    df = _frame()
    s, ps = col("s"), pl.col("s")
    _both(df, s.str.len_chars(), ps.str.len_chars())
    _both(df, s.str.len_bytes(), ps.str.len_bytes())
    _both(df, s.str.to_uppercase(), ps.str.to_uppercase())
    _both(df, s.str.contains("b"), ps.str.contains("b"))
    _both(df, s.str.starts_with("d"), ps.str.starts_with("d"))
    _both(df, s.str.pad_start(3, "_"), ps.str.pad_start(3, "_"))
    _both(df, s.str.pad_end(3, "."), ps.str.pad_end(3, "."))
    _both(df, s.str.zfill(3), ps.str.zfill(3))
    _both(df, s.str.replace("b", "X"), ps.str.replace("b", "X", literal=True))
    _both(df, s.str.slice(0, 1), ps.str.slice(0, 1))
    _both(df, s.str.strip_prefix("d"), ps.str.strip_prefix("d"))
    _both(df, s.str.count_matches("b"), ps.str.count_matches("b", literal=True))
    t = col("t")
    pt = pl.from_epoch(pl.col("t"), time_unit="us")
    for part in (
        "year",
        "month",
        "day",
        "hour",
        "minute",
        "second",
        "weekday",
        "ordinal_day",
        "quarter",
    ):
        _both(df, getattr(t.dt, part)(), getattr(pt.dt, part)())
    _both(df, t.dt.truncate("1h").cast("int64"), pt.dt.truncate("1h").dt.epoch("us"))
    _both(df, t.dt.epoch(), pt.dt.epoch("us"))


def test_frame_names_match_polars():
    df = DataFrame(
        {"k": ["a", "b", "a"], "x": [3.0, None, 1.0], "n": [1, 2, None], "s": ["p", "q", "r"]}
    )
    p = df.to_polars()
    assert (
        _clean(df.sum_horizontal().to_list())
        == p.select(pl.sum_horizontal("x", "n"))["x"].to_list()
    )
    assert (
        _clean(df.mean_horizontal().to_list())
        == p.select(pl.mean_horizontal("x", "n"))["x"].to_list()
    )
    assert (
        _clean(df.min_horizontal().to_list())
        == p.select(pl.min_horizontal("x", "n"))["x"].to_list()
    )
    assert (
        _clean(df.max_horizontal().to_list())
        == p.select(pl.max_horizontal("x", "n"))["x"].to_list()
    )
    assert (
        df.hstack({"z": Series([1, 2, 3])}).columns == p.hstack([pl.Series("z", [1, 2, 3])]).columns
    )
    assert df.vstack(df).shape == p.vstack(p).shape
    assert df.gather_every(2).to_dict() == p.gather_every(2).to_dict(as_series=False)
    assert df.gather_every(2, 1).to_dict() == p.gather_every(2, 1).to_dict(as_series=False)
    parts = df.partition_by("k", as_dict=True)
    ref = p.partition_by("k", as_dict=True, maintain_order=True)
    assert {k: v.to_dict() for k, v in parts.items()} == {
        k[0]: v.to_dict(as_series=False) for k, v in ref.items()
    }
    assert df.rows() == p.rows()
    assert df.rows(named=True) == p.rows(named=True)
    assert df.row(-1) == p.row(-1)
    assert df.item(0, "s") == p.item(0, "s")
    assert df.to_dicts() == p.to_dicts()
    assert df.select("n").head(1).item() == 1
    assert not df.is_empty() and df.head(0).is_empty()
    assert df.n_unique("k") == p.n_unique("k")
    assert df.get_column("s").to_list() == p.get_column("s").to_list()
    assert [c.to_list() for c in df.get_columns()] == [_clean(c.to_list()) for c in p.get_columns()]
    assert df.to_series(1).to_list() == _clean(p.to_series(1).to_list())
    nan = DataFrame({"a": [1.0, float("nan"), 3.0], "b": [1, 2, 3]})
    assert nan.fill_nan(0.0)["a"].to_list() == [1.0, 0.0, 3.0]
    assert nan.fill_nan(None)["a"].to_list() == [1.0, None, 3.0]
    assert nan.drop_nans()["b"].to_list() == [1, 3]
    left = DataFrame({"t": [1, 5, 10], "v": [1, 2, 3]})
    right = DataFrame({"t": [0, 4, 9], "w": [10, 20, 30]})
    assert df.join_asof.__doc__ is not None
    assert (
        left.join_asof(right, on="t")["w"].to_list()
        == left.to_polars().join_asof(right.to_polars(), on="t")["w"].to_list()
    )


def test_writers_round_trip(tmp_path):
    df = DataFrame({"a": [1, 2], "s": ["x", "y"]})
    df.write_parquet(str(tmp_path / "f.parquet"))
    df.write_csv(str(tmp_path / "f.csv"))
    df.write_ipc(str(tmp_path / "f.arrow"))
    import pyarrow.csv as pcsv
    import pyarrow.ipc as pipc
    import pyarrow.parquet as pq

    assert pq.read_table(str(tmp_path / "f.parquet")).to_pydict() == df.to_dict()
    assert pcsv.read_csv(str(tmp_path / "f.csv")).to_pydict() == df.to_dict()
    with pipc.open_stream(str(tmp_path / "f.arrow")) as reader:
        assert reader.read_all().to_pydict() == df.to_dict()
