"""iloc / loc / at / iat, set_index / reset_index, assignment through them and
resample: the index is a column (or the row position), reads are engine row
ops, writes rebuild the touched columns and rebind the handle (pandas 3
copy-on-write), and every value is checked against pandas."""

import pytest

pa = pytest.importorskip("pyarrow")
pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series, col  # noqa: E402


def _df():
    return DataFrame({"k": [10, 20, 30, 40], "a": [1, 2, 3, 4], "s": ["x", "y", "z", "w"]})


def _d(frame):
    return frame.to_arrow().to_pydict()


def _same(ours, ref):
    got = _d(ours)
    exp = {c: [None if pd.isna(v) else v for v in ref[c].tolist()] for c in ref.columns}
    assert got == exp


def test_iloc_reads_match_pandas():
    df = _df()
    pdf = df.to_pandas()
    row = df.iloc[1]
    assert dict(row.items()) == {"k": 20, "a": 2, "s": "y"}
    assert row.a == 2
    assert df.iloc[-1, 1] == pdf.iloc[-1, 1] == 4
    _same(df.iloc[1:3], pdf.iloc[1:3])
    _same(df.iloc[::2], pdf.iloc[::2])
    _same(df.iloc[[3, 0]], pdf.iloc[[3, 0]])
    _same(df.iloc[-2:], pdf.iloc[-2:])
    _same(df.iloc[:, 0:2], pdf.iloc[:, 0:2])
    _same(df.iloc[[0, 2], [2, 0]], pdf.iloc[[0, 2], [2, 0]])
    assert df.iloc[:, 2].to_list() == ["x", "y", "z", "w"]
    assert df.iloc[[True, False, True, False], 2].to_list() == ["x", "z"]
    assert df.iloc[Series([True, False, True, False])]["a"].to_list() == [1, 3]
    assert df.iloc[Series([3, 1])]["a"].to_list() == [4, 2]
    with pytest.raises(IndexError):
        df.iloc[4]
    with pytest.raises(IndexError):
        df.iloc[[0, -5]]
    with pytest.raises(TypeError):
        df.iloc["a"]
    with pytest.raises(TypeError):
        df.iloc[0, 1, 2]
    assert df.iat[2, 2] == "z"
    with pytest.raises(TypeError):
        df.iat[2]


def test_loc_reads_default_index_and_set_index():
    df = _df()
    pdf = df.to_pandas()
    # No index set: labels are row positions, slices inclusive as pandas.
    _same(df.loc[1:2], pdf.loc[1:2])
    _same(df.loc[[0, 3]], pdf.loc[[0, 3]])
    assert df.loc[[0, 3], "s"].to_list() == ["x", "w"]
    _same(df.loc[df["a"] > 2, ["k"]], pdf.loc[pdf["a"] > 2, ["k"]])
    _same(df.loc[:, "a":"s"], pdf.loc[:, "a":"s"])
    assert df.index.to_list() == [0, 1, 2, 3]

    d = df.set_index("k")
    pk = pdf.set_index("k")
    assert d.columns == ["k", "a", "s"]  # the column stays; only the name is stored
    assert d.index.to_list() == [10, 20, 30, 40]
    assert d.loc[20]["a"].to_list() == [2]
    assert d.loc[20:30, "a"].to_list() == pk.loc[20:30, "a"].tolist()
    assert d.loc[:20, "a"].to_list() == pk.loc[:20, "a"].tolist()
    assert d.loc[30:, "a"].to_list() == pk.loc[30:, "a"].tolist()
    assert _d(d.loc[[10, 40]])["a"] == pk.loc[[10, 40]]["a"].tolist()
    assert d.loc[col("a") >= 3, "s"].to_list() == ["z", "w"]
    assert d.loc[Series([40, 10]), "a"].to_list() == [1, 4]
    assert d.at[30, "s"] == pk.at[30, "s"]
    with pytest.raises(KeyError):
        d.loc[:, "nope"]
    with pytest.raises(KeyError):
        df.set_index("nope")
    with pytest.raises(TypeError):
        d.loc[True]
    with pytest.raises(TypeError):
        d.loc["a":"b"]
    with pytest.raises(KeyError, match="matches 0 rows"):
        d.at[99, "s"]

    # A string index.
    s = df.set_index("s")
    assert s.loc["y"]["a"].to_list() == [2]
    assert s.loc[["w", "x"], "a"].to_list() == [1, 4]
    assert s.at["z", "k"] == 30

    # The index name follows the frame through ops that keep the column, and
    # is dropped when the column goes.
    assert d.sort_values("a", ascending=False).loc[10, "s"].to_list() == ["x"]
    assert d.filter(d["a"] > 1)._index == ["k"]
    assert d.head(2).loc[20]["a"].to_list() == [2]
    assert d.select("a")._index is None
    assert d.lazy()._index == ["k"]


