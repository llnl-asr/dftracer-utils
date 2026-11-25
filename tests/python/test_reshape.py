# SPDX-License-Identifier: MIT
"""``stack`` and ``unstack`` on a Series and a DataFrame. Every case runs on real
pandas and on ours over the same index (one, two and three levels, unnamed
levels, repeated and null labels, unsorted labels, no index) and the values,
labels, level names and errors are compared. A frame here has flat column
names, so pandas' MultiIndex columns (``unstack`` of a frame) are compared as
the text of their tuple, the name ``from_pandas`` gives such a column."""

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402
from dftracer.utils._enums import DType  # noqa: E402

from .common import skip_below_pandas3  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

NAN = float("nan")
PANDAS_BEFORE_31 = tuple(int(x) for x in pd.__version__.split(".")[:2]) < (3, 1)

G = [2, 1, 2, 1, 3, 3]
H = ["y", "x", "x", "y", "x", "z"]
K = ["p", "p", "q", "q", "p", "q"]
V = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]
W = [10, 20, 30, 40, 50, 60]


def _frame(index, **cols):
    return pd.DataFrame(cols, index=index)


FRAMES = {
    "two": _frame(pd.MultiIndex.from_arrays([G, H], names=["a", "b"]), v=V, w=W),
    "three": _frame(pd.MultiIndex.from_arrays([G, H, K], names=["a", "b", "c"]), v=V, w=W),
    "unnamed": _frame(pd.MultiIndex.from_arrays([G, H]), v=V, w=W),
    "nulls": _frame(
        pd.MultiIndex.from_arrays([[2.0, NAN, 1.0, 2.0, NAN, 1.0], H], names=["a", "b"]), v=V, w=W
    ),
    "nullcols": _frame(
        pd.MultiIndex.from_arrays([G, ["y", None, "x", "y", None, "z"]], names=["a", "b"]),
        v=V,
        w=W,
    ),
    "holes": _frame(
        pd.MultiIndex.from_arrays([[1, 1, 2], ["x", "y", "x"]], names=["a", "b"]),
        v=[1.0, 2.0, 3.0],
        w=[1, 2, 3],
    ),
    "full": _frame(
        pd.MultiIndex.from_arrays([[2, 1, 2, 1], ["y", "y", "x", "x"]], names=["a", "b"]),
        v=[1.0, 2.0, 3.0, 4.0],
        w=[1, 2, 3, 4],
    ),
    "dups": _frame(
        pd.MultiIndex.from_arrays([[1, 1, 1], ["x", "x", "y"]], names=["a", "b"]),
        v=[1.0, 2.0, 3.0],
        w=[1, 2, 3],
    ),
    "one": _frame(pd.Index([30, 10, 20, 10, 40, 40], name="k"), v=V, w=W),
    "onedups": _frame(pd.Index([5, 4, 5], name="k"), v=[1.0, 2.0, 3.0], w=[1, 2, 3]),
    "none": pd.DataFrame({"v": V, "w": W}),
    "text": _frame(pd.MultiIndex.from_arrays([G, H], names=["a", "b"]), v=list("abcdef"), w=V),
    "nullvals": _frame(
        pd.MultiIndex.from_arrays([G, H], names=["a", "b"]), v=[1.0, NAN, 3.0, 4.0, NAN, 6.0], w=W
    ),
}
SERIES = {k: f["v"] for k, f in FRAMES.items()}
# unstack
THREE = FRAMES["three"]


def _kind(e):
    for base in (KeyError, IndexError, NotImplementedError, TypeError, ValueError):
        if isinstance(e, base):
            return base.__name__
    return type(e).__name__


def _outcome(fn, obj):
    try:
        return fn(obj)
    except Exception as e:  # noqa: BLE001 - the kind is compared
        return e


def _norm(v):
    if isinstance(v, tuple):
        return tuple(_norm(i) for i in v)
    return None if v is None or v is pd.NA or (isinstance(v, float) and v != v) else v


def _label(c):
    """The name a flat frame gives a column label: the text of the label."""
    if isinstance(c, float) and c != c:
        return "nan"
    return str(c)


def _labels(index):
    return [_norm(i) for i in index.tolist()]


