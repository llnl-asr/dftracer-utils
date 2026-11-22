# SPDX-License-Identifier: MIT
"""The levels of a row index on a Series and a DataFrame: droplevel, swaplevel,
reorder_levels, rename_axis, add_prefix and add_suffix, set_index(append=True),
reset_index(level=), sort_index(level=) and the Index surface of ``index``.
Every case runs on real pandas and on ours over the same index (one, two and
three levels, unnamed levels, repeated labels, null labels, no index) and the
values, labels, level names and errors are compared."""

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402

from .common import skip_below_pandas3  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

NAN = float("nan")


def _frame(index, **cols):
    return pd.DataFrame(cols, index=index)


G = ["b", "a", "b", "a", "a"]
N = [2, 1, 1, 2, 1]
M = ["x", "y", "z", "w", "x"]
V = [1.0, 2.0, 3.0, 4.0, 5.0]
W = [10, 20, 30, 40, 50]

FRAMES = {
    "two": _frame(pd.MultiIndex.from_arrays([G, N], names=["g", "n"]), v=V, w=W),
    "three": _frame(pd.MultiIndex.from_arrays([G, N, M], names=["g", "n", "m"]), v=V, w=W),
    "one": _frame(pd.Index([30, 10, 20, 10, 40], name="k"), v=V, w=W),
    "unnamed": _frame(pd.MultiIndex.from_arrays([G, N]), v=V, w=W),
    "nulls": _frame(
        pd.MultiIndex.from_arrays([G, [2.0, NAN, 1.0, 2.0, NAN]], names=["g", "n"]), v=V, w=W
    ),
    "none": pd.DataFrame({"v": V, "w": W}),
}
SERIES = {k: f["v"] for k, f in FRAMES.items()}


def _kind(e):
    for base in (KeyError, IndexError, NotImplementedError, TypeError, ValueError, AssertionError):
        if isinstance(e, base):
            return base.__name__
    return type(e).__name__


# pandas raises AssertionError or a bare Exception where ours raises a typed error.
SOFT = {"AssertionError": {"ValueError", "TypeError"}, "Exception": {"TypeError"}}


def _outcome(fn, obj):
    try:
        return fn(obj)
    except Exception as e:  # noqa: BLE001 - the kind is compared
        return e


def _frames_equal(x, y):
    assert list(x.index.names) == list(y.index.names)
    assert [_norm(i) for i in x.index.tolist()] == [_norm(i) for i in y.index.tolist()]
    assert list(x.columns) == list(y.columns)
    pd.testing.assert_frame_equal(
        x, y, check_dtype=False, check_index_type=False, check_column_type=False, check_names=False
    )


def _norm(v):
    if isinstance(v, tuple):
        return tuple(_norm(i) for i in v)
    return None if v is None or (isinstance(v, float) and v != v) else v


def _same(x, y):
    if isinstance(y, Exception):
        assert isinstance(x, Exception), f"ours returned, pandas raised {y!r}"
        want = _kind(y)
        assert _kind(x) == want or _kind(x) in SOFT.get(want, ()), f"{x!r} vs {y!r}"
        return
    assert not isinstance(x, Exception), f"ours raised {x!r}"
    x = x.to_pandas() if hasattr(x, "to_pandas") else x
    if isinstance(y, pd.DataFrame):
        _frames_equal(x, y)
    elif isinstance(y, pd.Series):
        assert list(x.index.names) == list(y.index.names)
        assert [_norm(i) for i in x.index.tolist()] == [_norm(i) for i in y.index.tolist()]
        pd.testing.assert_series_equal(
            x, y, check_dtype=False, check_index_type=False, check_names=False
        )
        assert x.name == y.name
    elif isinstance(y, pd.Index):
        assert list(x.names) == list(y.names)
        assert [_norm(i) for i in x.tolist()] == [_norm(i) for i in y.tolist()]
    else:
        assert _norm(x) == _norm(y)


def _check(fn, pandas_obj, ours_obj):
    skip_below_pandas3()
    _same(_outcome(fn, ours_obj), _outcome(fn, pandas_obj))


def _skip_known(form, name):
    """The pandas behaviors that differ between its versions, or are bugs."""
    if form[0] == "sort asc list" and name in ("one", "none"):
        pytest.skip("pandas ignores a list of flags on a single level")


