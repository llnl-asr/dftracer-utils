# SPDX-License-Identifier: MIT
"""The pandas result shapes on the row index: the labels a row-selecting op
keeps, ``loc`` of one label, setting with enlargement, the group-by of one
column and the windows of a group-by. Each case runs on real pandas and on
ours; the Polars spellings stay positional."""

import numpy as np
import pytest

pd = pytest.importorskip("pandas")
pytest.importorskip("pyarrow")

from dftracer.utils import DataFrame, Series  # noqa: E402

DATA = {
    "k": ["y", "x", "y", "x", "y", "z"],
    "a": [3, None, 1, 2, 5, 4],
    "b": [1.5, 2.5, None, 4.5, 0.5, 6.0],
    "s": ["p", "q", "r", "p", "q", "r"],
}


def _pair():
    ours = DataFrame(DATA)
    theirs = pd.DataFrame(
        {
            "k": pd.Series(DATA["k"], dtype="string"),
            "a": pd.Series(DATA["a"], dtype="Int64"),
            "b": pd.Series(DATA["b"], dtype="Float64"),
            "s": pd.Series(DATA["s"], dtype="string"),
        }
    )
    return ours, theirs


def _values(x):
    return [None if pd.isna(v) else v for v in x]


def _same_frame(ours, theirs):
    assert ours.index.to_list() == theirs.index.tolist()
    assert ours.columns == [str(c) for c in theirs.columns]
    for c in ours.columns:
        assert _values(ours[c].to_list()) == _values(theirs[c].tolist()), c


def _same_series(ours, theirs):
    assert ours.index.to_list() == theirs.index.tolist()
    assert _values(ours.to_list()) == _values(theirs.tolist())


SELECTING = [
    ("dropna", lambda d: d.dropna()),
    ("dropna_subset", lambda d: d.dropna(subset=["a"])),
    ("dropna_thresh", lambda d: d.dropna(thresh=4)),
    ("sort_values", lambda d: d.sort_values("a")),
    ("sort_values_desc", lambda d: d.sort_values(["k", "a"], ascending=[True, False])),
    ("nlargest", lambda d: d.nlargest(2, "b")),
    ("nsmallest", lambda d: d.nsmallest(2, "b")),
    ("tail", lambda d: d.tail(2)),
    ("tail_negative", lambda d: d.tail(-4)),
    ("take", lambda d: d.take([4, 1, 1])),
    ("mask", lambda d: d[d["b"] > 1.0]),
    ("loc_mask", lambda d: d.loc[d["b"] > 1.0]),
    ("loc_list", lambda d: d.loc[[4, 0]]),
    ("loc_slice", lambda d: d.loc[2:4]),
    ("iloc_slice", lambda d: d.iloc[1:4]),
    ("iloc_step", lambda d: d.iloc[::2]),
    ("iloc_list", lambda d: d.iloc[[5, 0]]),
    ("drop_duplicates", lambda d: d.drop_duplicates(subset=["s"])),
    ("query", lambda d: d.query("a > 2")),
    ("head", lambda d: d.head(3)),
]


@pytest.mark.parametrize("name,run", SELECTING, ids=[n for n, _ in SELECTING])
def test_row_selecting_ops_keep_the_original_positions(name, run):
    ours, theirs = _pair()
    if name == "query":
        theirs = theirs.astype({"a": "float64"})
        ours = ours.astype({"a": "float64"})
    _same_frame(run(ours), run(theirs))


def test_sample_keeps_the_positions_of_the_rows_it_took():
    ours, _ = _pair()
    out = ours.sample(3, random_state=2)
    taken = out.index.to_list()
    assert len(taken) == 3 and set(taken) <= set(range(6))
    assert out["k"].to_list() == [DATA["k"][i] for i in taken]


def test_an_op_that_keeps_every_row_in_order_makes_no_labels():
    ours, _ = _pair()
    for out in (
        ours.dropna(subset=["k"]),
        ours[ours["k"].is_not_null()],
        ours.drop_duplicates(),
        ours.query("s != 'zz'"),
        ours.head(3),
        ours.iloc[0:3],
        ours.tail(10),
    ):
        assert out.index_names() == []


