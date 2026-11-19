# SPDX-License-Identifier: MIT
"""Group-by results that pandas labels with the group keys in front (apply,
take, nlargest, nsmallest, value_counts): every form runs on real pandas
(``group_keys=True``, ``include_groups=False``) and on ours over the same frame,
indexed by one level, by two, and not indexed, grouped by one key and by two,
and the values, labels, level names and errors are compared."""

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

PANDAS3 = int(pd.__version__.split(".")[0]) >= 3
PANDAS31 = tuple(int(p) for p in pd.__version__.split(".")[:2]) >= (3, 1)

BASE = pd.DataFrame(
    {
        "g": ["b", "a", "b", "a", "a", "c", "c"],
        "h": [1, 1, 2, 1, 2, 1, 1],
        "v": [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0],
        "w": [10, 20, 30, 40, 50, 60, 70],
    }
)
FRAMES = {
    "plain": BASE,
    "one": BASE.set_index(pd.Index([10, 20, 30, 40, 50, 60, 70], name="k")),
    "two": BASE.set_index(
        pd.MultiIndex.from_arrays([[10, 20, 30, 40, 50, 60, 70], list("abcdefg")], names=["k", "j"])
    ),
    "unnamed": BASE.set_index(pd.Index([5, 4, 3, 2, 1, 0, 9])),
}
KEYS = {"g": "g", "g+h": ["g", "h"]}


def _norm(v):
    if isinstance(v, tuple):
        return tuple(_norm(i) for i in v)
    return None if v is None or (isinstance(v, float) and v != v) else v


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


def _same(x, y, ordered=True):
    if isinstance(y, Exception):
        assert isinstance(x, Exception), f"ours returned, pandas raised {y!r}"
        assert _kind(x) == _kind(y), f"{x!r} vs {y!r}"
        return
    assert not isinstance(x, Exception), f"ours raised {x!r}"
    x = x.to_pandas()
    if not ordered:
        x, y = x.sort_index(), y.sort_index()
    assert list(x.index.names) == list(y.index.names)
    assert [_norm(i) for i in x.index.tolist()] == [_norm(i) for i in y.index.tolist()]
    if isinstance(y, pd.DataFrame):
        assert list(x.columns) == list(y.columns)
        # the name of the column axis is not kept (the index names are compared above)
        pd.testing.assert_frame_equal(
            x,
            y,
            check_dtype=False,
            check_index_type=False,
            check_column_type=False,
            check_names=False,
        )
    else:
        pd.testing.assert_series_equal(x, y, check_dtype=False, check_index_type=False)


def _check(fn, name, ordered=True):
    pandas_frame = FRAMES[name]
    _same(
        _outcome(fn, DataFrame.from_pandas(pandas_frame)),
        _outcome(fn, pandas_frame),
        ordered,
    )


def _apply(func, **kw):
    """``groupby(keys).apply(func)``: ours and pandas, whatever the object."""

    def run(frame, keys):
        grouped = frame.groupby(keys, **kw)
        if isinstance(frame, pd.DataFrame):
            return grouped.apply(func, include_groups=False)
        return grouped.apply(func)

    return run


APPLY = {
    "head": lambda x: x.head(1),
    "head2": lambda x: x.head(2),
    "tail": lambda x: x.tail(1),
    "filter": lambda x: x[x["v"] > 1.5],
    "series": lambda x: x["v"] - x["v"].mean(),
    "series scaled": lambda x: x["v"] * 2,
    "sorted series": lambda x: x["v"].sort_values(ascending=False),
    "frame sorted": lambda x: x.sort_values("v", ascending=False),
    "identity": lambda x: x,
    "wide": lambda x: _wide(x),
}


def _wide(x):
    """A Series labelled the same for every group: pandas gives one row for each."""
    total = [x["v"].sum(), x["w"].sum()]
    if isinstance(x, pd.DataFrame):
        return pd.Series(total, index=["v", "w"], name="t")
    return Series.from_list(total).set_axis(["v", "w"]).rename("t")


@pytest.mark.parametrize("name", FRAMES)
@pytest.mark.parametrize("keys", KEYS)
@pytest.mark.parametrize("form", APPLY)
def test_apply(name, keys, form):
    run = _apply(APPLY[form])
    _check(lambda f: run(f, KEYS[keys]), name)


@pytest.mark.parametrize("name", ["plain", "one"])
@pytest.mark.parametrize("form", ["head", "series", "sorted series", "identity", "wide"])
def test_apply_with_sort_false_goes_in_order_of_first_appearance(name, form):
    run = _apply(APPLY[form], sort=False)
    _check(lambda f: run(f, "g"), name)


@pytest.mark.parametrize("name", ["plain", "one"])
@pytest.mark.parametrize("form", ["head", "series", "sorted series", "identity"])
def test_apply_without_the_keys_as_index_keeps_the_row_labels(name, form):
    if not PANDAS3:
        pytest.skip("pandas 2 labels as_index=False apply results differently")
    run = _apply(APPLY[form], as_index=False)
    _check(lambda f: run(f, "g"), name)


@pytest.mark.parametrize("name", FRAMES)
@pytest.mark.parametrize("keys", KEYS)
@pytest.mark.parametrize(
    "indices", [[0], [0, 1], [-1], [1, 0], [0, 0], [0, -1], [5], [-5]], ids=str
)
def test_take(name, keys, indices):
    _check(lambda f: f.groupby(KEYS[keys]).take(indices), name)