FRAME_FORMS = [
    ("droplevel 0", lambda o: o.droplevel(0)),
    ("droplevel 1", lambda o: o.droplevel(1)),
    ("droplevel -1", lambda o: o.droplevel(-1)),
    ("droplevel -2", lambda o: o.droplevel(-2)),
    ("droplevel name", lambda o: o.droplevel("g")),
    ("droplevel name n", lambda o: o.droplevel("n")),
    ("droplevel list names", lambda o: o.droplevel(["g", "n"])),
    ("droplevel list pos", lambda o: o.droplevel([0, -1])),
    ("droplevel empty", lambda o: o.droplevel([])),
    ("droplevel all", lambda o: o.droplevel([0, 1, 2])),
    ("droplevel missing name", lambda o: o.droplevel("zz")),
    ("droplevel too big", lambda o: o.droplevel(5)),
    ("droplevel too negative", lambda o: o.droplevel(-5)),
    ("swaplevel", lambda o: o.swaplevel()),
    ("swaplevel 0 1", lambda o: o.swaplevel(0, 1)),
    ("swaplevel 0 -1", lambda o: o.swaplevel(0, -1)),
    ("swaplevel names", lambda o: o.swaplevel("g", "n")),
    ("swaplevel same", lambda o: o.swaplevel(0, 0)),
    ("swaplevel missing", lambda o: o.swaplevel("zz", 0)),
    ("swaplevel too big", lambda o: o.swaplevel(0, 7)),
    ("reorder reverse", lambda o: o.reorder_levels([1, 0])),
    ("reorder names", lambda o: o.reorder_levels(["n", "g"])),
    ("reorder three", lambda o: o.reorder_levels([2, 0, 1])),
    ("reorder three names", lambda o: o.reorder_levels(["m", "g", "n"])),
    ("reorder identity", lambda o: o.reorder_levels([0, 1])),
    ("reorder mixed", lambda o: o.reorder_levels(["n", 0])),
    ("reorder short", lambda o: o.reorder_levels([0])),
    ("reorder missing", lambda o: o.reorder_levels(["zz", 0])),
    ("rename_axis list", lambda o: o.rename_axis(["a", "b"])),
    ("rename_axis list three", lambda o: o.rename_axis(["a", "b", "c"])),
    ("rename_axis list none", lambda o: o.rename_axis([None, "b"])),
    ("rename_axis list swap", lambda o: o.rename_axis(["n", "g"])),
    ("rename_axis list short", lambda o: o.rename_axis(["a"])),
    ("rename_axis scalar", lambda o: o.rename_axis("z")),
    ("rename_axis none", lambda o: o.rename_axis(None)),
    ("rename_axis dict", lambda o: o.rename_axis(index={"g": "G"})),
    ("rename_axis dict swap", lambda o: o.rename_axis(index={"g": "n", "n": "g"})),
    ("rename_axis dict missing", lambda o: o.rename_axis(index={"zz": "G"})),
    ("rename_axis func", lambda o: o.rename_axis(index=lambda x: None if x is None else x + "_")),
    ("rename_axis mapper dict", lambda o: o.rename_axis({"g": "G"}, axis=0)),
    ("rename_axis list keyword", lambda o: o.rename_axis(index=["a", "b"])),
    ("rename_axis columns none", lambda o: o.rename_axis(columns=None)),
    ("reset level name", lambda o: o.reset_index(level="n")),
    ("reset level pos", lambda o: o.reset_index(level=1)),
    ("reset level neg", lambda o: o.reset_index(level=-1)),
    ("reset level g", lambda o: o.reset_index(level="g")),
    ("reset level list", lambda o: o.reset_index(level=["n", "g"])),
    ("reset level list pos", lambda o: o.reset_index(level=[1, 0])),
    ("reset level drop", lambda o: o.reset_index(level="n", drop=True)),
    ("reset level list drop", lambda o: o.reset_index(level=[0, 1], drop=True)),
    ("reset level all", lambda o: o.reset_index(level=[0, 1])),
    ("reset all", lambda o: o.reset_index()),
    ("reset drop", lambda o: o.reset_index(drop=True)),
    ("reset level missing", lambda o: o.reset_index(level="zz")),
    ("reset level too big", lambda o: o.reset_index(level=9)),
    ("sort level name", lambda o: o.sort_index(level="n")),
    ("sort level pos", lambda o: o.sort_index(level=1)),
    ("sort level neg", lambda o: o.sort_index(level=-1)),
    ("sort level desc", lambda o: o.sort_index(level="n", ascending=False)),
    ("sort level no remaining", lambda o: o.sort_index(level="n", sort_remaining=False)),
    (
        "sort level desc no remaining",
        lambda o: o.sort_index(level=1, ascending=False, sort_remaining=False),
    ),
    ("sort level list", lambda o: o.sort_index(level=["n", "g"])),
    ("sort level list asc", lambda o: o.sort_index(level=["n", "g"], ascending=[False, True])),
    ("sort level list asc2", lambda o: o.sort_index(level=[0, 1], ascending=[True, False])),
    ("sort asc list", lambda o: o.sort_index(ascending=[False, True])),
    ("sort desc", lambda o: o.sort_index(ascending=False)),
    ("sort na first", lambda o: o.sort_index(level="n", na_position="first")),
    ("sort na first all", lambda o: o.sort_index(na_position="first")),
    ("sort ignore", lambda o: o.sort_index(level="n", ignore_index=True)),
    ("sort ignore plain", lambda o: o.sort_index(ignore_index=True)),
    (
        "sort key",
        lambda o: o.sort_index(level="n", key=lambda x: x * -1 if x.name in ("n", "k") else x),
    ),
    ("sort level missing", lambda o: o.sort_index(level="zz")),
    ("sort level too big", lambda o: o.sort_index(level=9)),
    ("sort asc length", lambda o: o.sort_index(level="n", ascending=[True, False])),
    ("sort plain", lambda o: o.sort_index()),
]
PREFIX_FRAME = [
    ("prefix", lambda o: o.add_prefix("p_")),
    ("suffix", lambda o: o.add_suffix("_s")),
    ("prefix axis 0", lambda o: o.add_prefix("p_", axis=0)),
    ("prefix axis index", lambda o: o.add_prefix("p_", axis="index")),
    ("suffix axis 0", lambda o: o.add_suffix("_s", axis=0)),
    ("prefix axis 1", lambda o: o.add_prefix("p_", axis=1)),
    ("suffix axis columns", lambda o: o.add_suffix("_s", axis="columns")),
    ("prefix bad axis", lambda o: o.add_prefix("p_", axis=2)),
]
PREFIX_SERIES = [
    ("prefix", lambda o: o.add_prefix("p_")),
    ("suffix", lambda o: o.add_suffix("_s")),
    ("prefix axis 0", lambda o: o.add_prefix("p_", axis=0)),
    ("prefix axis 1", lambda o: o.add_prefix("p_", axis=1)),
]
APPEND = [
    ("append one", "none", lambda o: o.set_index("w", append=True)),
    ("append two", "none", lambda o: o.set_index(["v", "w"], append=True)),
    ("append to one", "one", lambda o: o.set_index("w", append=True)),
    ("append to two", "two", lambda o: o.set_index("w", append=True)),
    ("append to three", "three", lambda o: o.set_index(["v", "w"], append=True)),
    ("append to unnamed", "unnamed", lambda o: o.set_index("v", append=True)),
    ("append missing", "two", lambda o: o.set_index("zz", append=True)),
    ("append a level", "two", lambda o: o.set_index("g", append=True)),
    ("append then reset", "one", lambda o: o.set_index("w", append=True).reset_index(level="w")),
]