def test_polars_names_stay_positional():
    ours, _ = _pair()
    for out in (
        ours.drop_nulls(),
        ours.sort("a"),
        ours.top_k(2, by="b"),
        ours.gather([4, 1]),
        ours.filter(ours["b"] > 1.0),
        ours.unique("s"),
    ):
        assert out.index_names() == []
    assert ours.lazy().sort("a").collect().index_names() == []


def test_labels_survive_the_next_op_and_a_set_index_frame_keeps_its_own():
    ours, theirs = _pair()
    _same_frame(ours.dropna().sort_values("b").tail(2), theirs.dropna().sort_values("b").tail(2))
    keyed = ours.set_index("s")
    assert keyed.dropna().index.to_list() == theirs.set_index("s").dropna().index.tolist()


def test_concat_of_a_labelled_and_a_positional_frame():
    ours, theirs = _pair()
    got = ours.head(2).concat(ours.tail(2))
    exp = pd.concat([theirs.head(2), theirs.tail(2)])
    assert got.index.to_list() == exp.index.tolist()
    assert got["k"].to_list() == exp["k"].tolist()


def test_series_methods_label_the_rows_as_before():
    s = Series([5, None, 3, 3, 1])
    p = pd.Series([5, None, 3, 3, 1], dtype="Int64")
    _same_series(s.tail(-3), p.tail(-3))
    _same_series(s.nlargest(3, keep="all"), p.nlargest(3, keep="all"))
    _same_series(s.nlargest(2, keep="last"), p.nlargest(2, keep="last"))
    _same_series(s.nsmallest(2, keep="first"), p.nsmallest(2, keep="first"))
    _same_series(s.nsmallest(3, keep="all"), p.nsmallest(3, keep="all"))
    with pytest.raises(ValueError, match="keep"):
        s.nlargest(2, keep="nope")
    a, b = Series([1, 2, 3, 4]), Series([1, 5, 3, 0])
    got, exp = a.compare(b), pd.Series([1, 2, 3, 4]).compare(pd.Series([1, 5, 3, 0]))
    assert got.index.to_list() == exp.index.tolist() == [1, 3]
    assert got["self"].to_list() == [2, 4] and got["other"].to_list() == [5, 0]


# ---- loc of one label ---------------------------------------------------------


def test_loc_of_a_unique_label_is_a_series_and_a_scalar_for_a_column():
    ours, theirs = _pair()
    ours, theirs = ours.fillna(0), theirs.fillna(0)
    row = ours.loc[2, ["a", "b"]]
    exp = theirs.loc[2, ["a", "b"]]
    assert isinstance(row, Series)
    assert row.index.to_list() == exp.index.tolist() == ["a", "b"]
    assert row.to_list() == [float(v) for v in exp.tolist()]
    assert ours.loc[2, "a"] == theirs.loc[2, "a"]
    assert ours.loc[2, "k"] == "y"


def test_loc_of_a_row_with_text_and_numbers_keeps_the_values():
    ours, _ = _pair()
    row = ours.fillna(0).loc[1]
    assert dict(row.items()) == {"k": "x", "a": 0, "b": 2.5, "s": "q"}


def test_loc_of_a_repeated_label_is_a_frame_and_of_a_partial_key_drops_the_level():
    d = {"k": [1, 1, 2], "v": [10, 20, 30]}
    ours, theirs = DataFrame(d).set_index("k"), pd.DataFrame(d).set_index("k")
    got, exp = ours.loc[1], theirs.loc[1]
    assert isinstance(got, DataFrame) and got.index.to_list() == exp.index.tolist() == [1, 1]
    assert got["v"].to_list() == [10, 20]
    assert isinstance(ours.loc[1, "v"], Series) and ours.loc[1, "v"].to_list() == [10, 20]
    assert ours.loc[2, "v"] == 30
    multi = {"a": [1, 1, 2], "b": ["x", "y", "x"], "v": [1.0, 2.0, 3.0]}
    deep, pdeep = DataFrame(multi).set_index(["a", "b"]), pd.DataFrame(multi).set_index(["a", "b"])
    assert deep.loc[1].index.to_list() == pdeep.loc[1].index.tolist() == ["x", "y"]
    assert deep.loc[(1, "y")].to_list() == pdeep.loc[(1, "y")].tolist()


def test_loc_of_a_missing_label_is_a_key_error():
    ours, _ = _pair()
    with pytest.raises(KeyError):
        ours.loc[99]
    with pytest.raises(KeyError):
        ours.loc[99, "a"]


# ---- setting with enlargement ----------------------------------------------


