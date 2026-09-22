"""The pandas surface added as compositions of engine ops (the frame, Series
and group-by mixins), each value checked against pandas 2.x on the same
data."""

import math

import pytest

pa = pytest.importorskip("pyarrow")
pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series, col  # noqa: E402

# include_groups= landed in pandas 2.2; before it, apply() forwards the keyword
# to the function.
_PG_APPLY_KW = (
    {"include_groups": False}
    if tuple(int(p) for p in pd.__version__.split(".")[:2]) >= (2, 2)
    else {}
)


def _pa_chunked(series):
    """The pyarrow-backed array of an arrow-dtype pandas Series; pandas renamed
    the attribute in 2.1."""
    arr = series.array
    return getattr(arr, "_pa_array", None) or arr._data


def _d(frame):
    return frame.to_arrow().to_pydict()


def _clean(values):
    return [None if (isinstance(v, float) and math.isnan(v)) or v is pd.NA else v for v in values]


def _frame():
    return DataFrame({"a": [1, 2, None, 4], "b": [1.5, -2.5, 3.5, 4.0], "s": ["x", "y", "x", "z"]})


def _close(a, b):
    if a is None or b is None or isinstance(a, (str, bool)) or isinstance(b, (str, bool)):
        return a == b
    return a == pytest.approx(b)


def _same_bool(ours, ref, cols=None):
    got = _d(ours)
    for c in cols or got:
        assert [bool(v) for v in got[c]] == ref[c].tolist(), c


def _same(ours, ref, cols=None):
    got = _d(ours)
    for c in cols or got:
        mine, theirs = _clean(got[c]), _clean(ref[c].tolist())
        assert len(mine) == len(theirs), c
        assert all(_close(a, b) for a, b in zip(mine, theirs)), (c, mine, theirs)


def test_frame_arithmetic_and_comparisons():
    df = _frame()
    p = df.to_pandas()
    num = p[["a", "b"]]
    _same(df + 1, num + 1)
    _same(df * 2, num * 2)
    _same(df - df, num - num)
    _same(1 - df, 1 - num)
    _same(df.add(Series([10, 20, 30, 40])), num.add(pd.Series([10, 20, 30, 40]), axis=0))
    _same(df.div(2), num.div(2))
    _same(df.rdiv(2), num.rdiv(2))
    _same(-df, -num)
    # A comparison against a null is null here (polars / SQL); pandas says
    # False.
    _same_bool(df.eq(1), p.eq(1), ["a", "b"])
    assert _d(df.eq("x"))["s"] == p.eq("x")["s"].tolist()
    assert _d(df.ne("x"))["a"] == [True] * 4
    _same_bool(df.select("a", "b").gt(2), num.gt(2))
    _same_bool(df.le(df), num.le(num))
    with pytest.raises(TypeError):  # as pandas: str > int
        df.gt(2)
    with pytest.raises(TypeError):
        df.add("x")
    with pytest.raises(ValueError, match="rows"):
        df.add(Series([1, 2]))


def test_frame_column_transforms():
    df = _frame()
    p = df.to_pandas()
    num = p[["a", "b"]]
    _same(df.abs(), num.abs(), ["a", "b"])
    _same(df.round(), num.round(), ["a", "b"])
    _same(df.clip(0, 3), num.clip(0, 3), ["a", "b"])
    _same(df.cumsum(), num.cumsum(), ["a", "b"])
    _same(df.cummax(), num.cummax(), ["a", "b"])
    _same(df.cummin(), num.cummin(), ["a", "b"])
    _same(df.diff(), num.diff(), ["a", "b"])
    _same(df.pct_change(), num.pct_change(fill_method=None), ["a", "b"])
    _same(df.shift(1), p.shift(1))
    _same(df.shift(-1), p.shift(-1))
    _same(df.rank(), num.rank(), ["a", "b"])
    _same(df.isna(), p.isna())
    _same(df.notna(), p.notna())
    assert df.isnull().equals(df.isna()) and df.notnull().equals(df.notna())
    _same(df.where(df["b"] > 0, -1), num.where(p["b"] > 0, -1), ["a", "b"])
    _same(df.mask(df["b"] > 0, -1), num.mask(p["b"] > 0, -1), ["a", "b"])
    _same(df.replace(1, 100), p.replace(1, 100), ["a", "b"])
    assert _d(df.replace("x", "X"))["s"] == p.replace("x", "X")["s"].tolist()
    assert _d(df.applymap(lambda v: v))["a"] == [1, 2, None, 4]
    assert _d(df.transform("abs"))["b"] == num.transform("abs")["b"].tolist()
    assert _d(df.select("a", "b").transform(lambda s: s * 0))["a"] == [0, 0, None, 0]


def test_frame_reductions_match_pandas():
    df = _frame()
    p = df.to_pandas()
    num = p[["a", "b"]]
    assert _d(df.nunique()) == {"a": [3], "b": [4], "s": [3]}
    assert _d(df.prod())["b"] == pytest.approx([num.prod()["b"]])
    assert _d(df.sem())["b"] == pytest.approx([num.sem()["b"]])
    assert _d(df.sem())["a"] == pytest.approx([num.sem()["a"]])
    assert _d(df.idxmax()) == {"a": [3], "b": [3]}
    assert _d(df.idxmin()) == {"a": [0], "b": [1]}
    assert _d(df.set_index("s").idxmax())["b"] == ["z"]
    assert _d(df.any()) == {"a": [True], "b": [True]}
    assert _d(DataFrame({"z": [0, 0]}).any()) == {"z": [False]}
    assert _d(DataFrame({"z": [1, 0]}).all()) == {"z": [False]}
    assert _d(df.mode())["b"] == [num.mode()["b"][0]]
    corr = _d(df.corr())
    ref = num.corr()
    assert corr["column"] == ["a", "b"]
    assert corr["b"] == pytest.approx(ref["b"].tolist())
    cov = _d(df.cov())
    assert cov["a"] == pytest.approx(num.cov()["a"].tolist())
    assert _d(df.median())["b"] == [num.median()["b"]]
    assert _d(df.quantile(0.5))["b"] == [num.quantile(0.5)["b"]]
    vc = df.value_counts(["s"])
    assert _d(vc) == {"s": ["x", "y", "z"], "count": [2, 1, 1]}
    assert _d(df.value_counts())["count"] == [1, 1, 1]  # the null row is dropped