@pytest.mark.parametrize("name", FRAMES)
@pytest.mark.parametrize("form", FRAME_FORMS, ids=[f[0] for f in FRAME_FORMS])
def test_frame_levels(name, form):
    _skip_known(form, name)
    pandas_frame = FRAMES[name]
    _check(form[1], pandas_frame, DataFrame.from_pandas(pandas_frame))


@pytest.mark.parametrize("name", SERIES)
@pytest.mark.parametrize("form", FRAME_FORMS, ids=[f[0] for f in FRAME_FORMS])
def test_series_levels(name, form):
    if form[0].startswith("reset") and "level" not in form[0]:
        pytest.skip("covered by test_series_index")
    _skip_known(form, name)
    pandas_series = SERIES[name]
    _check(form[1], pandas_series, Series.from_pandas(pandas_series))


@pytest.mark.parametrize("name", FRAMES)
@pytest.mark.parametrize("form", PREFIX_FRAME, ids=[f[0] for f in PREFIX_FRAME])
def test_frame_prefix(name, form):
    pandas_frame = FRAMES[name]
    _check(form[1], pandas_frame, DataFrame.from_pandas(pandas_frame))


@pytest.mark.parametrize("name", SERIES)
@pytest.mark.parametrize("form", PREFIX_SERIES, ids=[f[0] for f in PREFIX_SERIES])
def test_series_prefix(name, form):
    pandas_series = SERIES[name]
    _check(form[1], pandas_series, Series.from_pandas(pandas_series))