def test_loc_with_a_new_label_adds_a_row():
    ours, theirs = _pair()
    ours.loc[10] = ["q", 7, 0.5, "w"]
    theirs.loc[10] = ["q", 7, 0.5, "w"]
    assert ours.index.to_list() == theirs.index.tolist()
    assert ours["a"].to_list() == [None if pd.isna(v) else v for v in theirs["a"].tolist()]
    assert ours["s"].to_list() == theirs["s"].tolist()


def test_loc_with_a_new_label_and_a_column_fills_the_others_with_nulls():
    ours, _ = _pair()
    ours.loc[8, "a"] = 5
    assert ours.index.to_list() == [0, 1, 2, 3, 4, 5, 8]
    assert ours["a"].to_list()[-1] == 5
    assert ours["b"].to_list()[-1] is None and ours["k"].to_list()[-1] is None


def test_loc_with_a_new_label_on_a_text_index_and_a_scalar_or_mapping_value():
    d = {"k": ["a", "b"], "x": [1, 2], "y": [1.5, 2.5]}
    ours, theirs = DataFrame(d).set_index("k"), pd.DataFrame(d).set_index("k")
    ours.loc["c"] = {"x": 3, "y": 3.5}
    theirs.loc["c"] = {"x": 3, "y": 3.5}
    assert ours.index.to_list() == theirs.index.tolist() == ["a", "b", "c"]
    assert ours["x"].to_list() == theirs["x"].tolist()
    ours.loc["d"] = 0
    assert ours.loc["d"].to_list() == [0.0, 0.0]


def test_enlargement_refuses_a_label_or_value_that_does_not_fit():
    ours, _ = _pair()
    with pytest.raises(TypeError):
        ours.loc["new"] = ["q", 7, 0.5, "w"]
    with pytest.raises(ValueError, match="mismatched"):
        ours.loc[10] = [1, 2]
    with pytest.raises(KeyError):
        ours.loc[10, "nope"] = 1
    assert len(ours) == 6


def test_series_with_a_new_label_gets_a_row():
    s, p = Series([1, 2, 3]), pd.Series([1, 2, 3])
    s[5] = 9
    p[5] = 9
    _same_series(s, p)
    s.loc[7] = 1
    p.loc[7] = 1
    _same_series(s, p)
    t = Series([1.5, 2.5]).set_axis(["a", "b"])
    t["c"] = 3
    assert t.index.to_list() == ["a", "b", "c"] and t.to_list() == [1.5, 2.5, 3.0]
    with pytest.raises(TypeError, match="immutable"):
        t["a"] = 1
    with pytest.raises(TypeError):
        t["d"] = "x"


def test_a_series_with_an_exported_buffer_is_not_replaced():
    s = Series([1, 2, 3])
    view = np.asarray(s)
    with pytest.raises(BufferError):
        s[10] = 1
    del view
    s[10] = 1
    assert s.to_list() == [1, 2, 3, 1]


# ---- the group-by of one column ----------------------------------------------


def test_group_by_of_one_column_gives_a_series_and_of_a_list_a_frame():
    ours, theirs = _pair()
    for name in ("sum", "mean", "min", "max", "count", "first", "last", "median", "nunique"):
        got = getattr(ours.groupby("k")["b"], name)()
        exp = getattr(theirs.groupby("k")["b"], name)()
        assert isinstance(got, Series), name
        assert got.name == "b" and got.index.to_list() == exp.index.tolist(), name
        assert _values(got.to_list()) == _values(exp.tolist()), name
    frame = ours.groupby("k")[["b"]].sum()
    assert isinstance(frame, DataFrame) and frame.columns == ["b"]
    flat = ours.groupby("k", as_index=False)["b"].sum()
    assert isinstance(flat, DataFrame) and flat.columns == ["k", "b"]


def test_group_by_of_one_column_agg_and_transforms():
    ours, theirs = _pair()
    got = ours.groupby("k")["a"].agg("sum")
    assert isinstance(got, Series) and got.index.to_list() == ["x", "y", "z"]
    many = ours.groupby("k")["a"].agg(["sum", "count"])
    assert isinstance(many, DataFrame) and many.columns == ["sum", "count"]
    cum = ours.groupby("k")["b"].cumsum()
    assert isinstance(cum, Series) and cum.name == "b"
    assert _values(cum.to_list()) == _values(theirs.groupby("k")["b"].cumsum().tolist())
    head = ours.groupby("k")["a"].head(1)
    assert head.index.to_list() == theirs.groupby("k")["a"].head(1).index.tolist()