def test_reset_index():
    df = _df()
    assert df.reset_index().columns == ["index", "k", "a", "s"]
    assert _d(df.reset_index())["index"] == [0, 1, 2, 3]
    assert df.reset_index(drop=True).columns == ["k", "a", "s"]
    d = df.set_index("k")
    back = d.reset_index()
    assert back.columns == ["k", "a", "s"] and back._index is None
    assert d.reset_index(drop=True).columns == ["a", "s"]


def test_assignment_matches_pandas_copy_on_write():
    df = _df()
    d = df.set_index("k")
    pk = df.to_pandas().set_index("k")
    earlier = d.head(4)
    alias = d

    d.loc[d["a"] > 2, "a"] = 0
    pk.loc[pk["a"] > 2, "a"] = 0
    assert d["a"].to_list() == pk["a"].tolist() == [1, 2, 0, 0]
    assert alias["a"].to_list() == [1, 2, 0, 0]  # the same handle
    assert earlier["a"].to_list() == [1, 2, 3, 4]  # a handle taken before

    d.loc[20, "s"] = "Y"
    pk.loc[20, "s"] = "Y"
    # The index column stays a column here, so positions count it (pandas
    # moves it out of the columns: its column 0 is our column 1).
    d.iloc[0, 1] = 99
    pk.iloc[0, 0] = 99
    d.at[40, "a"] = -1
    pk.at[40, "a"] = -1
    d.loc[[10, 20], "a"] = [11, 22]
    pk.loc[[10, 20], "a"] = [11, 22]
    d.loc[d["a"].eq(0), "new"] = 7.5
    pk.loc[pk["a"].eq(0), "new"] = 7.5
    d["b"] = [1.5, 2.5, 3.5, 4.5]
    pk["b"] = [1.5, 2.5, 3.5, 4.5]
    d["c"] = 1
    pk["c"] = 1
    d.loc[:, ["a", "c"]] = 5
    pk.loc[:, ["a", "c"]] = 5
    d.iat[3, 1] = 8
    pk.iat[3, 0] = 8
    d.loc[d["b"] > 3, ["a", "c"]] = {"a": 0, "c": 9}  # a dict value, one per column
    got = _d(d)
    exp = pk.reset_index()
    for c in ["k", "a", "s", "new", "b", "c"]:
        if c in ("a", "c"):
            continue
        assert got[c] == [None if pd.isna(v) else v for v in exp[c].tolist()], c
    assert got["a"] == [5, 5, 0, 0]
    assert got["c"] == [5, 5, 9, 9]
    assert d.columns == ["k", "a", "s", "new", "b", "c"]
    assert d._index == ["k"]

    # Every column where a mask holds (pandas df[mask] = value).
    num = d.select("k", "a", "b")
    num[num["a"] > 4] = 0
    assert _d(num) == {"k": [0, 0, 30, 40], "a": [0, 0, 0, 0], "b": [0.0, 0.0, 3.5, 4.5]}
    # A Series value, full length, and a mismatched length.
    num["a"] = Series([7, 8, 9, 10])
    assert num["a"].to_list() == [7, 8, 9, 10]
    with pytest.raises(ValueError, match="rows"):
        num["a"] = [1, 2]
    with pytest.raises(ValueError, match="selection"):
        num.loc[num["a"] > 8, "a"] = [1, 2, 3]
    # A value the column's type refuses.
    with pytest.raises(TypeError, match="cannot assign"):
        d.loc[:, "s"] = 0
    with pytest.raises(TypeError):
        d[3] = 0