def _same(ours, want):
    skip_below_pandas3()
    if isinstance(want, Exception):
        assert isinstance(ours, Exception), f"ours returned, pandas raised {want!r}"
        assert _kind(ours) == _kind(want), f"{ours!r} vs {want!r}"
        return
    assert not isinstance(ours, Exception), f"ours raised {ours!r}"
    got = ours.to_pandas() if hasattr(ours, "to_pandas") else ours
    assert list(got.index.names) == list(want.index.names)
    assert _labels(got.index) == _labels(want.index)
    if isinstance(want, pd.DataFrame):
        assert isinstance(got, pd.DataFrame)
        assert list(got.columns) == [_label(c) for c in want.columns.tolist()]
        if len(want):
            want = want.copy()
            want.columns = list(got.columns)
            pd.testing.assert_frame_equal(
                got, want, check_dtype=False, check_index_type=False, check_names=False
            )
    else:
        assert isinstance(got, pd.Series)
        pd.testing.assert_series_equal(
            got, want, check_dtype=False, check_index_type=False, check_names=False
        )
        assert got.name == want.name


def _check(fn, pandas_obj, ours_obj):
    skip_below_pandas3()
    _same(_outcome(fn, ours_obj), _outcome(fn, pandas_obj))


def _skip(form, name, series=False):
    if form[0].startswith("sort false") and PANDAS_BEFORE_31:
        pytest.skip("pandas before 3.1 unstack(sort=False) returns values under the wrong labels")
    if form[0].startswith("level list") and name in ("one", "none", "onedups"):
        if series or form[0] == "level list neg":
            pytest.skip("pandas raises AttributeError for a list of levels on a plain index")
    if not series and name == "text" and form[0].startswith("level list"):
        pytest.skip("pandas stacks text and numbers into an object Series; ours refuses")


UNSTACK_FORMS = [
    ("default", lambda o: o.unstack()),
    ("level 0", lambda o: o.unstack(0)),
    ("level 1", lambda o: o.unstack(1)),
    ("level -1", lambda o: o.unstack(-1)),
    ("level -2", lambda o: o.unstack(-2)),
    ("level a", lambda o: o.unstack("a")),
    ("level b", lambda o: o.unstack("b")),
    ("level c", lambda o: o.unstack("c")),
    ("level list", lambda o: o.unstack(["a", "b"])),
    ("level list pos", lambda o: o.unstack([0, 2])),
    ("level list neg", lambda o: o.unstack([-1, 0])),
    ("level empty", lambda o: o.unstack([])),
    ("fill 0", lambda o: o.unstack(fill_value=0)),
    ("fill 0 level 0", lambda o: o.unstack(0, fill_value=0)),
    ("fill float", lambda o: o.unstack(fill_value=-1.5)),
    ("sort false", lambda o: o.unstack(sort=False)),
    ("sort false level 0", lambda o: o.unstack(0, sort=False)),
    ("sort true", lambda o: o.unstack(sort=True)),
    ("missing name", lambda o: o.unstack("zz")),
    ("level too big", lambda o: o.unstack(5)),
    ("level too small", lambda o: o.unstack(-5)),
]


@pytest.mark.parametrize("name", FRAMES)
@pytest.mark.parametrize("form", UNSTACK_FORMS, ids=[f[0] for f in UNSTACK_FORMS])
def test_frame_unstack(name, form):
    _skip(form, name)
    pandas_frame = FRAMES[name]
    _check(form[1], pandas_frame, DataFrame.from_pandas(pandas_frame))


@pytest.mark.parametrize("name", SERIES)
@pytest.mark.parametrize("form", UNSTACK_FORMS, ids=[f[0] for f in UNSTACK_FORMS])
def test_series_unstack(name, form):
    _skip(form, name, series=True)
    pandas_series = SERIES[name]
    _check(form[1], pandas_series, Series.from_pandas(pandas_series))


def _stack(**kw):
    return lambda o: o.stack(**kw)


STACK_FORMS = [
    ("default", _stack(future_stack=True)),
    ("level 0", _stack(level=0, future_stack=True)),
    ("level -1", _stack(level=-1, future_stack=True)),
    ("level list", _stack(level=[0], future_stack=True)),
    ("level two", _stack(level=[0, 1], future_stack=True)),
    ("level too big", _stack(level=5, future_stack=True)),
    ("level too small", _stack(level=-5, future_stack=True)),
    ("level name", _stack(level="zz", future_stack=True)),
    ("old default", _stack(future_stack=False)),
    ("old dropna false", _stack(future_stack=False, dropna=False)),
    ("old dropna true", _stack(future_stack=False, dropna=True)),
    ("old sort false", _stack(future_stack=False, sort=False)),
    ("old sort true", _stack(future_stack=False, sort=True)),
    ("new dropna", _stack(future_stack=True, dropna=True)),
    ("new dropna false", _stack(future_stack=True, dropna=False)),
    ("new sort", _stack(future_stack=True, sort=True)),
    ("new sort false", _stack(future_stack=True, sort=False)),
]


