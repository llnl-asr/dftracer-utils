# SPDX-License-Identifier: MIT
"""Label lookup (loc, at, xs, get, s[label]) on a Series and a DataFrame: every
form runs on real pandas and on ours over the same labels (unique and repeated,
sorted, unsorted and decreasing, NaN, mixed int and float, text, MultiIndex)
and the values, labels and errors are compared. A frame answers a whole label
with a one-row frame, where pandas answers with a Series: the pandas side of
those cases asks for the same rows with a list."""

import datetime
import warnings

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning")

NAN = float("nan")

INDEXES = {
    "unsorted": [30, 10, 20, 40, 50],
    "sorted": [10, 20, 30, 40, 50],
    "decreasing": [50, 40, 30, 20, 10],
    "dup sorted": [1, 1, 2, 3, 3],
    "dup unsorted": [2, 1, 2, 3, 1],
    "text": ["b", "a", "c", "d", "e"],
    "nan": [1.0, NAN, 3.0, 4.0, 5.0],
    "float": [1.0, 2.5, 3.0, 4.0, 5.0],
}
# A label that is there, one that is not, and one of another type.
LABELS = {
    "unsorted": (30, 999, "x"),
    "sorted": (30, 999, "x"),
    "decreasing": (30, 999, "x"),
    "dup sorted": (1, 2, 999),
    "dup unsorted": (1, 2, 999),
    "text": ("b", "z", 5),
    "nan": (3.0, NAN, 999.0),
    "float": (2.5, 3, 999),
}
BOUNDS = {
    "unsorted": (10, 30),
    "sorted": (20, 40),
    "decreasing": (40, 20),
    "dup sorted": (1, 2),
    "dup unsorted": (1, 2),
    "float": (2.5, 4),
}
TUPLES = {
    "sorted": [("a", 1), ("a", 2), ("b", 1), ("b", 2), ("c", 1)],
    "unsorted": [("b", 2), ("a", 1), ("c", 1), ("a", 2), ("b", 1)],
    "dup": [("a", 1), ("a", 1), ("b", 1), ("b", 2), ("c", 1)],
}


def _series(index):
    values = [float(i) + 0.5 for i in range(5)]
    if isinstance(index, list) and index and isinstance(index[0], tuple):
        return pd.Series(values, index=pd.MultiIndex.from_tuples(index, names=["g", "n"]), name="v")
    return pd.Series(values, index=pd.Index(index, name="k"), name="v")


def _frame(index):
    data = {"a": [float(i) + 0.5 for i in range(5)], "b": [i * 10 for i in range(5)]}
    if isinstance(index, list) and index and isinstance(index[0], tuple):
        return pd.DataFrame(data, index=pd.MultiIndex.from_tuples(index, names=["g", "n"]))
    return pd.DataFrame(data, index=pd.Index(index, name="k"))


def _kind(e):
    for base in (KeyError, TypeError, ValueError, IndexError):
        if isinstance(e, base):
            return base.__name__
    return type(e).__name__


def _run(f):
    try:
        with warnings.catch_warnings():
            # The index is unsorted on purpose; pandas warns about the lookup.
            warnings.filterwarnings("ignore", message=".*past lexsort depth.*")
            return f()
    except Exception as e:  # noqa: BLE001
        return e


def _to_pandas(r):
    return r.to_pandas() if hasattr(r, "to_pandas") else r


def _check(ours, ref):
    if isinstance(ref, Exception):
        assert isinstance(ours, Exception), f"pandas raised {ref!r}; ours returned {ours!r}"
        assert _kind(ours) == _kind(ref), (ours, ref)
        return
    assert not isinstance(ours, Exception), f"pandas returned; ours raised {ours!r}"
    ours = _to_pandas(ours)
    if isinstance(ref, pd.DataFrame):
        assert list(ours.index.names) == list(ref.index.names)
        assert str(ours.index.tolist()) == str(ref.index.tolist())
        pd.testing.assert_frame_equal(
            ours, ref, check_dtype=False, check_index_type=False, check_names=False
        )
    elif isinstance(ref, pd.Series):
        assert list(ours.index.names) == list(ref.index.names)
        assert str(ours.index.tolist()) == str(ref.index.tolist())
        pd.testing.assert_series_equal(
            ours, ref, check_dtype=False, check_index_type=False, check_names=False
        )
        # A row Series is named by its label; a name here is text or none.
        assert ours.name == (ref.name if isinstance(ref.name, str) or ref.name is None else None)
    else:
        assert ours == ref or (ours != ours and ref != ref), (ours, ref)