def test_frame_conveniences():
    df = _frame()
    p = df.to_pandas()
    assert df.empty is False and df.head(0).empty is True
    assert df.ndim == 2 and df.size == 12 and df.values.shape == (4, 3)
    assert df.to_numpy().shape == (4, 3)
    t = _d(df.T)
    assert t["column"] == ["a", "b", "s"] and t["0"] == ["1", "1.5", "x"] and t["2"][0] is None
    assert _d(DataFrame({"a": [1, 2]}).T) == {"column": ["a"], "0": [1], "1": [2]}
    assert [n for n, _ in df.items()] == ["a", "b", "s"]
    label, row = list(df.iterrows())[1]
    assert label == 1 and row["b"] == -2.5 and row.s == "y"
    tup = list(df.itertuples())[0]
    assert tup.Index == 0 and tup.a == 1 and tup.s == "x"
    assert df.to_records()[1] == (2, -2.5, "y")
    assert list(df.set_index("s").itertuples())[0].Index == "x"
    assert df.add_prefix("p_").columns == ["p_a", "p_b", "p_s"]
    assert df.add_suffix("_x").columns == ["a_x", "b_x", "s_x"]
    assert df.set_axis(["x", "y", "z"]).columns == ["x", "y", "z"]
    with pytest.raises(ValueError):
        df.set_axis(["x"])
    assert df.equals(df.copy()) and not df.equals(df.head(2))
    assert df.copy().set_index("a")._index == ["a"]
    assert df.select("a").squeeze().to_list() == [1, 2, None, 4]
    assert DataFrame({"a": [7]}).squeeze() == 7
    assert df.select_dtypes("number").columns == ["a", "b"]
    assert df.select_dtypes(include=["string"]).columns == ["s"]
    assert df.select_dtypes(exclude="number").columns == ["s"]
    assert _d(df.set_index("s").xs("x"))["a"] == [1, None]
    assert _d(df.truncate(1, 2))["a"] == [2, None]
    assert df.first_valid_index() == 0 and df.last_valid_index() == 3
    assert DataFrame({"a": pa.array([None, None], pa.int64())}).first_valid_index() is None
    assert _d(df.sort_index(ascending=False))["a"] == [4, None, 2, 1]
    assert _d(df.set_index("b").sort_index())["b"] == [-2.5, 1.5, 3.5, 4.0]
    assert _d(df.memory_usage())["a"][0] > 0
    assert _d(df.combine_first(DataFrame({"a": [9, 9, 9, 9]})))["a"] == [1, 2, 9, 4]
    d2 = df.copy()
    d2.update(DataFrame({"a": [None, 7, 8, None]}))
    assert d2["a"].to_list() == [1, 7, 8, 4]
    assert df.select("a", "b").fillna(0).dot(Series([1, 1, 1, 1])).to_list() == [7.0, 6.5]
    assert _d(DataFrame.from_records([{"a": 1, "b": 2}, {"a": 3}])) == {"a": [1, 3], "b": [2, None]}
    assert _d(DataFrame.from_records([(1, 2), (3, 4)], columns=["a", "b"])) == {
        "a": [1, 3],
        "b": [2, 4],
    }
    assert df.to_csv().startswith("a,b,s\n1.0,1.5,x")
    assert '"a"' in df.to_json() and "1.5" in df.to_string()
    assert (
        df.pipe(len) == 4
        and df.get("nope", "d") == "d"
        and df.get("a").to_list() == [1, 2, None, 4]
    )
    c = df.copy()
    assert c.pop("s").to_list() == ["x", "y", "x", "z"] and c.columns == ["a", "b"]
    c.insert(0, "z", 0)
    assert c.columns == ["z", "a", "b"] and c["z"].to_list() == [0, 0, 0, 0]
    with pytest.raises(ValueError):
        c.insert(0, "z", 1)
    assert p.shape == (4, 3)


def test_series_surface_matches_pandas():
    s = Series([3, 1, None, 3, 2])
    p = s.to_pandas()
    assert s.between(1, 2).to_list() == [False, True, None, False, True]
    # SQL: FALSE AND NULL is FALSE, so the open forms say False at a null.
    assert s.between(1, 3, "neither").to_list() == [False, False, False, False, True]
    assert s.between(1, 3, "left").to_list() == [False, True, False, False, True]
    with pytest.raises(ValueError):
        s.between(1, 2, "sideways")
    assert s.drop_duplicates().to_list() == [1, 2, 3]  # unique: sorted, nulls out
    assert s.duplicated().to_list() == p.duplicated().tolist()
    assert s.duplicated("last").to_list() == p.duplicated("last").tolist()
    assert s.duplicated(False).to_list() == p.duplicated(False).tolist()
    assert Series([1, 2, 3]).is_monotonic_increasing and not s.is_monotonic_increasing
    assert Series([3, 2]).is_monotonic_decreasing
    assert s.hasnans and not Series([1]).hasnans
    assert s.ndim == 1 and not s.empty and s.head(0).empty and s.nbytes > 0 and s.name is None
    assert s.values.shape == (5,) and len(s.array) == 5
    assert Series([7]).item() == 7
    with pytest.raises(ValueError):
        s.item()
    assert list(s.items())[:2] == [(0, 3), (1, 1)] and s.keys() == [0, 1, 2, 3, 4]
    assert s.get(1) == 1 and s.get(-1) == 2 and s.get(9, "d") == "d"
    assert s.copy().to_list() == s.to_list() and s.pipe(len) == 5
    codes, uniques = s.factorize()
    assert uniques.to_list() == [1, 2, 3] and codes.to_list() == [2, 0, -1, 2, 1]
    assert Series([1, 2]).repeat(2).to_list() == [1, 1, 2, 2]
    with pytest.raises(ValueError):
        Series([1]).repeat(-1)
    assert (
        s.replace(3, 0).to_list()
        == p.replace(3, 0).tolist()[:2] + [None] + p.replace(3, 0).tolist()[3:]
    )
    assert Series(["a", "b"]).replace("a", "z").to_list() == ["z", "b"]
    with pytest.raises(TypeError):
        Series(["a"]).replace(1, 2)
    assert _d(s.to_frame("v")) == {"v": [3, 1, None, 3, 2]}
    assert s.to_dict() == {0: 3, 1: 1, 2: None, 3: 3, 4: 2}
    assert _d(s.reset_index()) == {"index": [0, 1, 2, 3, 4], "0": [3, 1, None, 3, 2]}
    assert s.reset_index(drop=True).to_list() == s.to_list()
    assert s.truncate(1, 3).to_list() == [1, None, 3] and s.truncate(after=0).to_list() == [3]
    assert s.first_valid_index() == 0 and s.last_valid_index() == 4
    assert Series([None, 1]).first_valid_index() == 1

    a = Series([1.0, 2.0, 3.0, 4.0])
    b = Series([2.0, 4.0, 5.0, 9.0])
    assert a.corr(b) == pytest.approx(a.to_pandas().corr(b.to_pandas()))
    assert a.cov(b) == pytest.approx(a.to_pandas().cov(b.to_pandas()))
    desc = _d(a.describe())
    ref = a.to_pandas().describe()
    assert desc["statistic"] == list(ref.index)
    assert desc["value"] == pytest.approx(ref.tolist())
    assert a.agg("sum") == 10.0
    assert _d(a.agg(["sum", "mean"])) == {"statistic": ["sum", "mean"], "value": [10.0, 2.5]}
    assert a.transform("abs").to_list() == [1.0, 2.0, 3.0, 4.0]
    assert a.transform(lambda x: x * 2).to_list() == [2.0, 4.0, 6.0, 8.0]
    assert a.divide(2).to_list() == [0.5, 1.0, 1.5, 2.0]
    assert a.multiply(2).to_list() == a.rmul(2).to_list() == [2.0, 4.0, 6.0, 8.0]
    assert a.subtract(1).to_list() == [0.0, 1.0, 2.0, 3.0]
    assert a.radd(1).to_list() == [2.0, 3.0, 4.0, 5.0]
    assert a.rsub(10).to_list() == [9.0, 8.0, 7.0, 6.0]
    assert a.rdiv(1).to_list() == pytest.approx([1.0, 0.5, 1 / 3, 0.25])
    assert (1 / a).to_list() == pytest.approx([1.0, 0.5, 1 / 3, 0.25])
    assert a.case_when([(a > 3, 0), (a > 1, -1)]).to_list() == [1.0, -1.0, -1.0, 0.0]
    assert _d(s.groupby(Series(["x", "y", "x", "y", "x"])).sum()) == {
        "key": ["x", "y"],
        "value": [5, 4],
    }
    assert Series(["a-b", "c"]).str.split("-").explode().to_list() == ["a", "b", "c"]

    assert a.iloc[1] == 2.0 and a.iloc[-1] == 4.0 and a.iat[0] == 1.0 and a.at[2] == 3.0
    assert a.iloc[1:3].to_list() == [2.0, 3.0] and a.iloc[[0, 3]].to_list() == [1.0, 4.0]
    assert a.loc[a > 2].to_list() == [3.0, 4.0] and a.iloc[::2].to_list() == [1.0, 3.0]
    with pytest.raises(IndexError):
        a.iloc[9]
    with pytest.raises(TypeError, match="immutable"):
        a.iloc[0] = 1
    assert a.to_csv().startswith("0\n1.0")
    assert "1.0" in a.to_json() and "2.0" in a.to_string()