# ---- the windows of a group-by -----------------------------------------------


@pytest.mark.parametrize("sort", [True, False])
def test_group_by_rolling_and_expanding_carry_the_key_and_row_labels(sort):
    ours, theirs = _pair()
    ours, theirs = ours.select("k", "b"), theirs[["k", "b"]]
    for make in (
        lambda d: d.groupby("k", sort=sort).rolling(2).sum(),
        lambda d: d.groupby("k", sort=sort).rolling(2).mean(),
        lambda d: d.groupby("k", sort=sort).rolling(2).max(),
        lambda d: d.groupby("k", sort=sort).expanding().sum(),
        lambda d: d.groupby("k", sort=sort).expanding().count(),
    ):
        got, exp = make(ours), make(theirs)
        assert got.index_names() == ["k", "__index_level_1__"]
        assert got.index_levels()[0].to_list() == list(exp.index.get_level_values(0))
        assert got.index_levels()[1].to_list() == list(exp.index.get_level_values(1))
        assert _values(got["b"].to_list()) == _values(exp["b"].tolist())


def test_group_by_rolling_of_one_column_is_a_series_and_keeps_a_frame_index():
    ours, theirs = _pair()
    got = ours.groupby("k")["b"].rolling(2).sum()
    exp = theirs.groupby("k")["b"].rolling(2).sum()
    assert isinstance(got, Series) and got.name == "b"
    assert [list(r) for r in got.to_pandas().index] == [list(r) for r in exp.index]
    assert _values(got.to_list()) == _values(exp.tolist())
    kept = ours.select("k", "b").dropna(subset=["b"])
    out = kept.groupby("k").rolling(2).sum()
    exp = theirs[["k", "b"]].dropna(subset=["b"]).groupby("k").rolling(2).sum()
    assert out.index_levels()[1].to_list() == list(exp.index.get_level_values(1))


def test_polars_group_by_rolling_keeps_the_input_order_and_no_labels():
    ours, _ = _pair()
    out = ours.select("k", "b").group_by("k").rolling(2).sum()
    assert out.index_names() == []
    assert len(out) == 6


# ---- the index columns of an op that drops the index ---------------------------


def _no_hidden(frame):
    assert not [c for c in frame.columns if c.startswith("__index_level")], frame.columns


def test_ops_that_drop_the_index_drop_its_columns_too():
    ours, theirs = _pair()
    sorted_ours, sorted_theirs = ours.sort_values("a"), theirs.sort_values("a")
    for got, want in [
        (sorted_ours.merge(ours, on="k"), sorted_theirs.merge(theirs, on="k")),
        (ours.merge(sorted_ours, on="k"), theirs.merge(sorted_theirs, on="k")),
        (
            sorted_ours.melt(id_vars="k", value_vars=["a", "b"]),
            sorted_theirs.melt(id_vars="k", value_vars=["a", "b"]),
        ),
    ]:
        _no_hidden(got)
        assert got.shape == want.shape
    for got in [
        sorted_ours.join(ours, on="k"),
        ours.join(sorted_ours, on="k"),
        sorted_ours.with_row_index("i"),
        sorted_ours.null_count(),
        sorted_ours.lazy().join(ours.lazy(), on="k").collect(),
        sorted_ours.lazy().with_row_index("i").collect(),
        sorted_ours.lazy().describe().collect(),
        sorted_ours.compare_agg(ours, 1),
        sorted_ours.lazy().concat(ours.lazy()).collect(),
        ours.lazy().concat(sorted_ours.lazy()).collect(),
    ]:
        _no_hidden(got)
    grid = DataFrame({"k": ["u", "u"], "t": [1, 4], "v": [1.0, 2.0]}).sort_values("t")
    _no_hidden(grid.gap_fill(["k"], "t", 1, ["v"]))