def test_lazy_iloc_loc_and_assignment():
    df = _df()
    lf = df.lazy()
    assert _d(lf.iloc[1:3].collect())["a"] == [2, 3]
    assert _d(lf.iloc[2].collect())["a"] == [3]
    assert _d(lf.iloc[[0, 3], 1].collect()) == {"a": [1, 4]}
    assert _d(lf.iloc[1:].collect())["a"] == [2, 3, 4]
    assert _d(lf.iloc[Series([True, False, False, True])].collect())["a"] == [1, 4]
    with pytest.raises(IndexError):
        lf.iloc[-1]
    with pytest.raises(IndexError):
        lf.iloc[::2]
    # No index: labels are positions, a hidden row index is projected away.
    assert _d(lf.loc[1:2].collect())["a"] == [2, 3]
    assert lf.loc[1:2].collect().columns == ["k", "a", "s"]
    assert _d(lf.loc[[0, 3], "a"].collect()) == {"a": [1, 4]}
    assert _d(lf.loc[col("a") > 2, ["s"]].collect()) == {"s": ["z", "w"]}

    k = lf.set_index("k")
    assert k.filter(col("a") > 1)._index == ["k"]
    assert _d(k.loc[20:30, "a"].collect()) == {"a": [2, 3]}
    assert _d(k.loc[20].collect())["s"] == ["y"]
    assert k.loc[[10, 40]].collect().columns == ["k", "a", "s"]
    assert _d(k.loc[Series([False, True, True, False]), "a"].collect()) == {"a": [2, 3]}
    with pytest.raises(TypeError):
        k.loc[Series([10, 20])]
    assert k.reset_index()._index is None
    assert k.reset_index(drop=True).columns == ["a", "s"]
    assert lf.reset_index().columns == ["index", "k", "a", "s"]

    k.loc[col("a") > 2, "a"] = 0
    k.iloc[0, 1] = 99
    k.loc[20, "a"] = col("k") * 2
    k.loc[:, "a"] = col("a") + 1
    assert _d(k.collect()) == {
        "k": [10, 20, 30, 40],
        "a": [100, 41, 1, 1],
        "s": ["x", "y", "z", "w"],
    }
    assert k.columns == ["k", "a", "s"]
    # Positional labels with no index set: the hidden row index is gone after.
    p = df.lazy()
    p.loc[1:2, "a"] = -1
    assert _d(p.collect())["a"] == [1, -1, -1, 4]
    assert p.columns == ["k", "a", "s"]
    with pytest.raises(KeyError, match="cannot create"):
        k.loc[:, "new"] = 1
    with pytest.raises(TypeError):
        k.loc[:, "a"] = Series([1, 2, 3, 4])
    with pytest.raises(TypeError):
        k.loc[:, "s"] = "Y"


def test_resample_matches_pandas():
    us = 1_000_000
    t = DataFrame(
        {
            "ts": [7 * us, 0, 1 * us, 6 * us, 12 * us],
            "v": [4.0, 1.0, 2.0, 3.0, 5.0],
            "n": [1, 1, 1, 1, 1],
        }
    )
    ref = pd.DataFrame(
        {"ts": pd.to_datetime(t["ts"].to_list(), unit="us"), "v": t["v"].to_list(), "n": 1}
    ).set_index("ts")

    def same(ours, theirs):
        got = _d(ours)
        assert got["ts"] == [0, 5 * us, 10 * us]
        for c in got:
            if c != "ts":
                assert got[c] == pytest.approx(theirs[c].tolist()), c

    same(t.resample("5s", on="ts").sum(), ref.resample("5s").sum())
    same(t.set_index("ts").resample("5s").mean(), ref.resample("5s").mean())
    same(t.resample("5s", on="ts").max(), ref.resample("5s").max())
    same(t.resample("5s", on="ts").min(), ref.resample("5s").min())
    same(t.resample("5s", on="ts").count(), ref.resample("5s").count())
    same(t.resample("5s", on="ts").first(), ref.resample("5s").first())
    same(t.resample("5s", on="ts").last(), ref.resample("5s").last())
    same(t.resample(5 * us, on="ts").sum(), ref.resample("5s").sum())
    same(t.resample("5000ms", on="ts").sum(), ref.resample("5s").sum())
    assert _d(t.resample("5s", on="ts").size())["size"] == ref.resample("5s").size().tolist()
    got = _d(t.set_index("ts").resample("5s").agg(col("v").mean().alias("m"), c=("n", "sum")))
    assert got == {"ts": [0, 5 * us, 10 * us], "m": [1.5, 3.5, 5.0], "c": [2, 2, 1]}
    # Lazy: the same plan, streaming.
    assert _d(t.lazy().resample("5s", on="ts").sum().collect()) == _d(
        t.resample("5s", on="ts").sum()
    )
    assert _d(t.lazy().set_index("ts").resample("1min").size().collect())["size"] == [5]
    # A different column unit.
    ns = DataFrame({"ts": [0, 4_000_000_000, 5_000_000_000], "v": [1, 2, 3]})
    assert _d(ns.resample("5s", on="ts", unit="ns").sum())["v"] == [3, 3]
    with pytest.raises(ValueError, match="set_index"):
        t.resample("5s")
    with pytest.raises(ValueError, match="whole number"):
        t.resample("1ns", on="ts")
    with pytest.raises(ValueError, match="cannot parse"):
        t.resample("five", on="ts")
    with pytest.raises(ValueError, match="unknown unit"):
        t.resample("5 fortnights", on="ts")
    with pytest.raises(KeyError):
        t.resample("5s", on="nope")