def test_groupby_surface_matches_pandas():
    df = DataFrame(
        {"k": ["x", "y", "x", "y", "x"], "a": [3, 1, None, 2, 3], "b": [1.0, 5.0, 2.0, 5.0, 0.5]}
    )
    g = df.group_by("k")
    pg = df.to_pandas().groupby("k")
    assert g.ngroups == pg.ngroups == 2
    assert g.groups == {k: list(v) for k, v in pg.groups.items()}
    assert g.indices == g.groups
    assert _d(g.get_group("y")) == {"k": ["y", "y"], "a": [1, 2], "b": [5.0, 5.0]}
    assert _d(df.group_by(["k", "a"]).get_group(("x", 3)))["b"] == [1.0, 0.5]
    assert g.pipe(lambda x: x.ngroups) == 2
    assert _d(g.nunique()) == {"k": ["x", "y"], "a": [1, 2], "b": [3, 1]}
    assert _d(g.sem())["b"] == pytest.approx(pg.sem()["b"].tolist())
    assert _d(g.sem())["a"] == pytest.approx(pg.sem()["a"].tolist())
    assert _d(g.any())["a"] == pg.any()["a"].tolist()
    assert _d(df.with_column("a", df["a"].fillna(0)).group_by("k").all())["a"] == [False, True]
    assert _d(g.idxmax()) == {"k": ["x", "y"], "a": [0, 3], "b": [2, 1]}
    assert _d(g.idxmin()) == {"k": ["x", "y"], "a": [0, 1], "b": [4, 1]}
    assert _d(g.idxmax())["b"] == pg.idxmax()["b"].tolist()
    assert _d(df.with_row_index("r").set_index("r").group_by("k").idxmax())["b"] == [2, 1]
    ohlc = _d(g.ohlc())
    ref = pg.ohlc()
    for c in ("a", "b"):
        for part in ("open", "high", "low", "close"):
            assert ohlc[f"{c}_{part}"] == ref[(c, part)].tolist(), (c, part)
    desc = _d(g.describe())
    ref = pg.describe()
    for c in ("a", "b"):
        for part in ("count", "mean", "std", "min", "max"):
            assert desc[f"{c}_{part}"] == pytest.approx(ref[(c, part)].tolist()), (c, part)
    vc = _d(g.value_counts())
    assert vc == {
        "k": ["x", "x", "y", "y"],
        "a": [3, 3, 1, 2],
        "b": [1.0, 0.5, 5.0, 5.0],
        "count": [1, 1, 1, 1],
    }
    assert _d(g.transform("mean")) == {c: pg.transform("mean")[c].tolist() for c in ("a", "b")}
    assert _d(g.transform("sum"))["a"] == pg.transform("sum")["a"].tolist()
    assert _d(g.transform("count"))["b"] == pg.transform("count")["b"].tolist()
    assert _d(df.group_by().transform("mean"))["b"] == [2.7] * 5

    # Lazy: the same, streaming.
    lg = df.lazy().group_by("k")
    for name in ("nunique", "sem", "any", "all", "ohlc", "describe", "value_counts"):
        assert _d(getattr(lg, name)().collect()) == _d(getattr(g, name)()), name
    assert _d(lg.transform("mean").collect()) == _d(g.transform("mean"))
    assert _d(lg.get_group("y").collect()) == _d(g.get_group("y"))
    assert lg.pipe(lambda x: x.keys) == ["k"]
    assert _d(lg.idxmax().collect()) == _d(g.idxmax())
    assert _d(lg.idxmin().collect()) == _d(g.idxmin())
    # A two-column aggregate and a parameter through the streaming spec.
    assert _d(lg.agg(col("b").corr(col("a")).alias("r")).collect()) == _d(
        g.agg(col("b").corr(col("a")).alias("r"))
    )
    assert _d(lg.agg(col("a").quantile(0.5).alias("q")).collect()).keys() == {"k", "q"}
    with pytest.raises(ValueError, match="no keys"):
        df.lazy().group_by().transform("mean")