def test_prefix_of_every_label_type():
    labels = {
        "int": [1, 2, 3],
        "float": [1.5, NAN, 3.0],
        "str": ["a", None, "c"],
        "bool": [True, False, True],
        "ts": pd.to_datetime(["2020-01-01", None, "2020-01-03"]),
    }
    for kind, index in labels.items():
        s = pd.Series([1.0, 2.0, 3.0], index=pd.Index(index, name="k"))
        _check(lambda o: o.add_prefix("p_"), s, Series.from_pandas(s))
        _check(lambda o: o.add_suffix("_s"), s, Series.from_pandas(s))
        f = s.to_frame("v")
        _check(lambda o: o.add_prefix("p_", axis=0), f, DataFrame.from_pandas(f))


@pytest.mark.parametrize("case", APPEND, ids=[c[0] for c in APPEND])
def test_set_index_append(case):
    _, name, fn = case
    pandas_frame = FRAMES[name]
    _check(fn, pandas_frame, DataFrame.from_pandas(pandas_frame))


def test_set_index_append_on_a_lazy_frame_matches_the_eager_one():
    eager = DataFrame.from_pandas(FRAMES["one"])
    lazy = eager.lazy().set_index("w", append=True).collect()
    assert lazy.to_pandas().equals(eager.set_index("w", append=True).to_pandas())
    assert lazy.index_names() == ["k", "w"]
    positional = DataFrame.from_pandas(FRAMES["none"])
    got = positional.lazy().set_index("w", append=True).collect()
    assert got.to_pandas().index.names == [None, "w"]


@pytest.mark.parametrize("name", FRAMES)
def test_index_object_surface(name):
    pf = FRAMES[name]
    ours = DataFrame.from_pandas(pf)
    index = ours.index
    assert list(index.names) == list(pf.index.names) or name == "none"
    assert index.nlevels == pf.index.nlevels
    for i in range(pf.index.nlevels):
        want = pf.index.get_level_values(i).tolist()
        got = index.get_level_values(i).to_list()
        assert [_norm(x) for x in got] == [_norm(x) for x in want]
        if pf.index.names[i] is not None:
            by_name = index.get_level_values(pf.index.names[i]).to_list()
            assert [_norm(x) for x in by_name] == [_norm(x) for x in want]
        negative = index.get_level_values(i - pf.index.nlevels).to_list()
        assert [_norm(x) for x in negative] == [_norm(x) for x in want]


@pytest.mark.parametrize("name", SERIES)
def test_series_index_object_surface(name):
    ps = SERIES[name]
    ours = Series.from_pandas(ps)
    index = ours.index
    assert index.nlevels == ps.index.nlevels
    assert list(index.names) == list(ps.index.names) or name == "none"


@pytest.mark.parametrize("bad", ["zz", 3, -4])
def test_get_level_values_of_a_missing_level(bad):
    pf = FRAMES["three"]
    for obj in (pf.index, DataFrame.from_pandas(pf).index):
        with pytest.raises((KeyError, IndexError)) as caught:
            obj.get_level_values(bad)
        assert type(caught.value).__name__ == ("KeyError" if bad == "zz" else "IndexError")
    ps = FRAMES["one"]["v"]
    for obj in (ps.index, Series.from_pandas(ps).index):
        with pytest.raises((KeyError, IndexError)):
            obj.get_level_values("zz" if bad == "zz" else 5)