@pytest.mark.parametrize("name", [n for n in FRAMES if n != "text"])
@pytest.mark.parametrize("form", STACK_FORMS, ids=[f[0] for f in STACK_FORMS])
def test_frame_stack(name, form):
    pandas_frame = FRAMES[name]
    _check(form[1], pandas_frame, DataFrame.from_pandas(pandas_frame))


def test_stack_default_is_the_new_implementation_on_pandas3():
    skip_below_pandas3()
    pandas_frame = FRAMES["nullvals"]
    ours = DataFrame.from_pandas(pandas_frame).stack()
    want = pandas_frame.stack(future_stack=True)
    _same(ours, want)
    assert len(ours) == len(want)


def test_stack_of_text_and_numbers_is_refused():
    pandas_frame = FRAMES["text"]
    with pytest.raises(ValueError):
        DataFrame.from_pandas(pandas_frame).stack()


def test_stack_of_text_columns():
    pf = pd.DataFrame({"u": list("abc"), "w": list("def")}, index=pd.Index([3, 1, 2], name="k"))
    _check(_stack(future_stack=True), pf, DataFrame.from_pandas(pf))


def test_stack_of_bool_and_mixed_numbers():
    pf = pd.DataFrame({"u": [1, 2, 3], "w": [1.5, 2.5, NAN]}, index=pd.Index([3, 1, 2], name="k"))
    _check(_stack(future_stack=True), pf, DataFrame.from_pandas(pf))
    pb = pd.DataFrame({"u": [True, False], "w": [False, False]})
    _check(_stack(future_stack=True), pb, DataFrame.from_pandas(pb))


def test_stack_with_no_columns_and_no_rows():
    pf = pd.DataFrame({"u": [], "w": []}, dtype="float64")
    _check(_stack(future_stack=True), pf, DataFrame.from_pandas(pf))


def test_stack_of_an_unnamed_column_level_adds_an_unnamed_level():
    pf = FRAMES["two"]
    got = DataFrame.from_pandas(pf).stack().to_pandas()
    assert list(got.index.names) == ["a", "b", None]


def test_a_stacked_column_level_name_is_lost():
    pf = FRAMES["one"].rename_axis(columns="cc")
    got = DataFrame.from_pandas(pf).stack()
    assert got.to_pandas().index.nlevels == 2


@pytest.mark.parametrize("name", ["two", "three", "one", "unnamed", "nulls", "none", "nullvals"])
def test_stack_then_unstack_gives_the_frame_back(name):
    pf = FRAMES[name]
    ours = DataFrame.from_pandas(pf)
    _check(lambda o: o.stack(future_stack=True).unstack(), pf, ours)


def _no_last_name(want):
    """The name of the stacked column level is lost in ours."""
    if isinstance(want, Exception):
        return want
    return want.rename_axis(index=[*want.index.names[:-1], None])


@pytest.mark.parametrize("name", ["two", "full", "nulls", "holes", "unnamed"])
def test_unstack_then_stack_round_trip(name):
    ps = SERIES[name]
    ours = Series.from_pandas(ps)
    back = _outcome(lambda o: o.unstack().stack(), ours)
    want = _outcome(lambda o: o.unstack().stack(future_stack=True), ps)
    _same(back, _no_last_name(want))


def test_unstack_of_a_group_by_size():
    pf = pd.DataFrame({"a": [1, 1, 2, 2, 2, 3], "b": ["x", "y", "x", "x", "y", "y"], "c": range(6)})
    ours = DataFrame.from_pandas(pf)
    got = ours.groupby(["a", "b"]).size().unstack()
    want = pf.groupby(["a", "b"]).size().unstack()
    _same(got, want)
    _same(ours.groupby(["a", "b"]).size().unstack(0), pf.groupby(["a", "b"]).size().unstack(0))
    _same(
        ours.groupby(["a", "b"]).size().unstack(fill_value=0),
        pf.groupby(["a", "b"]).size().unstack(fill_value=0),
    )
    _same(
        ours.groupby(["a", "b"])["c"].sum().unstack(),
        pf.groupby(["a", "b"])["c"].sum().unstack(),
    )