def test_floordiv_mod_pow_ffill_bfill_match_python_and_pandas():
    s = Series([7, -7, None, 5])
    p = s.to_pandas()
    assert (s // 2).to_list() == [3, -4, None, 2]
    assert _clean(p.floordiv(2).tolist()) == [3.0, -4.0, None, 2.0]
    assert (s % 3).to_list() == [1, 2, None, 2]
    assert (s // -2).to_list() == [-4, 3, None, -3]  # floor toward -inf, as Python
    assert (s % -3).to_list() == [-2, -1, None, -1]  # the divisor's sign, as Python
    assert (s**2).to_list() == [49, 49, None, 25]
    assert (2**s).to_list() == [128, None, None, 32]  # a negative exponent is null
    assert (100 // s).to_list() == [14, -15, None, 20]
    assert (s // 0).to_list() == [None] * 4 and (s % 0).to_list() == [None] * 4
    assert (
        s.floordiv(2).to_list() == (s // 2).to_list()
        and s.rfloordiv(100).to_list() == (100 // s).to_list()
    )
    assert s.mod(3).to_list() == (s % 3).to_list() and s.rmod(100).to_list() == (100 % s).to_list()
    assert s.pow(2).to_list() == (s**2).to_list() and s.rpow(2).to_list() == (2**s).to_list()
    q, r = s.divmod(3)
    assert q.to_list() == [2, -3, None, 1] and r.to_list() == [1, 2, None, 2]
    f = Series([7.5, -7.5, 2.0])
    assert (f // 2).to_list() == [3.0, -4.0, 1.0] and (f % 2).to_list() == [1.5, 0.5, 0.0]
    assert (f**0.5).to_list()[0] == pytest.approx(7.5**0.5)
    assert (f // Series([2.0, 2.0, 0.0])).to_list() == [3.0, -4.0, None]
    assert (s // Series([2, 2, 2, 2])).to_list() == [3, -4, None, 2]
    assert (Series([2, 3]) ** Series([3, -1])).to_list() == [8, None]
    assert (Series([1, 4]) ** 0.5).to_list() == [1.0, 2.0]
    with pytest.raises(TypeError):
        Series(["a"]) // 2

    assert s.ffill().to_list() == [7, -7, -7, 5] and s.pad().to_list() == s.ffill().to_list()
    assert s.bfill().to_list() == [7, -7, 5, 5] and s.backfill().to_list() == s.bfill().to_list()
    assert Series([None, None, 1, None]).ffill().to_list() == [None, None, 1, 1]
    assert Series([None, None, 1, None]).bfill().to_list() == [1, 1, 1, None]
    assert Series(["a", None, "b"]).ffill().to_list() == ["a", "a", "b"]
    assert Series([1, 2]).ffill().to_list() == [1, 2]
    df = DataFrame({"a": [7, -7, None, 5], "b": [1.5, 2.5, None, 4.0], "s": ["x", None, "y", None]})
    pdf = df.to_pandas()
    assert _d(df // 2) == {"a": [3, -4, None, 2], "b": [0.0, 1.0, None, 2.0]}
    assert _d(df % 3)["b"] == [1.5, 2.5, None, 1.0]
    assert _d(df**2)["a"] == [49, 49, None, 25]
    assert _d(df.rfloordiv(100))["a"] == [14, -15, None, 20]
    assert _d(df.ffill()) == {c: _clean(pdf.ffill()[c].tolist()) for c in ("a", "b", "s")}
    assert _d(df.bfill()) == {c: _clean(pdf.bfill()[c].tolist()) for c in ("a", "b", "s")}

    # The running scans keep a null null (pandas), rather than carrying.
    n = Series([1, 2, None, 4])
    assert n.cumsum().to_list() == [1, 3, None, 7]
    assert n.cummax().to_list() == [1, 2, None, 4]
    assert n.cummin().to_list() == [1, 1, None, 1]
    assert n.cumprod().to_list() == [1, 2, None, 8]
    # String -> number: whole-row numbers parse, anything else is null.
    assert Series(["12", " 3 ", "x", None, "4.5"]).astype("int64").to_list() == [
        12,
        3,
        None,
        None,
        None,
    ]
    assert Series(["12", "4.5", "x"]).astype("float64").to_list() == [12.0, 4.5, None]
    assert Series(["7"]).astype("int32").to_list() == [7]
    # True division: two integers give floats, as pandas / polars.
    assert (Series([1, 2, 3]) / 2).to_list() == [0.5, 1.0, 1.5]
    assert (Series([1, 2]) / Series([2, 2])).to_list() == [0.5, 1.0]
    assert Series([1, 2]).div(2).to_list() == [0.5, 1.0]


def test_str_batch_matches_pandas():
    vals = ["hello world", "Foo Bar", "ab1", "  ", "", None, "ABC", "a-b-a"]
    s = Series(vals)
    p = s.to_pandas()

    def same(ours, theirs):
        got = ours.to_list()
        exp = [None if (not isinstance(v, list) and pd.isna(v)) else v for v in theirs.tolist()]
        # A null input has a null answer here; pandas 3's string dtype answers
        # False to a predicate on a missing value, pandas 2 NA.
        exp = [None if v is None else e for v, e in zip(vals, exp)]
        assert got == exp

    same(s.str.capitalize(), p.str.capitalize())
    same(s.str.title(), p.str.title())
    same(s.str.swapcase(), p.str.swapcase())
    same(s.str.casefold(), p.str.casefold())
    for m in (
        "isalnum",
        "isalpha",
        "isdigit",
        "isdecimal",
        "isnumeric",
        "isspace",
        "islower",
        "isupper",
        "istitle",
    ):
        same(getattr(s.str, m)(), getattr(p.str, m)())
    same(s.str.count("a"), p.str.count("a"))
    same(s.str.count(""), p.str.count(""))
    same(s.str.rfind("a"), p.str.rfind("a"))
    same(s.str.removeprefix("he"), p.str.removeprefix("he"))
    same(s.str.removesuffix("a"), p.str.removesuffix("a"))
    same(s.str.repeat(2), p.str.repeat(2))
    same(s.str.repeat(0), p.str.repeat(0))
    for width in (6, 7, 8):
        same(s.str.center(width, "*"), p.str.center(width, "*"))
    same(s.str.ljust(5, "."), p.str.ljust(5, "."))
    same(s.str.rjust(5, "."), p.str.rjust(5, "."))
    same(s.str.cat(s, sep="|"), p.str.cat(p, sep="|"))
    same(s.str.cat("!"), p.str.cat(["!"] * len(p)))
    assert s.str.cat(sep=",") == p.str.cat(sep=",")
    same(s.str.findall("[a-z]+"), p.str.findall("[a-z]+"))
    for part in range(3):
        same(s.str.partition("-")[str(part)], p.str.partition("-")[part])
        same(s.str.rpartition("-")[str(part)], p.str.rpartition("-")[part])
    assert s.str.partition("-", expand=False).to_list()[7] == ["a", "-", "b-a"]
    same(s.str.split("-").str.join("+"), p.str.split("-").str.join("+"))
    same(s.str.get(0), p.str.get(0))
    same(s.str.get(20), p.str.get(20))
    same(s.str.split("-").str.get(1), p.str.split("-").str.get(1))
    same(s.str.split("-").str.get(-1), p.str.split("-").str.get(-1))
    same(s.str.slice_replace(1, 3, "X"), p.str.slice_replace(1, 3, "X"))
    same(s.str.slice_replace(2, None, "X"), p.str.slice_replace(2, None, "X"))
    same(s.str.rsplit("-"), p.str.rsplit("-"))
    same(Series(["ab", "cab"]).str.index("b"), pd.Series(["ab", "cab"]).str.index("b"))
    same(Series(["ab", "bab"]).str.rindex("b"), pd.Series(["ab", "bab"]).str.rindex("b"))
    with pytest.raises(ValueError):
        s.str.index("zz")
    with pytest.raises(ValueError):
        s.str.get(-1)
    with pytest.raises(ValueError):
        s.str.findall("(")
    with pytest.raises(TypeError):
        Series([1, 2]).str.count("a")
    dummies = p.str.get_dummies("-").to_dict("list")
    dummies.pop("", None)  # pandas 3 emits a column for the empty token
    assert _d(s.str.get_dummies("-")) == dummies
    # The kernels behind the accessor, by their engine names.
    assert s.str_case(1).to_list() == s.str.title().to_list()
    assert s.str_is(7).to_list() == s.str.isupper().to_list()
    with pytest.raises(ValueError):
        s.str_case(9)


def test_dt_accessor_matches_pandas():
    # The epoch, a 2023 instant, one before the epoch, 2000-02-29, 2024-02-29,
    # the last microsecond of day 0, 2024-03-31 23:59:59.
    us = [
        0,
        1_700_000_000_123_456,
        -1,
        951_782_400_000_000,
        1_709_164_800_000_000,
        86_399_999_999,
        1_711_929_599_000_000,
    ]
    s = Series(us)
    p = pd.to_datetime(pd.Series(us), unit="us").astype("datetime64[ns]")
    for name in (
        "year",
        "month",
        "day",
        "hour",
        "minute",
        "second",
        "microsecond",
        "nanosecond",
        "dayofweek",
        "weekday",
        "day_of_week",
        "dayofyear",
        "day_of_year",
        "quarter",
        "is_leap_year",
        "days_in_month",
        "daysinmonth",
        "is_month_start",
        "is_month_end",
        "is_year_start",
        "is_year_end",
        "is_quarter_start",
        "is_quarter_end",
    ):
        assert getattr(s.dt, name).to_list() == getattr(p.dt, name).tolist(), name
    iso = _d(s.dt.isocalendar())
    ref = p.dt.isocalendar()
    assert iso == {c: ref[c].tolist() for c in ("year", "week", "day")}
    for freq, pfreq in (("1h", "h"), ("5min", "5min"), ("1d", "D"), ("30s", "30s")):
        assert s.dt.floor(freq).to_list() == (p.dt.floor(pfreq).astype("int64") // 1000).tolist(), (
            freq
        )
        assert s.dt.ceil(freq).to_list() == (p.dt.ceil(pfreq).astype("int64") // 1000).tolist(), (
            freq
        )
        assert s.dt.round(freq).to_list() == (p.dt.round(pfreq).astype("int64") // 1000).tolist(), (
            freq
        )
    assert s.dt.normalize().to_list() == (p.dt.normalize().astype("int64") // 1000).tolist()
    # Half to even, as pandas.
    halves = Series([2_500_000, 7_500_000, 12_500_000])
    assert halves.dt.round("5s").to_list() == [0, 10_000_000, 10_000_000]
    # Another unit on an Int64 column, and a null.
    assert Series([90]).dt("s").minute.to_list() == [1]
    assert Series([1500]).dt("ms").total_seconds().to_list() == [1.5]
    assert Series([1_500_000, None]).dt.total_seconds().to_list() == [1.5, None]
    assert Series([None, 0]).dt.year.to_list() == [None, 1970]
    with pytest.raises(ValueError):
        s.dt("weeks")
    with pytest.raises(TypeError):
        Series(["x"]).dt.year
    with pytest.raises(ValueError):
        s.dt.floor("1ns")  # not a whole number of the column's unit

    # Typed columns bring their own unit.
    ts = Series.from_arrow(pa.array(pd.to_datetime([0, 1_700_000_000_123_456_789], unit="ns")))
    pts = pd.to_datetime(pd.Series([0, 1_700_000_000_123_456_789]), unit="ns")
    assert ts.dt.nanosecond.to_list() == pts.dt.nanosecond.tolist() == [0, 789]
    assert ts.dt.microsecond.to_list() == pts.dt.microsecond.tolist()
    assert ts.dt.floor("1h").to_list() == pts.dt.floor("h").tolist()
    assert ts.dt.floor("1h").dtype == ts.dtype
    assert ts.astype("int64").to_list() == [0, 1_700_000_000_123_456_789]
    dates = Series.from_arrow(pa.array([0, 19000], pa.date32()))
    assert dates.dt.day.to_list() == pd.to_datetime(pd.Series([0, 19000]), unit="D").dt.day.tolist()
    with pytest.raises(TypeError):
        dates.dt.floor("1d")  # a date has no sub-day instant to round
    dur = Series.from_arrow(pa.array([1_500_000, 90_000_000], pa.duration("us")))
    assert dur.dt.total_seconds().to_list() == [1.5, 90.0]
    assert dur.dt.minute.to_list() == [0, 1]
    # The kernels behind the accessor, by their engine names.
    assert s.dt_part(0).to_list() == s.dt.year.to_list()
    assert s.dt_round(3_600_000_000, 0).to_list() == s.dt.floor("1h").to_list()
    with pytest.raises(ValueError):
        s.dt_part(99)


def test_groupby_apply_and_filter_trace_into_the_engine(caplog):
    import logging

    df = DataFrame({"k": ["x", "y", "x", "y", "x"], "v": [3, 1, 5, 2, 3], "n": [1, 1, 2, 1, 1]})
    g = df.group_by("k")
    pg = df.to_pandas().groupby("k")
    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        # An aggregate: one group-by, keys + value.
        assert _d(g.apply(lambda grp: grp["v"].sum())) == {"k": ["x", "y"], "value": [11, 3]}
        assert (
            _d(g.apply(lambda grp: grp["v"].sum()))["value"]
            == pg.apply(lambda grp: grp["v"].sum(), **_PG_APPLY_KW).tolist()
        )
        # Arithmetic over aggregates.
        ratio = g.apply(lambda grp: grp["v"].sum() / grp["n"].sum())
        assert _d(ratio) == {"k": ["x", "y"], "value": [2.75, 1.5]}
        assert _d(g.apply(lambda grp: grp.size() * 2))["value"] == [6, 4]
        # A row expression over aggregates: one value per input row.
        centered = g.apply(lambda grp: grp["v"] - grp["v"].mean())
        assert isinstance(centered, Series)
        assert centered.to_list() == pytest.approx([-2 / 3, -0.5, 4 / 3, 0.5, -2 / 3])
        assert g.apply(lambda grp: grp.v * 2).to_list() == [6, 2, 10, 4, 6]
        # filter: a condition over aggregates.
        assert _d(g.filter(lambda grp: grp["v"].sum() > 5)) == _d(df.filter(df["k"].str_eq("x")))
        assert _d(g.filter(lambda grp: grp["v"].sum() > 5)) == pg.filter(
            lambda grp: grp["v"].sum() > 5
        ).to_dict("list")
        assert _d(g.filter(lambda grp: grp["v"].mean() > grp["n"].mean() * 2))["v"] == [3, 5, 3]
        assert _d(g.filter(lambda grp: grp.size() >= 3))["k"] == ["x", "x", "x"]
        assert _d(g.filter(lambda grp: grp["v"].sum() > 100)) == {"k": [], "v": [], "n": []}
        assert _d(df.group_by().apply(lambda grp: grp["v"].sum() / grp["n"].sum())) == {
            "value": [14 / 6]
        }
    assert caplog.records == []
    with pytest.raises(TypeError, match="condition over aggregates"):
        g.filter(lambda grp: grp["v"] > 2)
    with pytest.raises(TypeError, match="reduce every column"):
        g.filter(lambda grp: (grp["v"] - grp["v"].mean()) > 0)
    with pytest.raises(KeyError):
        g.apply(lambda grp: grp["nope"].sum())

    # The Python tier: a body the trace cannot follow, with the warning.
    def first_if_big(grp):
        return grp["v"].to_list()[0] if len(grp) > 2 else -1

    with caplog.at_level(logging.WARNING, logger="dftracer.utils"):
        assert _d(g.apply(first_if_big)) == {"k": ["x", "y"], "value": [3, -1]}
        assert _d(g.filter(lambda grp: len(grp) > 2))["v"] == [3, 5, 3]
        assert _d(g.apply(lambda grp: grp.head(1))) == {"k": ["x", "y"], "v": [3, 1], "n": [1, 1]}
        assert g.apply(lambda grp: grp["v"].sort_values()).to_list() == [3, 3, 5, 1, 2]
    messages = [r.getMessage() for r in caplog.records]
    assert len(messages) == 4
    assert messages[0].startswith("GroupBy.apply: 'first_if_big' could not be traced")
    assert messages[1].startswith("GroupBy.filter: '<lambda>'")
    assert all("per group" in m for m in messages)

    # Lazy: the traced forms as plan steps; no Python tier.
    lg = df.lazy().group_by("k")
    assert _d(lg.apply(lambda grp: grp["v"].sum() / grp["n"].sum()).collect()) == _d(ratio)
    assert _d(lg.apply(lambda grp: grp["v"] - grp["v"].mean()).collect())["value"] == pytest.approx(
        centered.to_list()
    )
    assert _d(lg.apply(lambda grp: grp.v * 2).collect()) == {"value": [6, 2, 10, 4, 6]}
    assert _d(lg.filter(lambda grp: grp["v"].sum() > 5).collect()) == _d(
        g.filter(lambda grp: grp["v"].sum() > 5)
    )
    with pytest.raises(TypeError, match="could not be traced"):
        lg.apply(first_if_big)


def test_datetime_index_methods_match_pandas():
    us = 1_000_000
    ts = [0, 30 * us, 60 * us, 45_000 * us, 45_030 * us, 86_430 * us, 172_800 * us]
    df = DataFrame({"ts": ts, "v": list(range(7))}).set_index("ts")
    p = pd.DataFrame({"v": list(range(7))}, index=pd.to_datetime(ts, unit="us"))
    assert _d(df.at_time("00:00:30"))["v"] == p.at_time("00:00:30")["v"].tolist() == [1, 5]
    assert _d(df.at_time("12:30"))["v"] == p.at_time("12:30")["v"].tolist()
    assert (
        _d(df.between_time("00:00:10", "12:30"))["v"]
        == p.between_time("00:00:10", "12:30")["v"].tolist()
    )
    for inclusive in ("both", "neither", "left", "right"):
        assert (
            _d(df.between_time("00:00:30", "00:01:00", inclusive))["v"]
            == p.between_time("00:00:30", "00:01:00", inclusive=inclusive)["v"].tolist()
        ), inclusive
    # An end before the start wraps past midnight.
    assert (
        _d(df.between_time("12:30", "00:00:30"))["v"]
        == p.between_time("12:30", "00:00:30")["v"].tolist()
    )
    assert _d(df.first("1min"))["v"] == [0, 1]
    assert _d(df.last("1D"))["v"] == [5, 6]
    assert _d(df.reset_index().at_time("00:00:30", on="ts"))["v"] == [1, 5]
    with pytest.raises(ValueError):
        df.at_time("25:00")
    with pytest.raises(ValueError):
        df.reset_index().at_time("00:00")  # no index column and no on=

    small = DataFrame({"ts": [0, 2 * us, 3 * us], "v": [1.0, 2.0, 3.0]}).set_index("ts")
    ps = pd.DataFrame({"v": [1.0, 2.0, 3.0]}, index=pd.to_datetime([0, 2 * us, 3 * us], unit="us"))
    assert _d(small.asfreq("1s")) == {"ts": [0, us, 2 * us, 3 * us], "v": [1.0, None, 2.0, 3.0]}
    assert (
        _d(small.asfreq("1s", method="ffill"))["v"] == ps.asfreq("1s", method="ffill")["v"].tolist()
    )
    assert (
        _d(small.asfreq("1s", method="bfill"))["v"] == ps.asfreq("1s", method="bfill")["v"].tolist()
    )
    assert _d(small.asfreq("1s", fill_value=0))["v"] == ps.asfreq("1s", fill_value=0)["v"].tolist()
    assert small.asfreq("1s")._index == ["ts"]
    with pytest.raises(ValueError, match="more than once"):
        DataFrame({"ts": [0, 0], "v": [1, 2]}).set_index("ts").asfreq("1s")
    # A typed Timestamp column brings its own unit and type.
    typed = DataFrame(
        {"ts": pa.array(pd.to_datetime([0, 2 * us, 3 * us], unit="us")), "v": [1.0, 2.0, 3.0]}
    ).set_index("ts")
    assert _d(typed.asfreq("1s", method="bfill"))["v"] == [1.0, 2.0, 2.0, 3.0]
    assert typed.asfreq("1s")["ts"].dtype == typed["ts"].dtype
    assert _d(typed.first("2s"))["v"] == [1.0]
    assert _d(typed.at_time("00:00:02"))["v"] == [2.0]


def test_groupby_fills_rolling_take_sample_resample_match_pandas():
    df = DataFrame(
        {
            "k": ["x", "y", "x", "x", "y", "x"],
            "v": [None, 1.0, 2.0, None, 4.0, 5.0],
            "n": [1, 2, 3, 4, 5, 6],
        }
    )
    p = df.to_pandas()
    g, pg = df.group_by("k"), p.groupby("k")
    lg = df.lazy().group_by("k")

    _same(g.ffill(), pg.ffill())
    _same(g.bfill(), pg.bfill())
    _same(lg.ffill().collect(), pg.ffill())
    _same(lg.bfill().collect(), pg.bfill())

    def rolled(kind, *args):
        out = getattr(pg.rolling(2), kind)(*args)
        return out.reset_index(level=0, drop=True).sort_index()

    for kind in ("sum", "mean", "min", "max"):
        _same(getattr(g.rolling(2), kind)(), rolled(kind))
        _same(getattr(lg.rolling(2), kind)().collect(), rolled(kind))
    for kind in ("var", "std", "median"):
        _same(getattr(g.rolling(2), kind)(), rolled(kind))
    _same(g.rolling(2).quantile(0.5), rolled("quantile", 0.5))
    _same(g.rolling(1).sum(), p[["v", "n"]])
    with pytest.raises(ValueError):
        g.rolling(0)
    with pytest.raises(TypeError, match="eager"):
        lg.rolling(2).std()

    # take / nth by list: the rows at those positions per group, input order.
    taken = pg.take([0, -1]).reset_index(level=0, drop=True).sort_index()
    _same(g.take([0, -1]), taken, ["v", "n"])
    _same(lg.take([0, -1]).collect(), taken, ["v", "n"])
    _same(g.nth([0, 1]), pg.nth([0, 1]))
    _same(lg.nth([0, 1]).collect(), pg.nth([0, 1]))
    assert _d(g.take([0, 0])) == _d(g.nth(0))
    assert len(g.take([])) == 0

    # sample: n rows per group (fewer for a smaller group), deterministic by
    # seed, in input order; the lazy plan picks the same rows.
    one = g.sample(2)
    assert _d(one)["k"] == ["x", "y", "x", "y"]
    assert _d(one)["n"] == sorted(_d(one)["n"])
    assert _d(lg.sample(2).collect()) == _d(one)
    assert _d(g.sample(2, seed=3)) != _d(one)
    assert _d(g.sample(2, random_state=3)) == _d(g.sample(2, seed=3))
    assert _d(g.sample(9))["n"] == [1, 2, 3, 4, 5, 6]
    assert [len(grp) for _, grp in g.sample(1).to_pandas().groupby("k")] == [1, 1]

    # resample within each group: keys + time bucket, one group_by.
    us = 1_000_000
    t = DataFrame(
        {
            "k": ["a", "b", "a", "a", "b"],
            "ts": [0, us, 2 * us, 3 * us, 9 * us],
            "v": [1, 2, 3, 4, 5],
        }
    ).set_index("ts")
    pt = pd.DataFrame(
        {"k": ["a", "b", "a", "a", "b"], "v": [1, 2, 3, 4, 5]},
        index=pd.to_datetime([0, us, 2 * us, 3 * us, 9 * us], unit="us"),
    )
    ref = pt.groupby("k").resample("2s")["v"].sum()
    ref = ref[ref != 0]  # an empty bucket is not emitted
    got = t.group_by("k").resample("2s").sum()
    assert _d(got) == {"k": ["a", "a", "b", "b"], "ts": [0, 2 * us, 0, 8 * us], "v": [1, 7, 2, 5]}
    assert _d(got)["v"] == ref.tolist()
    assert [int(ts.timestamp() * us) for _, ts in ref.index] == _d(got)["ts"]
    assert got._index == ["k", "ts"]
    assert _d(t.group_by("k").resample("2s").size())["size"] == [1, 2, 1, 1]
    assert _d(t.group_by("k").resample("2s").agg(vmax=("v", "max")))["vmax"] == [1, 4, 2, 5]
    assert _d(t.group_by("k").resample(2 * us).sum()) == _d(got)
    lazy = t.reset_index().lazy().group_by("k").resample("2s", on="ts").sum().collect()
    assert _d(lazy) == _d(got)
    with pytest.raises(ValueError, match="group key"):
        t.group_by("k").resample("2s", on="k")
    with pytest.raises(KeyError):
        t.group_by("k").resample("2s", on="nope")
    typed = DataFrame({"k": t["k"], "ts": pa.array(pt.index), "v": t["v"]}).set_index("ts")
    assert _d(typed.group_by("k").resample("2s").sum())["v"] == [1, 7, 2, 5]


def test_tz_localize_and_tz_convert_match_pandas():
    us = 1_000_000
    ticks = [0, 3600 * us, 90_000 * us]
    s = Series(pa.array(pd.to_datetime(ticks, unit="us").astype("datetime64[ns]")))
    ps = pd.Series(pd.to_datetime(ticks, unit="us")).astype("datetime64[ns]")
    assert s.dt.tz is None
    utc = s.dt.tz_localize("UTC")
    assert utc.dt.tz == "UTC"
    assert str(utc.to_arrow().type) == "timestamp[ns, tz=UTC]"
    assert utc.to_pandas().equals(ps.dt.tz_localize("UTC"))
    # tz_convert keeps the instants and changes the zone the type carries.
    tokyo = utc.dt.tz_convert("Asia/Tokyo")
    assert tokyo.dt.tz == "Asia/Tokyo"
    assert tokyo.astype("int64").to_list() == utc.astype("int64").to_list()
    assert tokyo.to_pandas().equals(ps.dt.tz_localize("UTC").dt.tz_convert("Asia/Tokyo"))
    assert tokyo.to_pandas().dt.hour.tolist() == [9, 10, 10]
    # The .dt parts read UTC whatever the zone.
    assert tokyo.dt.hour.to_list() == [0, 1, 1]
    assert tokyo.dt.tz_convert(None).dt.tz is None
    assert utc.dt.tz_localize(None).dt.tz is None
    assert utc.dt.tz_localize(None).astype("int64").to_list() == [t * 1000 for t in ticks]
    with pytest.raises(ValueError, match="only UTC"):
        s.dt.tz_localize("Asia/Tokyo")
    with pytest.raises(TypeError, match="already"):
        utc.dt.tz_localize("UTC")
    with pytest.raises(TypeError, match="naive"):
        s.dt.tz_convert("UTC")
    with pytest.raises(TypeError):
        Series([1, 2]).dt.tz_localize("UTC")

    df = DataFrame({"ts": s, "v": [1, 2, 3]}).set_index("ts")
    paris = df.tz_localize("UTC").tz_convert("Europe/Paris")
    assert paris["ts"].dt.tz == "Europe/Paris"
    assert paris._index == ["ts"]
    assert _d(paris)["v"] == [1, 2, 3]
    pdf = pd.DataFrame({"v": [1, 2, 3]}, index=pd.to_datetime(ticks, unit="us"))
    assert (
        paris.to_pandas()["ts"].tolist()
        == pdf.tz_localize("UTC").tz_convert("Europe/Paris").index.tolist()
    )
    assert df.reset_index().tz_localize("UTC", on="ts")["ts"].dt.tz == "UTC"
    with pytest.raises(ValueError):
        df.reset_index().tz_localize("UTC")


def test_groupby_prod_cumprod_median_quantile_match_pandas():
    df = DataFrame(
        {
            "k": ["x", "y", "x", "x", "y", "x"],
            "v": [None, 1.0, 2.0, None, 4.0, 5.0],
            "n": [1, 2, 3, 4, 5, 6],
        }
    )
    p = df.to_pandas()
    g, pg, lg = df.group_by("k"), p.groupby("k"), df.lazy().group_by("k")
    _same(g.prod(), pg.prod(), ["v", "n"])
    _same(lg.prod().collect(), pg.prod(), ["v", "n"])
    _same(g.cumprod(), pg.cumprod())
    _same(lg.cumprod().collect(), pg.cumprod())
    assert _d(df.agg(v=("v", "prod")))["v"] == [40.0]
    # Exact quantiles, linear interpolation, one group_by per column.
    _same(g.median(), pg.median(), ["v", "n"])
    _same(lg.median().collect(), pg.median(), ["v", "n"])
    for q in (0.0, 0.3, 0.5, 0.9, 1.0):
        _same(g.quantile(q), pg.quantile(q), ["v", "n"])
        _same(lg.quantile(q).collect(), pg.quantile(q), ["v", "n"])
    _same(df.group_by().median(), p[["v", "n"]].median().to_frame().T)
    _same(df.group_by().quantile(0.3), p[["v", "n"]].quantile(0.3).to_frame().T)
    # A group with no present value keeps a null row, as pandas.
    holes = DataFrame({"k": ["x", "y"], "v": [None, 2.0]})
    assert _d(holes.group_by("k").median()) == {"k": ["x", "y"], "v": [None, 2.0]}
    with pytest.raises(ValueError):
        g.quantile(1.5)
    with pytest.raises(ValueError, match="exact group-by method"):
        g.agg("median")
    # describe now carries the exact quartiles.
    ours = _d(g.describe())
    theirs = pg.describe()
    for c in ("v", "n"):
        for stat in ("count", "mean", "std", "min", "25%", "50%", "75%", "max"):
            assert _clean(ours[f"{c}_{stat}"]) == pytest.approx(theirs[(c, stat)].tolist()), (
                c,
                stat,
            )


def test_frame_and_series_windows_match_pandas():
    df = DataFrame({"a": [1.0, None, 3.0, 6.0, 2.0], "b": [2, 4, 1, 3, 5], "s": list("abcde")})
    p = df.to_pandas()[["a", "b"]]
    # rolling: a window holding a null is null (min_periods = window).
    for stat in ("sum", "mean", "min", "max", "var", "std", "median"):
        _same(getattr(df.rolling(2), stat)(), getattr(p.rolling(2), stat)())
    _same(df.rolling(3).quantile(0.3), p.rolling(3).quantile(0.3))
    assert df.rolling(2).sum().columns == ["a", "b"]
    # expanding: a null row repeats the previous value.
    for stat in ("sum", "mean", "min", "max", "count", "var", "std"):
        _same(getattr(df.expanding(), stat)(), getattr(p.expanding(), stat)())
        _same(
            DataFrame({"a": getattr(df["a"].expanding(), stat)()}),
            getattr(p["a"].expanding(), stat)().to_frame(),
        )
    # ewm: pandas adjust=True, a null row repeats and decays the weights.
    for kw in ({"alpha": 0.5}, {"span": 3}, {"com": 2}, {"halflife": 2}):
        _same(df.ewm(**kw).mean(), p.ewm(**kw).mean())
        _same(df.ewm(**kw).std(), p.ewm(**kw).std())
        _same(DataFrame({"a": df["a"].ewm(**kw).mean()}), p["a"].ewm(**kw).mean().to_frame())
    with pytest.raises(ValueError, match="exactly one"):
        df.ewm(alpha=0.5, span=3)
    with pytest.raises(ValueError):
        df.ewm()
    # sqrt / log keep a null a null.
    assert Series([None, 4.0]).sqrt().to_list() == [None, 2.0]
    assert Series([None, 1.0]).log().to_list() == [None, 0.0]


def test_last_pandas_names_match_pandas():
    q = DataFrame({"a": [1, 2, None], "b": [1.0, 5.0, 3.0], "s": ["x", "y", "z"]})
    o = DataFrame({"a": [1, 3, None], "b": [1.0, 5.0, 4.0], "s": ["x", "y", "q"]})
    pq, po = q.to_pandas(), o.to_pandas()
    _same_bool(q.isin([1, "x"]), pq.isin([1, "x"]))
    _same_bool(q.isin({"a": [2], "s": ["z"]}), pq.isin({"a": [2], "s": ["z"]}))
    _same_bool(q.isin(Series([5.0, 3.0])), pq.isin([5.0, 3.0]))
    _same(q.select("a", "b").combine(o, lambda x, y: x + y), (pq[["a", "b"]] + po[["a", "b"]]))
    assert _d(q.compare(o)) == {
        "row": [1, 2, 2],
        "column": ["a", "b", "s"],
        "self": ["2", "3.0", "z"],
        "other": ["3", "4.0", "q"],
    }
    assert _d(q.compare(q)) == {"row": [], "column": [], "self": [], "other": []}
    _same(q.corrwith(o), pq.corrwith(po, numeric_only=True).to_frame().T)
    text = q.info()
    assert text.startswith("DataFrame: 3 rows, 3 columns") and "a: int64, 2 present" in text
    assert q.set_index("a").rename_axis("idx").columns[0] == "idx"
    assert q.set_index("a").rename_axis("idx")._index == ["idx"]
    with pytest.raises(ValueError):
        q.rename_axis("idx")
    _same(q.kurtosis(), pq.kurtosis(numeric_only=True).to_frame().T)
    assert _d(q.select("a", "b").swapaxes()) == _d(q.select("a", "b").T)

    s = Series([1.0, None, 3.0, None, 5.0])
    ps = pd.Series([1.0, None, 3.0, None, 5.0])
    assert s.asof(3) == ps.asof(3) == 3.0
    assert s.asof(0) == 1.0
    with pytest.raises(IndexError):
        s.asof(9)
    assert math.isnan(s.autocorr(1)) and math.isnan(ps.autocorr(1))
    two = Series([1.0, 2.0, 4.0, 8.0])
    assert two.autocorr(1) == pytest.approx(pd.Series([1.0, 2.0, 4.0, 8.0]).autocorr(1))
    assert (
        s.combine_first(Series([9.0] * 5)).to_list()
        == ps.combine_first(pd.Series([9.0] * 5)).tolist()
    )
    patch = Series([None, 7.0, None, None, None])
    assert s.update(patch).to_list() == [1.0, 7.0, 3.0, None, 5.0]
    assert s.drop([0, 4]).to_list() == [None, 3.0, None]
    assert s.drop(2).to_list() == [1.0, None, None, 5.0]
    with pytest.raises(KeyError):
        s.drop(9)
    assert s.equals(Series([1.0, None, 3.0, None, 5.0]))
    assert not s.equals(Series([1, 2, 3, 4, 5]))
    assert not s.equals(Series([1.0, None, 3.0]))
    with pytest.raises(TypeError):
        s.pop(0)
    assert s.sem() == pytest.approx(ps.sem())
    assert math.isnan(Series([None, 1.0]).sem())
    assert s.xs(2) == 3.0
    assert s.sort_index().to_list() == s.to_list()
    assert s.sort_index(ascending=False).to_list() == s.to_list()[::-1]
    assert s.memory_usage() == s.nbytes

    # GroupBy: expanding, ewm, corr, cov.
    df = DataFrame(
        {
            "k": ["x", "y", "x", "x", "y", "x"],
            "v": [None, 1.0, 2.0, None, 4.0, 5.0],
            "n": [1, 2, 3, 4, 5, 6],
        }
    )
    p = df.to_pandas()
    g, pg = df.group_by("k"), p.groupby("k")

    def per_row(frame):
        return frame.reset_index(level=0, drop=True).sort_index()

    for stat in ("sum", "mean", "min", "max", "count", "var", "std"):
        _same(getattr(g.expanding(), stat)(), per_row(getattr(pg.expanding(), stat)()))
    _same(g.ewm(alpha=0.5).mean(), per_row(pg.ewm(alpha=0.5).mean()))
    _same(g.ewm(span=3).std(), per_row(pg.ewm(span=3).std()))
    corr = _d(g.corr())
    assert corr["k"] == ["x"] * 4 + ["y"] * 4 and corr["a"] == ["n", "n", "v", "v"] * 2
    for k, a, b, value in zip(corr["k"], corr["a"], corr["b"], corr["value"]):
        assert value == pytest.approx(pg.corr().loc[(k, a), b])
    cov = _d(g.cov())
    for k, a, b, value in zip(cov["k"], cov["a"], cov["b"], cov["value"]):
        assert value == pytest.approx(pg.cov().loc[(k, a), b])


def test_group_by_null_keys_and_view_columns():
    # A null key: dropped as pandas by default, its own group with dropna=False.
    g = DataFrame({"k": ["a", None, "a", ""], "y": [1.0, 3.0, 5.0, 7.0]})
    assert _d(g.group_by("k").sum()) == {"k": ["a", ""], "y": [6.0, 7.0]}
    assert g.to_pandas().groupby("k").sum().reset_index().sort_values("k", ascending=False).to_dict(
        "list"
    ) == _d(g.groupby("k").sum())
    assert _d(g.group_by("k", dropna=False).sum()) == {"k": ["a", None, ""], "y": [6.0, 3.0, 7.0]}
    assert _d(g.lazy().group_by("k").size().collect()) == {"k": ["a", ""], "size": [2, 1]}
    assert _d(g.lazy().group_by("k", dropna=False).size().collect()) == {
        "k": ["a", None, ""],
        "size": [2, 1, 1],
    }
    f = DataFrame({"x": [1.0, None, 1.0, 0.0], "y": [1.0, 3.0, 5.0, 7.0]})
    assert _d(f.group_by("x", dropna=False).sum()) == {"x": [1.0, None, 0.0], "y": [6.0, 3.0, 7.0]}
    assert _d(f.group_by(["x"], dropna=False).size())["size"] == [2, 1, 1]
    assert _d(f.group_by("x").apply(lambda grp: grp["y"].sum())) == {
        "x": [1.0, 0.0],
        "value": [6.0, 7.0],
    }
    # A filter's result is a view; the group-by reads it as any column.
    v = Series([1.0, None, 3.0, None]).filter(Series([True, True, False, False]))
    frame = DataFrame({"x": v, "y": Series([1.0, 3.0])})
    assert _d(frame.group_by().agg(sx=("x", "sum"), sy=("y", "sum"))) == {"sx": [1.0], "sy": [4.0]}
    assert _d(frame.group_by("x", dropna=False).sum()) == {"x": [1.0, None], "y": [1.0, 3.0]}


def test_to_pandas_arrow_shares_buffers():
    df = DataFrame(
        {"a": list(range(1000)), "b": [float(i) for i in range(1000)], "s": ["x"] * 1000}
    )
    default = df.to_pandas()
    # pandas 3 gives its string dtype where pandas 2 gave object.
    assert [str(t) for t in default.dtypes][:2] == ["int64", "float64"]
    assert str(default.dtypes.iloc[2]) in ("object", "str")
    shared = df.to_pandas(arrow=True)
    assert [str(t) for t in shared.dtypes] == [
        "int64[pyarrow]",
        "double[pyarrow]",
        "string[pyarrow]",
    ]
    ours = df.to_arrow().column("a").chunks[0].buffers()[1].address
    assert _pa_chunked(shared["a"]).chunks[0].buffers()[1].address == ours
    assert default["a"].to_numpy().ctypes.data != ours
    assert shared["a"].tolist() == default["a"].tolist()
    one = df["b"].to_pandas(arrow=True)
    assert str(one.dtype) == "double[pyarrow]" and one.tolist() == default["b"].tolist()


def test_groupby_column_selection_and_series_key_match_pandas():
    df = DataFrame(
        {"k": ["x", "y", "x", "y", "x"], "a": [3, 1, 4, 2, 3], "b": [1.0, 5.0, 2.0, 5.0, 0.5]}
    )
    pdf = df.to_pandas()
    # groupby(k)["b"].agg([...]): the outputs by aggregate name, as pandas.
    ours = _d(df.groupby("k")["b"].agg(["sum", "mean"]))
    ref = pdf.groupby("k")["b"].agg(["sum", "mean"])
    assert ours == {"k": ["x", "y"], "sum": ref["sum"].tolist(), "mean": ref["mean"].tolist()}
    assert _d(df.groupby("k")["b"].sum())["b"] == pdf.groupby("k")["b"].sum().tolist()
    assert list(df.groupby("k")[["a"]].agg("max").columns) == ["k", "a"]
    # A Series key: the group column is named `key`; groups in first-seen
    # order (pandas sorts them), so compare by key.
    ours = _d(df.groupby(df["a"] // 2)["b"].agg(["count", "sum"]))
    ref = pdf.groupby(pdf["a"] // 2)["b"].agg(["count", "sum"])
    assert ours["key"] == [1, 0, 2]
    assert dict(zip(ours["key"], ours["count"])) == ref["count"].to_dict()
    assert dict(zip(ours["key"], ours["sum"])) == ref["sum"].to_dict()
    # A non-name is simply not in the frame.
    assert (df["a"] in df) is False