def test_set_names_on_the_index():
    pf = FRAMES["two"]
    ours = DataFrame.from_pandas(pf).index
    assert list(ours.set_names(["a", "b"]).names) == list(pf.index.set_names(["a", "b"]).names)
    assert list(ours.set_names("z", level="n").names) == list(
        pf.index.set_names("z", level="n").names
    )
    assert list(ours.set_names("z", level=0).names) == list(pf.index.set_names("z", level=0).names)
    one = FRAMES["one"]
    single = DataFrame.from_pandas(one).index
    assert single.set_names("q").name == one.index.set_names("q").name
    assert single.set_names(["q"]).name == "q"
    assert single.set_names(None).name is None
    with pytest.raises(ValueError):
        ours.set_names(["a"])
    with pytest.raises(ValueError):
        ours.set_names(["a", "a"])


def test_a_level_name_that_clashes_with_a_column_is_refused():
    # pandas lets a level and a column share a name; a frame here cannot
    pf = FRAMES["two"]
    ours = DataFrame.from_pandas(pf)
    pf.rename_axis(index={"g": "v"})
    with pytest.raises(ValueError):
        ours.rename_axis(index={"g": "v"})
    with pytest.raises(ValueError):
        ours.rename_axis(["v", "n"])
    with pytest.raises(ValueError):
        ours.rename_axis(["a", "a"])
    with pytest.raises(ValueError):
        ours.index.set_names(["a", "a"])
    # reset_index puts the level back as a column: it cannot join a column of that name
    clash = pd.DataFrame(
        {"level_0": [1, 2], "v": [3, 4]}, index=pd.MultiIndex.from_arrays([[1, 2], [3, 4]])
    )
    with pytest.raises(ValueError):
        clash.reset_index()
    with pytest.raises(ValueError):
        DataFrame.from_pandas(clash).reset_index()
    clash_s = pd.Series(
        [1, 2], name="g", index=pd.MultiIndex.from_arrays([[1, 2], [3, 4]], names=["g", "n"])
    )
    with pytest.raises(ValueError):
        clash_s.reset_index(level="g")
    with pytest.raises(ValueError):
        Series.from_pandas(clash_s).reset_index(level="g")


def test_rename_axis_columns_has_no_name_here():
    ours = DataFrame.from_pandas(FRAMES["two"])
    with pytest.raises(NotImplementedError):
        ours.rename_axis(columns="c")


def test_axis_one_is_the_column_labels():
    pf = FRAMES["two"]
    ours = DataFrame.from_pandas(pf)
    for fn in (
        lambda o: o.droplevel(0, axis=1),
        lambda o: o.droplevel("zz", axis=1),
        lambda o: o.swaplevel(axis=1),
        lambda o: o.reorder_levels([0], axis=1),
    ):
        _check(fn, pf, ours)
    _check(lambda o: o.droplevel([], axis=1), pf, ours)
    ps = FRAMES["two"]["v"]
    with pytest.raises(ValueError):
        Series.from_pandas(ps).droplevel(0, axis=1)


def test_reset_index_levels_lead_the_columns():
    pf = FRAMES["three"]
    ours = DataFrame.from_pandas(pf)
    got = ours.reset_index(level=["m", "g"]).to_pandas()
    want = pf.reset_index(level=["m", "g"])
    assert list(got.columns) == list(want.columns) == ["g", "m", "v", "w"]
    _same(ours.reset_index(), pf.reset_index())
    unnamed = FRAMES["unnamed"]
    got = DataFrame.from_pandas(unnamed).reset_index(level=1).to_pandas()
    assert list(got.columns) == list(unnamed.reset_index(level=1).columns) == ["level_1", "v", "w"]


def test_inplace_is_refused():
    ours = DataFrame.from_pandas(FRAMES["two"])
    for fn in (
        lambda: ours.rename_axis(["a", "b"], inplace=True),
        lambda: ours.sort_index(inplace=True),
        lambda: ours.index.set_names(["a", "b"], inplace=True),
    ):
        with pytest.raises(ValueError):
            fn()
    s = Series.from_pandas(SERIES["two"])
    with pytest.raises(ValueError):
        s.sort_index(inplace=True)
    with pytest.raises(ValueError):
        s.index.set_names("q", inplace=True)