def _ours(p):
    return DataFrame.from_pandas(p) if isinstance(p, pd.DataFrame) else Series.from_pandas(p)


def _case(p, fn, pfn=None):
    _check(_run(lambda: fn(_ours(p))), _run(lambda: (pfn or fn)(p)))


def _cases(name, frame):
    """(id, ours, pandas or None) for every form on the index `name`."""
    a, b, c = LABELS[name]
    yield "scalar", (lambda s: s.loc[a]), None
    yield "scalar missing", (lambda s: s.loc[b]), None
    yield "scalar of another type", (lambda s: s.loc[c]), None
    yield "list", (lambda s: s.loc[[a, a]]), None
    yield "list with a missing label", (lambda s: s.loc[[a, b]]), None
    yield (
        "list order and repeats",
        (lambda s: s.loc[[a] + ([a] if name.startswith("dup") else [])]),
        None,
    )
    yield "mask list", (lambda s: s.loc[[True, False, True, False, True]]), None
    yield "mask", ((lambda s: s.loc[s["a"] > 1.0]) if frame else (lambda s: s.loc[s > 1.0])), None
    yield "at", ((lambda s: s.at[a, "a"]) if frame else (lambda s: s.at[a])), None
    yield "at missing", ((lambda s: s.at[b, "a"]) if frame else (lambda s: s.at[b])), None
    yield "xs", (lambda s: s.xs(a)), None
    if frame:
        yield "loc column", (lambda s: s.loc[a, "b"]), None
        yield "loc columns", (lambda s: s.loc[[a], ["b", "a"]]), None
        yield "loc all rows", (lambda s: s.loc[:, "a"]), None
    else:
        yield "get", (lambda s: s.get(a)), None
        yield "get missing", (lambda s: s.get(b, -1)), None
        yield "getitem", (lambda s: s[a]), None
    if name in BOUNDS:
        lo, hi = BOUNDS[name]
        yield "slice", (lambda s: s.loc[lo:hi]), None
        yield "slice open start", (lambda s: s.loc[:hi]), None
        yield "slice open end", (lambda s: s.loc[lo:]), None
        yield "slice reversed", (lambda s: s.loc[hi:lo]), None
        yield "slice past the ends", (lambda s: s.loc[lo - 5 : hi + 100]), None
        yield "slice bounds not labels", (lambda s: s.loc[lo + 1 : hi + 1]), None
    if name == "text":
        yield "slice of text", (lambda s: s.loc["a":"c"]), None
    if name == "nan":
        yield "NaN label", (lambda s: s.loc[NAN]), None
        yield "NaN in a list", (lambda s: s.loc[[NAN, 3.0]]), None
    if name in ("unsorted", "sorted"):
        yield "float for an integer label", (lambda s: s.loc[30.0]), None
        yield "float that is no label", (lambda s: s.loc[30.5]), None
    if name == "float":
        yield "integer for a float label", (lambda s: s.loc[3]), None


def _params():
    for frame in (False, True):
        for name in INDEXES:
            for case_id, fn, pfn in _cases(name, frame):
                yield pytest.param(
                    frame, name, fn, pfn, id=f"{'frame' if frame else 'series'}-{name}-{case_id}"
                )


@pytest.mark.parametrize("frame,name,fn,pfn", list(_params()))
def test_label_lookup_matches_pandas(frame, name, fn, pfn):
    index = INDEXES[name]
    _case(_frame(index) if frame else _series(index), fn, pfn)