def test_unstack_is_a_frame_of_a_series_and_a_series_of_a_flat_frame():
    flat = DataFrame.from_pandas(FRAMES["onedups"].iloc[[0, 1]])
    assert isinstance(flat.unstack(), Series)
    assert isinstance(DataFrame.from_pandas(FRAMES["two"]).unstack(), DataFrame)
    assert isinstance(Series.from_pandas(SERIES["two"]).unstack(), DataFrame)


def test_unstack_labels_that_are_numbers_and_floats():
    for labels in ([1, 2, 1, 2], [1.5, 2.5, 1.5, 2.5], [True, False, True, False]):
        ps = pd.Series(
            [1.0, 2.0, 3.0, 4.0],
            index=pd.MultiIndex.from_arrays([[1, 1, 2, 2], labels], names=["a", "b"]),
        )
        _check(lambda o: o.unstack(), ps, Series.from_pandas(ps))
    pf = pd.DataFrame(
        {"u": [1.0, 2.0, 3.0, 4.0]},
        index=pd.MultiIndex.from_arrays([[1, 1, 2, 2], [1.5, 2.5, 1.5, 2.5]], names=["a", "b"]),
    )
    _check(lambda o: o.unstack(), pf, DataFrame.from_pandas(pf))


def test_unstack_of_a_large_column_set_orders_numbers_by_value():
    n = 25
    ps = pd.Series(
        list(range(n)),
        index=pd.MultiIndex.from_arrays([[1] * n, list(range(n - 1, -1, -1))], names=["a", "b"]),
    )
    _check(lambda o: o.unstack(), ps, Series.from_pandas(ps))


def _random_pairs(rows, cols, count, seed):
    """`count` distinct (row, column) labels in an order that is not sorted."""
    import numpy as np

    rng = np.random.default_rng(seed)
    pairs = pd.MultiIndex.from_arrays(
        [rng.integers(0, rows, count), rng.integers(0, cols, count)], names=["a", "b"]
    ).drop_duplicates()
    order = rng.permutation(len(pairs))
    return pairs[order], rng.random(len(pairs))


@pytest.mark.parametrize("seed", [1, 2])
def test_unstack_of_many_unsorted_cells_matches_pandas(seed):
    index, values = _random_pairs(70, 45, 4000, seed)
    ps = pd.Series(values, index=index, name="v")
    forms = [
        lambda o: o.unstack(),
        lambda o: o.unstack(0),
        lambda o: o.unstack(fill_value=-1.0),
    ]
    forms.append(lambda o: o.unstack(sort=False))
    for form in forms:
        _check(form, ps, Series.from_pandas(ps))
    pf = pd.DataFrame({"v": values, "w": (values * 100).astype("int64")}, index=index)
    _check(lambda o: o.unstack(), pf, DataFrame.from_pandas(pf))


def test_unstack_of_a_result_with_too_many_cells_is_refused():
    n = 50_000
    index = pd.MultiIndex.from_arrays([list(range(n)), list(range(n))], names=["a", "b"])
    ps = pd.Series(range(n), index=index)
    with pytest.raises(ValueError, match="too big"):
        Series.from_pandas(ps).unstack()


def test_unstack_with_many_columns_names_each_one():
    n = 3000
    ps = pd.Series(
        [float(i) for i in range(n)],
        index=pd.MultiIndex.from_arrays([[0] * n, list(range(n))], names=["a", "b"]),
    )
    got = Series.from_pandas(ps).unstack().to_pandas()
    assert list(got.columns) == list(range(n)) or list(got.columns) == [str(i) for i in range(n)]
    assert got.shape == (1, n)
    assert got.iloc[0, n - 1] == float(n - 1)


def test_unstack_a_positional_series_is_refused():
    with pytest.raises(ValueError):
        Series.from_list([1, 2, 3]).unstack()


def test_unstack_duplicates_message():
    with pytest.raises(ValueError, match="Index contains duplicate entries, cannot reshape"):
        Series.from_pandas(SERIES["dups"]).unstack()
    with pytest.raises(ValueError, match="Index contains duplicate entries, cannot reshape"):
        DataFrame.from_pandas(FRAMES["dups"]).unstack()


def test_unstack_integer_values_keep_their_type_when_no_cell_is_missing():
    got = Series.from_pandas(FRAMES["full"]["w"]).unstack()
    assert all(d == DType.INT64 for d in got.schema.values())
    got = Series.from_pandas(FRAMES["full"]["w"]).unstack(fill_value=0)
    assert all(d == DType.INT64 for d in got.schema.values())