def test_a_named_index_column_survives_compare_agg_and_gap_fill():
    base = DataFrame({"k": ["a", "b", "c"], "m": [1.0, 2.0, 3.0]})
    variant = DataFrame({"k": ["a", "b", "c"], "m": [2.0, 2.0, 6.0]})
    got = base.set_index("m").compare_agg(variant.set_index("m"), 1)
    assert list(got.columns) == ["k", "l_m", "r_m", "delta_m", "pct_m"]
    grid = DataFrame({"k": ["u", "u", "v"], "t": [1, 4, 1], "v": [1.0, 2.0, 3.0], "w": [1, 2, 3]})
    filled = grid.set_index("w").gap_fill(["k"], "t", 1, ["v"])
    assert "w" in filled.columns


def test_lazy_compare_agg_keeps_a_metric_that_is_the_index():
    base = DataFrame({"k": ["a", "b", "c"], "m": [1.0, 2.0, 3.0], "n": [1, 2, 3]})
    variant = DataFrame({"k": ["a", "b", "c"], "m": [2.0, 2.0, 6.0], "n": [1, 2, 3]})
    eager = base.set_index("m").compare_agg(variant.set_index("m"), 1)
    lazy = base.set_index("m").lazy().compare_agg(variant.set_index("m").lazy(), 1).collect()
    assert list(lazy.columns) == list(eager.columns)
    assert "delta_m" in lazy.columns


def test_lazy_compare_agg_drops_the_key_index_as_the_eager_op_does():
    d = DataFrame({"k": ["a", "a", "b"], "m": [1.0, 2.0, 3.0]})
    g = d.groupby("k").agg(m=("m", "sum"))
    assert list(g.compare_agg(g, 1).index) == [0, 1]
    assert list(g.lazy().compare_agg(g.lazy(), 1).collect().index) == [0, 1]


# ---- a labelled Series and the group-by result --------------------------------


def test_series_repr_shows_its_labels():
    ours, theirs = _pair()
    got = repr(ours.sort_values("a")["a"]).splitlines()
    want = repr(theirs.sort_values("a")["a"]).splitlines()
    assert [line.split()[0] for line in got[:-1]] == [line.split()[0] for line in want[:-1]]
    grouped = repr(ours.groupby("k")["a"].sum()).splitlines()
    assert [line.split()[0] for line in grouped[:4]] == ["k", "x", "y", "z"]
    assert ours.groupby("k")["a"].sum().index.to_list() == ["x", "y", "z"]


def test_group_nunique_counts_zero_for_a_group_of_only_nulls():
    ours = DataFrame({"c": ["u", "v", "w", "w"], "a": [None, 1, 2, 2]})
    theirs = pd.DataFrame(
        {"c": ["u", "v", "w", "w"], "a": pd.Series([None, 1, 2, 2], dtype="Int64")}
    )
    got = ours.groupby("c")["a"].nunique()
    want = theirs.groupby("c")["a"].nunique()
    assert got.index.to_list() == want.index.tolist()
    assert got.to_list() == want.tolist()


def test_replace_does_not_free_a_column_a_released_reader_reads():
    import threading

    n = 400_000
    s = Series(list(range(n)))
    keys = list(range(0, n, 3))
    stop = threading.Event()
    errors = []

    def read():
        while not stop.is_set():
            try:
                assert len(s.loc[keys]) == len(keys)
            except Exception as e:  # noqa: BLE001
                errors.append(repr(e))
                return

    threads = [threading.Thread(target=read) for _ in range(4)]
    for t in threads:
        t.start()
    for i in range(n, n + 60):
        s[i] = i
    stop.set()
    for t in threads:
        t.join()
    assert not errors
    assert len(s) == n + 60


def test_the_pandas_duplicated_ignores_the_row_labels():
    data = {"a": [2, 1, 2, 1, 3], "b": [1.0, 1.0, 1.0, 1.0, 3.0]}
    ours = DataFrame(data).sort_values("a")
    theirs = pd.DataFrame(data).sort_values("a")
    assert ours.duplicated(keep=False).to_list() == theirs.duplicated(keep=False).tolist()


def test_align_of_two_short_series_survives_a_concurrent_replace():
    import threading

    left = Series(list(range(10_000)))
    right = Series(list(range(5_000, 15_000)))
    stop = threading.Event()
    errors = []

    def read():
        while not stop.is_set():
            try:
                left._align(right, 0)
            except Exception as e:  # noqa: BLE001
                errors.append(repr(e))
                return

    threads = [threading.Thread(target=read) for _ in range(4)]
    for t in threads:
        t.start()
    for i in range(10_000, 10_040):
        left[i] = i
    stop.set()
    for t in threads:
        t.join()
    assert not errors