def _multi_cases(frame):
    yield "full key", (lambda s: s.loc[("a", 1)]), None
    yield "partial key", (lambda s: s.loc["a"]), None
    yield "partial tuple", (lambda s: s.loc[("b",)]), None
    yield "missing full key", (lambda s: s.loc[("z", 1)]), None
    yield "missing partial key", (lambda s: s.loc["z"]), None
    yield "list of tuples", (lambda s: s.loc[[("a", 2), ("b", 1)]]), None
    yield "list of tuples with a missing one", (lambda s: s.loc[[("a", 2), ("z", 1)]]), None
    yield "xs partial", (lambda s: s.xs("a")), None
    yield "xs level", (lambda s: s.xs(1, level="n")), None
    yield "xs level kept", (lambda s: s.xs(1, level="n", drop_level=False)), None
    yield "xs first level", (lambda s: s.xs("a", level=0)), None
    yield "slice", (lambda s: s.loc[("a", 2) : ("b", 1)]), None
    yield "partial slice", (lambda s: s.loc["a":"b"]), None
    yield "mask", ((lambda s: s.loc[s["a"] > 2.0]) if frame else (lambda s: s.loc[s > 2.0])), None
    yield "at", ((lambda s: s.at[("a", 2), "a"]) if frame else (lambda s: s.at[("a", 2)])), None
    if not frame:
        yield "get", (lambda s: s.get(("a", 2))), None
        yield "get missing", (lambda s: s.get(("z", 2), -1)), None


@pytest.mark.parametrize("frame", [False, True], ids=["series", "frame"])
@pytest.mark.parametrize("kind", ["sorted", "unsorted", "dup"])
def test_multiindex_lookup_matches_pandas(frame, kind):
    p = (_frame if frame else _series)(TUPLES[kind])
    for case_id, fn, pfn in _multi_cases(frame):
        if kind == "dup" and case_id == "at":
            continue  # pandas refuses a scalar read on a repeated MultiIndex key
        try:
            _case(p, fn, pfn)
        except AssertionError as e:
            raise AssertionError(f"{kind} {case_id}: {e}") from None


def test_a_positional_series_and_frame_have_the_positions_as_labels():
    p = pd.Series([1.0, 2.0, 3.0], name="p")
    s = Series.from_pandas(p)
    assert s.loc[1] == 1.0 + 1 == s.at[1] == s.get(1)
    assert s.get(7, "none") == "none"
    assert s.get(-1, "none") == "none"
    for fn in (lambda x: x.loc[[2, 0]], lambda x: x.loc[0:1], lambda x: x.loc[[True, False, True]]):
        _case(p, fn)
    with pytest.raises(KeyError):
        s.loc[7]
    f = DataFrame({"a": [1.0, 2.0, 3.0]})
    assert f.at[1, "a"] == 2.0
    with pytest.raises(KeyError):
        f.loc[9]
    assert f.loc[0:1]["a"].to_list() == [1.0, 2.0]


def test_a_series_integer_key_is_a_label_not_a_position_on_a_labelled_series():
    s = Series.from_pandas(pd.Series([1.5, 2.5, 3.5], index=["x", "y", "z"]))
    with pytest.raises(KeyError):
        s[0]
    assert s["y"] == 2.5
    assert s.iloc[0] == 1.5
    assert s[0:2].to_list() == [1.5, 2.5]  # an integer slice is positional
    assert s["x":"y"].to_list() == [1.5, 2.5]


def test_assignment_through_labels_rebinds_the_frame():
    p = pd.DataFrame({"a": [1.0, 2.0, 3.0, 4.0]}, index=pd.Index([10, 20, 20, 30], name="k"))
    d = DataFrame.from_pandas(p)
    d.loc[20, "a"] = 0.0
    p.loc[20, "a"] = 0.0
    d.at[30, "a"] = 9.0
    p.at[30, "a"] = 9.0
    d.loc[[10, 30], "a"] = -1.0
    p.loc[[10, 30], "a"] = -1.0
    assert d["a"].to_list() == p["a"].tolist()
    with pytest.raises(KeyError, match="99"):
        d.at[99, "a"] = 1.0
    d.loc[99, "a"] = 1.0
    p.loc[99, "a"] = 1.0
    assert d.index.to_list() == p.index.tolist() and d["a"].to_list() == p["a"].tolist()