def test_unstack_on_a_lazy_frame_collect_matches():
    ours = DataFrame.from_pandas(FRAMES["two"])
    assert ours.lazy().collect().unstack().to_pandas().equals(ours.unstack().to_pandas())


def test_empty_series_unstack():
    ps = pd.Series([], index=pd.MultiIndex.from_arrays([[], []], names=["a", "b"]), dtype="float64")
    _check(lambda o: o.unstack(), ps, Series.from_pandas(ps))


def test_sort_of_a_frame_whose_key_is_a_filtered_series_does_not_crash():
    """A filtered Series is a selection over its base; the sort kernel read its
    data pointer, which a selection has not."""
    base = Series.from_list([5, 3, 4, 1, 2, 0])
    mask = Series.from_list([False, True, False, True, True, False])
    frame = DataFrame.from_dict({"k": base.filter(mask), "v": Series.from_list([0, 1, 2])})
    assert frame.sort("k")["v"].to_list() == [1, 2, 0]
    assert frame.sort("k", descending=True)["v"].to_list() == [0, 2, 1]


def _labelled(values, index_pairs):
    index = pd.MultiIndex.from_arrays(list(zip(*index_pairs)), names=["a", "b"])
    pandas_series = pd.Series(values, index=index, dtype=object)
    frame = DataFrame.from_dict(
        {
            "a": [p[0] for p in index_pairs],
            "b": [p[1] for p in index_pairs],
            "v": values,
        }
    ).set_index(["a", "b"])
    return pandas_series, frame["v"]


PAIRS = [(1, "x"), (1, "y"), (2, "x")]


@pytest.mark.parametrize(
    "values, fill",
    [(["p", None, "r"], "q"), ([True, None, False], True), ([True, None, False], False)],
    ids=["text-null", "bool-null-true", "bool-null-false"],
)
def test_fill_value_on_a_text_or_bool_column_that_holds_a_null(values, fill):
    ps, ours = _labelled(values, PAIRS)
    # the pair (2, "y") is missing, so the fill lands there; the held null stays
    for args in ((), (0,)):
        want = ps.unstack(*args, fill_value=fill)
        got = ours.unstack(*args, fill_value=fill).to_pandas()
        assert list(got.index.names) == list(want.index.names)
        assert list(got.columns) == [str(c) for c in want.columns]
        for g, w in zip(got.columns, want.columns):
            assert [_norm(x) for x in got[g].tolist()] == [_norm(x) for x in want[w].tolist()]


@pytest.mark.parametrize("fill", [0, 2.5, -1])
def test_a_number_fill_on_a_bool_column_is_refused(fill):
    for values in ([True, False, True], [True, None, False]):
        _, ours = _labelled(values, PAIRS)
        with pytest.raises(TypeError):
            ours.unstack(fill_value=fill)
    frame = DataFrame.from_dict({"a": [1, 1, 2], "b": ["x", "y", "x"], "v": [True, False, True]})
    with pytest.raises(TypeError):
        frame.set_index(["a", "b"]).unstack(fill_value=fill)


def test_a_bool_fill_on_a_number_column_is_refused():
    _, ours = _labelled([1.0, 2.0, 3.0], PAIRS)
    with pytest.raises(TypeError):
        ours.unstack(fill_value=True)


def _stacked_values(pf, **kw):
    skip_below_pandas3()
    want = pf.stack(**kw)
    got = DataFrame.from_pandas(pf).stack(**kw).to_pandas()
    assert _labels(got.index) == _labels(want.index)
    assert [_norm(x) for x in got.tolist()] == [_norm(x) for x in want.tolist()]


def test_stack_with_an_all_null_column_next_to_numbers():
    pf = pd.DataFrame({"u": [1.0, 2.0], "w": [None, None]}, index=pd.Index([1, 2], name="k"))
    _stacked_values(pf, future_stack=True)
    _stacked_values(pf, future_stack=False)
    _stacked_values(pf, future_stack=False, dropna=False)
    _stacked_values(pd.DataFrame({"u": [1, 2], "w": [None, None]}), future_stack=True)


def test_a_view_string_level_converts_to_the_pandas_string_dtype():
    # The column names of an unstacked frame are a view string level, which
    # pyarrow 17 cannot cast to the large_string of pandas' string dtype; it
    # is made flat natively.
    skip_below_pandas3()
    got = DataFrame.from_pandas(FRAMES["none"]).unstack().to_pandas()
    _same(got, FRAMES["none"].unstack())