@pytest.mark.parametrize("name", FRAMES)
@pytest.mark.parametrize("keys", KEYS)
@pytest.mark.parametrize("method", ["nlargest", "nsmallest"])
@pytest.mark.parametrize("n", [0, 1, 2, 5])
def test_top(name, keys, method, n):
    _check(lambda f: getattr(f.groupby(KEYS[keys])["v"], method)(n), name)


@pytest.mark.parametrize("name", ["plain", "one"])
@pytest.mark.parametrize("method", ["nlargest", "nsmallest"])
def test_top_of_an_integer_column(name, method):
    _check(lambda f: getattr(f.groupby("g")["w"], method)(2), name)


@pytest.mark.parametrize("name", FRAMES)
@pytest.mark.parametrize("keys", KEYS)
@pytest.mark.parametrize("form", ["take", "top", "value_counts"])
def test_without_the_keys_as_index(name, keys, form):
    if not PANDAS3:
        pytest.skip("pandas 2 labels as_index=False results differently")
    if form == "top" and not PANDAS31:
        pytest.skip("pandas before 3.1 gives as_index=False nlargest a frame")
    forms = {
        "take": lambda g: g.take([0, 1]),
        "top": lambda g: g["v"].nlargest(1),
        "value_counts": lambda g: g.value_counts(),
    }
    _check(lambda f: forms[form](f.groupby(KEYS[keys], as_index=False)), name, ordered=False)


@pytest.mark.parametrize("name", FRAMES)
@pytest.mark.parametrize("keys", KEYS)
def test_value_counts(name, keys):
    _check(lambda f: f.groupby(KEYS[keys]).value_counts(), name, ordered=False)
    _check(lambda f: f.groupby(KEYS[keys])["h"].value_counts(), name, ordered=False)


def test_the_keys_are_left_out_of_the_group_frames():
    ours = DataFrame.from_pandas(BASE)
    seen = []

    def note(frame):
        seen.append(frame.columns)
        return frame.head(1)

    ours.groupby("g").apply(note)
    assert seen and all(columns == ["h", "v", "w"] for columns in seen)
    # the Polars spelling passes every column and keeps the keys as columns
    seen.clear()
    out = ours.group_by("g").apply(note)
    assert seen and all(columns == ["g", "h", "v", "w"] for columns in seen)
    assert "g" in out.columns


def test_a_result_column_named_like_a_key_is_refused():
    ours = DataFrame.from_pandas(BASE)
    with pytest.raises(ValueError):
        ours.groupby("g").apply(lambda x: x.head(1).with_column("g", x["v"].head(1)))


SCALARS = {
    "traced sum": lambda x: x["w"].sum(),
    "traced mean": lambda x: x["v"].mean(),
    "python scalar": lambda x: (
        float(x["w"].to_list()[0]) if isinstance(x["w"], Series) else x["w"].iloc[0]
    ),
    "list": lambda x: list(x.columns),
    "sum of columns": lambda x: x[["w"]].sum(),
    "sum of two columns": lambda x: x[["v", "w"]].sum(),
    "mean of columns": lambda x: x[["v", "w"]].mean(),
}


@pytest.mark.parametrize("name", ["plain", "one"])
@pytest.mark.parametrize("keys", KEYS)
@pytest.mark.parametrize("form", SCALARS)
def test_apply_of_a_scalar_a_list_or_a_reduction(name, keys, form):
    run = _apply(SCALARS[form])
    _check(lambda f: run(f, KEYS[keys]), name)


@pytest.mark.parametrize("name", ["plain", "one"])
@pytest.mark.parametrize("sort", [True, False])
@pytest.mark.parametrize("ascending", [False, True])
def test_value_counts_follows_sort(name, sort, ascending):
    frame = pd.DataFrame(
        {"g": list("bbbaaacc"), "h": [1, 1, 2, 1, 2, 2, 3, 4], "w": [1, 1, 1, 2, 2, 2, 3, 3]}
    )
    if name == "one":
        frame.index = pd.Index(list("abcdefgh"), name="k")
    ours = DataFrame.from_pandas(frame)
    for cols in (["h"], ["h", "w"]):
        _same(
            ours.groupby("g", sort=sort)[cols].value_counts(ascending=ascending),
            frame.groupby("g", sort=sort)[cols].value_counts(ascending=ascending),
        )


def test_nlargest_result_is_a_named_series():
    ours = DataFrame.from_pandas(BASE)
    got = ours.groupby("g")["v"].nlargest(1)
    assert isinstance(got, Series) and got.name == "v"
    assert got.index_names() == ["g", "__index_level_1__"]


@pytest.mark.parametrize("name", FRAMES)
@pytest.mark.parametrize("keys", KEYS)
@pytest.mark.parametrize(
    "pick, n",
    [(p, n) for p in ("head", "tail") for n in (0, 1, 2, 5)]
    + [("nth", n) for n in (0, 1, 2, 5, -1, -2, -5)],
)
def test_head_tail_nth_keep_their_labels_in_input_order(name, keys, pick, n):
    _check(lambda f: getattr(f.groupby(KEYS[keys]), pick)(n), name)