def test_a_timestamp_index_looks_up_by_datetime():
    stamps = pd.to_datetime(["2020-01-01", "2020-01-02", "2020-01-03"])
    p = pd.Series([1.0, 2.0, 3.0], index=pd.Index(stamps, name="t"), name="v")
    s = Series.from_pandas(p)
    when = datetime.datetime(2020, 1, 2)
    assert s.loc[when] == 2.0
    assert s.loc[[when, datetime.datetime(2020, 1, 1)]].to_list() == [2.0, 1.0]
    assert s.loc[when:].to_list() == [2.0, 3.0]
    with pytest.raises(KeyError):
        s.loc[datetime.datetime(2021, 1, 1)]
    f = DataFrame.from_pandas(p.to_frame("a"))
    assert f.loc[when, "a"] == 2.0
    assert f.loc[when].to_list() == [2.0]


def test_a_list_of_mixed_label_types_finds_what_each_label_finds():
    s = Series.from_pandas(pd.Series([1.0, 2.0, 3.0], index=[1, 2, 3]))
    with pytest.raises(KeyError):
        s.loc[[1, "a"]]
    assert s.get([1, "a"], "none") == "none"


def test_a_bool_is_not_a_label_of_numbers_and_a_number_is_not_one_of_bools():
    for index in ([0, 1], [0.0, 1.0]):
        p = pd.Series([1.0, 2.0], index=index)
        _case(p, lambda x: x.loc[True])
        _case(p, lambda x: x.loc[[True, 5]])
    b = pd.Series([1.0, 2.0], index=[True, False])
    _case(b, lambda x: x.loc[True])
    _case(b, lambda x: x.loc[1])
    _case(b, lambda x: x.loc[1.0])
    _case(b, lambda x: x.loc[[1]])
    d = DataFrame.from_pandas(pd.DataFrame({"a": [1.0, 2.0]}, index=pd.Index([0, 1], name="k")))
    with pytest.raises(KeyError):
        d.loc[True]


def test_a_timestamp_key_must_agree_with_the_timezone_of_the_labels():
    naive = pd.Timestamp("2020-01-01 00:00")
    aware = pd.Timestamp("2020-01-01 00:00", tz="UTC")
    for index, key in (
        (pd.DatetimeIndex([aware]), naive),
        (pd.DatetimeIndex([naive]), aware),
        (pd.DatetimeIndex([aware]), aware),
        (pd.DatetimeIndex([naive]), naive),
        (pd.DatetimeIndex([aware]), aware.tz_convert("America/Chicago")),
    ):
        p = pd.Series([1.0], index=index)
        _case(p, lambda x, k=key: x.loc[k])
        _case(p, lambda x, k=key: x.loc[[k]])
    f = pd.DataFrame({"a": [1.0]}, index=pd.DatetimeIndex([aware]))
    with pytest.raises(KeyError):
        DataFrame.from_pandas(f).loc[naive]


def test_a_float_key_is_cast_to_a_float32_label_type():
    p = pd.Series([1.0, 2.0], index=pd.Index([0.1, 0.2], dtype="float32"))
    _case(p, lambda x: x.loc[0.1])
    _case(p, lambda x: x.loc[0.2])
    _case(p, lambda x: x.loc[[0.1]])  # a list is not cast, as pandas
    d = DataFrame.from_pandas(p.rename("a").to_frame())
    assert d.loc[0.2, "a"] == 2.0


def test_none_finds_null_labels_of_a_text_level_only():
    nan = float("nan")
    _case(pd.Series([1.0, 2.0], index=[nan, 1.0]), lambda x: x.loc[None])
    _case(pd.Series([1.0, 2.0], index=[nan, 1.0]), lambda x: x.loc[nan])
    _case(pd.Series([1.0, 2.0], index=[nan, 1.0]), lambda x: x.loc[[None]])
    _case(pd.Series([1.0, 2.0], index=[None, "a"]), lambda x: x.loc[None])


def test_a_labelled_series_from_a_frame_looks_up_by_the_frames_labels():
    p = pd.DataFrame({"a": [1.0, 2.0, 3.0]}, index=pd.Index([5, 6, 7], name="k"))
    d = DataFrame.from_pandas(p)
    assert d["a"].loc[6] == 2.0
    assert d["a"].loc[[7, 5]].to_list() == [3.0, 1.0]
    assert d["a"].xs(7) == 3.0