def test_several_index_columns_match_pandas_multiindex():
    df = DataFrame(
        {
            "pid": [1, 1, 2, 2, 2],
            "tid": [10, 11, 10, 11, 12],
            "v": [1.0, 2.0, 3.0, 4.0, 5.0],
        }
    )
    d = df.set_index(["pid", "tid"])
    pm = df.to_pandas().set_index(["pid", "tid"])
    assert d._index == ["pid", "tid"]
    assert d.index.columns == ["pid", "tid"]
    assert len(d.index_levels()) == 2

    # A full tuple, a partial tuple (the leading level), a bare first label.
    assert _d(d.loc[(2, 11)])["v"] == [pm.loc[(2, 11), "v"]]
    assert _d(d.loc[(2, 11), :])["v"] == [4.0]
    assert d.loc[(2, 11), "v"].to_list() == [4.0]
    assert _d(d.loc[(2,)])["v"] == pm.loc[(2,), "v"].tolist()
    assert _d(d.loc[2])["v"] == pm.loc[2, "v"].tolist()
    assert _d(d.loc[[(1, 11), (2, 12)]])["v"] == pm.loc[[(1, 11), (2, 12)], "v"].tolist()
    assert d.loc[[(1, 11), (2, 12)], "v"].to_list() == [2.0, 5.0]
    assert _d(d.loc[[1]])["v"] == [1.0, 2.0]
    assert d.at[(2, 10), "v"] == pm.at[(2, 10), "v"]
    with pytest.raises(TypeError, match="one-column"):
        d.loc[1:2]
    with pytest.raises((KeyError, TypeError)):
        d.loc[(1, 10, 99)]
    with pytest.raises(KeyError, match="levels"):
        d.loc[[(1, 10, 99)]]
    with pytest.raises(ValueError, match="columns"):
        d.resample("5s")

    # Writes through a tuple label.
    d.loc[(2, 11), "v"] = 0
    pm.loc[(2, 11), "v"] = 0
    d.loc[[(1, 10), (2, 12)], "v"] = -1
    pm.loc[[(1, 10), (2, 12)], "v"] = -1
    assert d["v"].to_list() == pm["v"].tolist()
    assert d._index == ["pid", "tid"]
    # The names follow the frame while both columns stay, and go together.
    assert d.sort_values("v")._index == ["pid", "tid"]
    assert d.select("pid", "v")._index is None
    assert d.reset_index(drop=True).columns == ["v"]
    assert df.set_index(["pid"])._index == ["pid"]
    with pytest.raises(ValueError):
        df.set_index([])

    # Lazy: the same labels as expressions over both columns.
    k = df.lazy().set_index(["pid", "tid"])
    assert _d(k.loc[(2, 11)].collect())["v"] == [4.0]
    assert _d(k.loc[(2, 11), "v"].collect()) == {"v": [4.0]}
    assert _d(k.loc[2].collect())["v"] == [3.0, 4.0, 5.0]
    assert _d(k.loc[[(1, 11), (2, 12)]].collect())["v"] == [2.0, 5.0]
    with pytest.raises(TypeError, match="one-column"):
        k.loc[1:2]
    k.loc[(2, 11), "v"] = 0
    k.loc[[(1, 10), (2, 12)], "v"] = -1
    assert _d(k.collect())["v"] == d["v"].to_list()
    assert k.collect()._index == ["pid", "tid"]