def test_the_result_keeps_the_values_and_moves_no_row_but_for_sort_index():
    pf = FRAMES["three"]
    ours = DataFrame.from_pandas(pf)
    for fn in (
        lambda o: o.droplevel(1),
        lambda o: o.swaplevel(0, 2),
        lambda o: o.reorder_levels([2, 1, 0]),
        lambda o: o.rename_axis(["a", "b", "c"]),
        lambda o: o.add_prefix("p", axis=0),
    ):
        assert fn(ours).to_pandas()["v"].tolist() == V


MIXED = pd.DataFrame(
    {"v": range(6)},
    index=pd.MultiIndex.from_arrays(
        [[1, 2, 1, 2, 1, 2], ["b", "b", "a", "a", "c", "c"]], names=["a", "b"]
    ),
)


@pytest.mark.parametrize(
    "form",
    [
        lambda o: o.sort_index(level="a", key=lambda i: i * -1),
        lambda o: o.sort_index(level="a", sort_remaining=False, key=lambda i: i * -1),
        lambda o: o.sort_index(level=1, key=lambda i: i.str.upper() if i.name == "b" else i),
        lambda o: o.sort_index(key=lambda i: i * -1 if i.name == "a" else i),
    ],
    ids=["key on a, b remains", "key on a only", "key on text level", "key by name, no level"],
)
def test_key_applies_to_the_named_levels_only(form):
    _check(form, MIXED, DataFrame.from_pandas(MIXED))
    _check(form, MIXED["v"], Series.from_pandas(MIXED["v"]))


SET = pd.DataFrame({"a": [3, 1, 2], "b": ["x", "y", "z"], "c": [1.0, 2.0, 3.0]})


@pytest.mark.parametrize(
    "form",
    [
        lambda o: o.set_index("a").set_index("b"),
        lambda o: list(o.set_index("a").set_index("b").columns),
        lambda o: o.set_index("a").set_index("b").reset_index(),
        lambda o: o.set_index("a").set_index("a"),
        lambda o: o.set_index("a").set_index(["b", "c"]),
        lambda o: o.set_index(["a", "b"]).set_index("c"),
        lambda o: o.set_index("a").set_index("b").set_index("a"),
        lambda o: o.set_index("a").set_index("b", append=True).set_index("c"),
    ],
    ids=[
        "replace",
        "columns",
        "reset",
        "same",
        "two keys",
        "from two",
        "dropped one",
        "append then set",
    ],
)
def test_set_index_without_append_drops_the_old_index_columns(form):
    _check(form, SET, DataFrame.from_pandas(SET))


def test_from_arrow_reads_a_range_index_like_from_pandas():
    pa = pytest.importorskip("pyarrow")
    frames = [
        pd.DataFrame({"v": range(6)}).iloc[2:5],
        pd.DataFrame({"v": range(5)}, index=pd.RangeIndex(10, 60, 10, name="k")),
        pd.DataFrame({"v": range(4)}).set_axis(pd.RangeIndex(3, -1, -1)),
        pd.DataFrame({"v": range(3)}),
    ]
    for p in frames:
        got = DataFrame.from_arrow(pa.Table.from_pandas(p))
        assert got.index_names() == DataFrame.from_pandas(p).index_names()
        assert got.to_pandas().index.tolist() == p.index.tolist()


@pytest.mark.parametrize(
    "labels",
    [
        [1, 2, 3],
        [10**12, -5, 0],
        [1.5, 2.0, 1e20],
        [1e-05, 0.1, 3.25],
        ["a", "b", "c"],
        [True, False, True],
        pd.to_datetime(["2020-01-01 00:00", "2020-06-01 12:30", "2021-01-01 00:00"]),
    ],
    ids=["int", "big int", "float", "small float", "str", "bool", "timestamp"],
)
def test_prefix_text_of_every_label_type_matches_pandas(labels):
    s = pd.Series([1.0, 2.0, 3.0], index=pd.Index(labels, name="k"))
    _check(lambda o: o.add_prefix("p_"), s, Series.from_pandas(s))
    _check(lambda o: o.add_suffix("_s"), s, Series.from_pandas(s))
    _check(
        lambda o: o.add_suffix("_s", axis=0),
        s.to_frame("v"),
        DataFrame.from_pandas(s.to_frame("v")),
    )
