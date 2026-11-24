# SPDX-License-Identifier: MIT
"""reindex, reindex_like and align on a Series and a DataFrame: every form runs
on real pandas and on ours over the same labels (unique, repeated, sorted,
unsorted and decreasing, NaN, text, MultiIndex, none) and the values, labels,
column names and errors are compared."""

import datetime
import math

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, Series  # noqa: E402

from .common import skip_below_pandas3  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

PANDAS31 = tuple(int(p) for p in pd.__version__.split(".")[:2]) >= (3, 1)
PANDAS30 = not PANDAS31
NAN = float("nan")

INDEXES = {
    "sorted": [10, 20, 30],
    "unsorted": [30, 10, 20],
    "decreasing": [30, 20, 10],
    "dup": [1, 1, 2],
    "text": ["b", "d", "f"],
    "float": [1.5, 2.5, 3.5],
    "nan": [1.0, NAN, 3.0],
}
TARGETS = {
    "sorted": [20, 40, 10],
    "unsorted": [20, 40, 10],
    "decreasing": [20, 40, 10],
    "dup": [1, 2, 9],
    "text": ["d", "z", "b"],
    "float": [2.5, 9.0, 1.5],
    "nan": [3.0, 9.0, NAN],
}
SAME = {
    "sorted": [10, 20, 30],
    "dup": [1, 1, 2],
}
# Targets for the fill methods against sorted [10, 20, 30].
FILL_TARGETS = {
    "around": [5, 10, 15, 25, 35, 40],
    "dense": [10, 11, 12, 13, 20, 21, 22],
    "low": [8, 9, 10, 18, 19, 20],
    "far": [11, 12, 13, 21, 22, 23, 40],
    "ties": [15, 25],
    "unsorted": [25, 5, 15, 35],
    "dups": [10, 10, 15, 15, 25],
}


def _kind(e):
    for base in (KeyError, NotImplementedError, TypeError, ValueError, IndexError):
        if isinstance(e, base):
            return base.__name__
    return type(e).__name__


def _run(f):
    try:
        return f()
    except Exception as e:  # noqa: BLE001
        return e


def _same(a, b):
    if isinstance(a, float) and isinstance(b, float) and math.isnan(a) and math.isnan(b):
        return True
    return a == b


def _numeric_kind(dtype):
    if pd.api.types.is_bool_dtype(dtype):
        return "bool"
    if pd.api.types.is_integer_dtype(dtype):
        return "int"
    if pd.api.types.is_float_dtype(dtype):
        return "float"
    return "other"


def _compare(ours, ref):
    if isinstance(ref, tuple):
        assert isinstance(ours, tuple) and len(ours) == len(ref)
        for o, r in zip(ours, ref):
            _compare(o, r)
        return
    ours = ours.to_pandas() if hasattr(ours, "to_pandas") else ours
    if isinstance(ref, pd.DataFrame):
        assert isinstance(ours, pd.DataFrame)
        assert list(ours.columns) == list(ref.columns)
        assert str(ours.index.tolist()) == str(ref.index.tolist())
        assert list(ours.index.names) == list(ref.index.names)
        pd.testing.assert_frame_equal(
            ours, ref, check_dtype=False, check_index_type=False, check_column_type=False
        )
        assert [_numeric_kind(t) for t in ours.dtypes] == [_numeric_kind(t) for t in ref.dtypes]
    elif not isinstance(ref, pd.Series):
        assert _same(ours, ref), (ours, ref)
    else:
        assert isinstance(ours, pd.Series)
        assert str(ours.index.tolist()) == str(ref.index.tolist())
        assert list(ours.index.names) == list(ref.index.names)
        pd.testing.assert_series_equal(
            ours, ref, check_dtype=False, check_index_type=False, check_names=False
        )
        assert ours.name == ref.name
        assert _numeric_kind(ours.dtype) == _numeric_kind(ref.dtype)


def _wrap(x):
    if isinstance(x, pd.DataFrame):
        return DataFrame.from_pandas(x)
    if isinstance(x, pd.Series):
        return Series.from_pandas(x)
    return x


def check(make, call, *, dtypes=True):
    """Run ``call`` on the pandas objects ``make()`` builds and on ours."""
    skip_below_pandas3()
    pandas_objects = make()
    ref = _run(lambda: call(*pandas_objects))
    ours = _run(lambda: call(*[_wrap(x) for x in make()]))
    if isinstance(ref, Exception):
        assert isinstance(ours, Exception), f"pandas raised {ref!r}; ours returned {ours!r}"
        assert _kind(ours) == _kind(ref), (ours, ref)
        return ours
    assert not isinstance(ours, Exception), f"pandas returned; ours raised {ours!r}"
    _compare(ours, ref)
    return ours


def _s(index, name="v"):
    return pd.Series(list(range(1, len(index) + 1)), index=pd.Index(index, name="k"), name=name)


def _f(index):
    n = len(index)
    return pd.DataFrame(
        {
            "a": list(range(1, n + 1)),
            "b": [i + 0.5 for i in range(1, n + 1)],
            "c": ["x", "y", "z", "w"][:n],
        },
        index=pd.Index(index, name="k"),
    )


MI = pd.MultiIndex.from_tuples([(1, "a"), (1, "b"), (2, "a")], names=["x", "y"])


# ---- Series.reindex -------------------------------------------------------------


@pytest.mark.parametrize("kind", INDEXES)
def test_series_reindex_labels(kind):
    check(lambda: (_s(INDEXES[kind]),), lambda s: s.reindex(TARGETS[kind]))


@pytest.mark.parametrize("kind", INDEXES)
def test_series_reindex_index_keyword(kind):
    check(lambda: (_s(INDEXES[kind]),), lambda s: s.reindex(index=TARGETS[kind]))


@pytest.mark.parametrize("kind", SAME)
def test_series_reindex_same_labels_even_when_repeated(kind):
    check(lambda: (_s(SAME[kind]),), lambda s: s.reindex(SAME[kind]))


def test_series_reindex_repeated_labels_raise():
    ours = check(lambda: (_s([1, 1, 2]),), lambda s: s.reindex([1, 2]))
    assert "duplicate labels" in str(ours)
    check(lambda: (_s([1, 1, 2]),), lambda s: s.reindex([1, 2, 3], method="ffill"))


@pytest.mark.parametrize(
    "fill",
    [
        0,
        1.5,
        7,
        -1,
        pytest.param(
            0.0,
            marks=pytest.mark.skipif(
                not PANDAS31, reason="pandas before 3.1 widens an integer column to float64"
            ),
        ),
    ],
)
def test_series_reindex_fill_value(fill):
    check(lambda: (_s([10, 20, 30]),), lambda s: s.reindex([20, 40, 10], fill_value=fill))
    check(lambda: (_s([10, 20, 30]),), lambda s: s.reindex([20, 10], fill_value=fill))


def test_series_reindex_fill_value_float_and_text_columns():
    check(
        lambda: (pd.Series([1.5, NAN, 3.5], index=[1, 2, 3]),),
        lambda s: s.reindex([2, 9], fill_value=0),
    )
    check(
        lambda: (pd.Series(["a", "b"], index=[1, 2]),),
        lambda s: s.reindex([1, 9], fill_value="m"),
    )
    check(
        lambda: (pd.Series(["a", "b"], index=[1, 2]),),
        lambda s: s.reindex([1, 9]),
    )
    check(
        lambda: (pd.Series([True, False], index=[1, 2]),),
        lambda s: s.reindex([1, 9], fill_value=True),
    )
    check(
        lambda: (pd.Series([1, 2], index=[1, 2], dtype="int32"),),
        lambda s: s.reindex([1, 9], fill_value=5),
    )


def test_series_reindex_int_widens_to_float():
    out = Series.from_pandas(_s([10, 20, 30])).reindex([20, 40])
    assert out.dtype.name == "FLOAT64"
    kept = Series.from_pandas(_s([10, 20, 30])).reindex([30, 10])
    assert kept.dtype.name == "INT64"


def test_series_reindex_keeps_the_target_label_type():
    check(lambda: (_s([10, 20, 30]),), lambda s: s.reindex([10.0, 20.5]))
    check(lambda: (_s([10, 20, 30]),), lambda s: s.reindex(["a", "b"]))
    check(lambda: (_s([10, 20, 30]),), lambda s: s.reindex([]))


def test_series_reindex_target_names():
    check(lambda: (_s([1, 2]),), lambda s: s.reindex(pd.Index([2, 3], name="z")))
    check(lambda: (_s([1, 2]),), lambda s: s.reindex(pd.Index([2, 3])))
    check(lambda: (_s([1, 2]),), lambda s: s.reindex(pd.Series([2, 3], name="q")))


def test_series_reindex_positional():
    check(lambda: (pd.Series([1, 2, 3]),), lambda s: s.reindex([2, 0, 5]))
    check(lambda: (pd.Series([1, 2, 3]),), lambda s: s.reindex([2, 0, 5], method="ffill"))
    check(lambda: (pd.Series([1, 2, 3], name="v"),), lambda s: s.reindex(range(5)))


def test_series_reindex_multiindex():
    def make():
        return (pd.Series([1, 2, 3], index=MI, name="v"),)

    check(make, lambda s: s.reindex([(2, "a"), (3, "z"), (1, "b")]))
    check(make, lambda s: s.reindex([(1, "b")], method="ffill"))
    check(make, lambda s: s.reindex([(1, "c")], method="nearest"))
    check(make, lambda s: s.reindex([(1, "c")], method="ffill", tolerance=1))
    check(
        make,
        lambda s: s.reindex(pd.MultiIndex.from_tuples([(2, "a"), (9, "z")], names=["p", "q"])),
    )
    check(make, lambda s: s.reindex(list(MI)))


def test_series_reindex_multiindex_partial_keys_raise():
    s = Series.from_pandas(pd.Series([1, 2, 3], index=MI, name="v"))
    with pytest.raises(ValueError):
        s.reindex([2, 3])


@pytest.mark.parametrize("method", ["ffill", "pad", "bfill", "backfill", "nearest"])
@pytest.mark.parametrize("target", ["around", "ties", "unsorted", "dups"])
@pytest.mark.parametrize("index", ["sorted", "decreasing"])
def test_series_reindex_methods(method, target, index):
    check(
        lambda: (_s(INDEXES[index]),),
        lambda s: s.reindex(FILL_TARGETS[target], method=method),
    )


@pytest.mark.parametrize("method", ["ffill", "bfill", "nearest"])
@pytest.mark.parametrize("limit", [0, 1, 2])
@pytest.mark.parametrize("target", ["dense", "low", "far", "around", "unsorted"])
@pytest.mark.parametrize("index", ["sorted", "decreasing"])
def test_series_reindex_limit(method, limit, target, index):
    check(
        lambda: (_s(INDEXES[index]),),
        lambda s: s.reindex(FILL_TARGETS[target], method=method, limit=limit),
    )


@pytest.mark.parametrize("method", ["ffill", "bfill", "nearest"])
@pytest.mark.parametrize("tolerance", [0, 4, 5, 6, 10, 100])
@pytest.mark.parametrize("target", ["around", "ties"])
def test_series_reindex_tolerance(method, tolerance, target):
    check(
        lambda: (_s([10, 20, 30]),),
        lambda s: s.reindex(FILL_TARGETS[target], method=method, tolerance=tolerance),
    )
    check(
        lambda: (_s([30, 20, 10]),),
        lambda s: s.reindex(FILL_TARGETS[target], method=method, tolerance=tolerance),
    )


def test_series_reindex_limit_and_tolerance_together():
    check(
        lambda: (_s([10, 20, 30]),),
        lambda s: s.reindex([5, 10, 11, 12, 21, 22, 40], method="ffill", limit=1, tolerance=1),
    )


def test_series_reindex_method_on_text_labels():
    def make():
        return (pd.Series([1, 2, 3], index=list("bdf")),)

    check(make, lambda s: s.reindex(list("acegz"), method="ffill"))
    check(make, lambda s: s.reindex(list("acegz"), method="bfill"))
    check(make, lambda s: s.reindex(list("ace"), method="nearest"))
    check(make, lambda s: s.reindex(list("ace"), method="ffill", tolerance=1))


def test_series_reindex_method_on_float_and_nan_labels():
    check(
        lambda: (pd.Series([1, 2, 3], index=[1.5, 2.5, 3.5]),),
        lambda s: s.reindex([1.0, 2.0, 3.0, 4.0], method="nearest"),
    )
    check(
        lambda: (pd.Series([1, 2, 3], index=[1.5, 2.5, 3.5]),),
        lambda s: s.reindex([1, 2, 3, 4], method="bfill"),
    )


def test_series_reindex_method_errors():
    check(lambda: (_s([10, 30, 20]),), lambda s: s.reindex([5, 10], method="ffill"))
    check(lambda: (_s([10, 30, 20]),), lambda s: s.reindex([10, 20], method="ffill"))
    check(lambda: (_s([10, 30, 20]),), lambda s: s.reindex([10, 30, 20], method="ffill"))
    check(lambda: (_s([10, 20, 30]),), lambda s: s.reindex([5], tolerance=4))
    check(lambda: (_s([10, 20, 30]),), lambda s: s.reindex([5], limit=4))
    check(lambda: (_s([10, 20, 30]),), lambda s: s.reindex([5], method="sideways"))
    check(lambda: (_s([10, 20, 30]),), lambda s: s.reindex([25, 5], method="ffill", limit=1))
    check(lambda: (_s([30, 20, 10]),), lambda s: s.reindex([5, 25], method="ffill", limit=1))
    check(lambda: (_s([10, 20, 30]),), lambda s: s.reindex([5], method="ffill", limit=-1))


def test_series_reindex_timestamps():
    ix = pd.to_datetime(["2020-01-01", "2020-01-03", "2020-01-05"])
    want = pd.to_datetime(["2020-01-02", "2020-01-03", "2020-01-09"])

    def make():
        return (pd.Series([1, 2, 3], index=ix),)

    check(make, lambda s: s.reindex(want))
    check(make, lambda s: s.reindex(want, method="ffill"))
    check(make, lambda s: s.reindex(want, method="nearest", tolerance=pd.Timedelta("1D")))
    check(make, lambda s: s.reindex(want, method="bfill", tolerance=datetime.timedelta(days=1)))


# ---- DataFrame.reindex ------------------------------------------------------------


@pytest.mark.parametrize("kind", INDEXES)
def test_frame_reindex_labels(kind):
    check(lambda: (_f(INDEXES[kind]),), lambda d: d.reindex(TARGETS[kind]))
    check(lambda: (_f(INDEXES[kind]),), lambda d: d.reindex(index=TARGETS[kind]))
    check(lambda: (_f(INDEXES[kind]),), lambda d: d.reindex(TARGETS[kind], axis=0))
    check(lambda: (_f(INDEXES[kind]),), lambda d: d.reindex(TARGETS[kind], axis="index"))


def test_frame_reindex_same_labels_even_when_repeated():
    check(lambda: (_f([1, 1, 2]),), lambda d: d.reindex([1, 1, 2]))
    check(lambda: (_f([1, 1, 2]),), lambda d: d.reindex([1, 2]))


def test_frame_reindex_widens_integers_and_keeps_text():
    out = DataFrame.from_pandas(_f([10, 20, 30])).reindex([20, 40])
    assert out["a"].dtype.name == "FLOAT64"
    assert out["c"].dtype.name == "STRING"


@pytest.mark.parametrize("fill", [0, 1.5, -1])
def test_frame_reindex_fill_value(fill):
    def make():
        return (pd.DataFrame({"a": [1, 2, 3], "b": [1.5, 2.5, 3.5]}, index=[10, 20, 30]),)

    check(make, lambda d: d.reindex([20, 40], fill_value=fill))
    check(make, lambda d: d.reindex([20, 40], columns=["b", "q"], fill_value=fill))


def test_frame_reindex_fill_value_text_column():
    check(
        lambda: (pd.DataFrame({"c": ["x", "y"]}, index=[1, 2]),),
        lambda d: d.reindex([1, 9], fill_value="m"),
    )
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex([20, 40], fill_value=0))


def test_frame_reindex_columns():
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex(columns=["c", "a", "q"]))
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex(columns=["c", "a", "q"], fill_value=0))
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex(["b", "zz"], axis=1))
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex(["b"], axis="columns"))
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex(columns=["b", "a"]))
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex(columns=[]))
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex())


def test_frame_reindex_rows_and_columns():
    check(
        lambda: (pd.DataFrame({"a": [1, 2, 3], "b": [1.5, 2.5, 3.5]}, index=[10, 20, 30]),),
        lambda d: d.reindex([30, 99], columns=["a", "q"], fill_value=-1),
    )
    check(
        lambda: (_f([10, 20, 30]),),
        lambda d: d.reindex(index=[30, 99], columns=["a", "q"]),
    )
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex([20], axis=1, index=[10]))
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex([20], index=[10], columns=["a"]))


def test_frame_reindex_argument_errors():
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex([20], axis=2))
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex(index=[20], axis=0))
    check(lambda: (_f([10, 20, 30]),), lambda d: d.reindex(columns=["a"], axis=1))


@pytest.mark.parametrize("method", ["ffill", "bfill", "nearest"])
def test_frame_reindex_methods(method):
    check(
        lambda: (_f([10, 20, 30]),),
        lambda d: d.reindex([5, 15, 45], method=method),
    )
    check(
        lambda: (_f([10, 20, 30]),),
        lambda d: d.reindex([5, 11, 12, 45], method=method, limit=1, tolerance=20),
    )
    check(lambda: (_f([10, 30, 20]),), lambda d: d.reindex([5], method=method))
    check(lambda: (_f([1, 1, 2]),), lambda d: d.reindex([3], method=method))


def test_frame_reindex_without_an_index():
    check(lambda: (pd.DataFrame({"a": [1, 2, 3]}),), lambda d: d.reindex([2, 5]))
    check(lambda: (pd.DataFrame({"a": [1, 2, 3]}),), lambda d: d.reindex([2, 5], method="ffill"))
    check(
        lambda: (pd.DataFrame({"a": [1, 2, 3]}),),
        lambda d: d.reindex([2, 5], columns=["a", "b"], fill_value=0),
    )


def test_frame_reindex_multiindex():
    def make():
        return (pd.DataFrame({"a": [1, 2, 3]}, index=MI),)

    check(make, lambda d: d.reindex([(2, "a"), (5, "q")]))
    check(make, lambda d: d.reindex(list(MI)))
    check(make, lambda d: d.reindex([(1, "b")], method="ffill"))
    check(make, lambda d: d.reindex([(1, "c")], method="nearest"))
    check(make, lambda d: d.reindex([(1, "c")], method="ffill", tolerance=1))


def test_frame_reindex_keeps_an_index_set_by_set_index():
    ours = DataFrame.from_dict({"k": [10, 20, 30], "a": [1, 2, 3]}).set_index("k")
    out = ours.reindex([30, 99])
    assert out.index_names() == ["k"]
    assert out["a"].to_list() == [3, None]


# ---- reindex_like -----------------------------------------------------------------


def test_series_reindex_like():
    check(lambda: (_s([10, 20, 30]), _s([30, 99])), lambda s, o: s.reindex_like(o))
    check(
        lambda: (_s([10, 20, 30]), _s([25, 99])),
        lambda s, o: s.reindex_like(o, method="ffill"),
    )
    check(
        lambda: (_s([10, 20, 30]), _s([25, 99])),
        lambda s, o: s.reindex_like(o, method="nearest", tolerance=6),
    )
    check(
        lambda: (_s([10, 20, 30]), _s([25, 99])),
        lambda s, o: s.reindex_like(o, method="ffill", limit=1),
    )
    check(lambda: (pd.Series([1, 2, 3]), pd.Series([0, 0, 0, 0])), lambda s, o: s.reindex_like(o))


def test_frame_reindex_like():
    other = pd.DataFrame({"a": [0], "z": [0]}, index=pd.Index([30], name="j"))
    check(lambda: (_f([10, 20, 30]), other), lambda d, o: d.reindex_like(o))
    check(
        lambda: (_f([10, 20, 30]), pd.DataFrame({"a": [0, 0]}, index=[25, 99])),
        lambda d, o: d.reindex_like(o, method="ffill"),
    )
    check(lambda: (_f([10, 20, 30]), _f([10, 20, 30])), lambda d, o: d.reindex_like(o))


def test_series_reindex_like_a_frame():
    check(lambda: (_s([10, 20, 30]), _f([30, 99])), lambda s, o: s.reindex_like(o))


# ---- align ------------------------------------------------------------------------

PAIRS = {
    "unique": ([3, 1, 2], [2, 5, 3]),
    "dup": ([1, 1, 2, 3], [1, 3, 3]),
    "perm": ([1, 2, 3], [3, 2, 1]),
    "subset": ([1, 2, 3], [1, 2]),
    "same": ([3, 1, 2], [3, 1, 2]),
    "dups mono": ([3, 1, 3, 2], [3, 3, 2]),
    "dups unsorted": ([2, 1, 2], [2, 5, 2]),
    "decreasing": ([3, 2, 1], [3, 1]),
    "text": (["b", "a", "c"], ["c", "d", "a"]),
    "nan": ([1.0, 2.0, NAN], [NAN, 2.0, 5.0]),
    "dup same": ([1, 1, 2], [1, 1, 2]),
    "dup other order": ([2, 1, 1], [1, 1, 2]),
}


def _pair(a, b):
    n = len(a)
    m = len(b)
    return (
        pd.Series(list(range(1, n + 1)), index=pd.Index(a, name="k"), name="l"),
        pd.Series(list(range(10, 10 * m + 1, 10)), index=pd.Index(b, name="k"), name="r"),
    )


@pytest.mark.parametrize("join", ["outer", "inner", "left", "right"])
@pytest.mark.parametrize("pair", PAIRS)
def test_series_align(pair, join):
    a, b = PAIRS[pair]
    if PANDAS30 and join == "inner" and pair == "dup":
        pytest.skip("pandas 3.0 orders the other side's repeated labels unlike 2.x and 3.1")
    check(lambda: _pair(a, b), lambda x, y: x.align(y, join=join))


@pytest.mark.parametrize("join", ["outer", "inner", "left", "right"])
def test_series_align_axis_zero(join):
    check(lambda: _pair([3, 1, 2], [2, 5, 3]), lambda x, y: x.align(y, join=join, axis=0))
    check(lambda: _pair([3, 1, 2], [2, 5, 3]), lambda x, y: x.align(y, join=join, axis=1))


@pytest.mark.parametrize("fill", [0, 1.5, 7])
def test_series_align_fill_value(fill):
    check(lambda: _pair([1, 2, 3], [2, 9]), lambda x, y: x.align(y, fill_value=fill))
    check(
        lambda: (
            pd.Series([1.0, NAN, 3.0], index=[1, 2, 3]),
            pd.Series([5, 6], index=[2, 9]),
        ),
        lambda x, y: x.align(y, fill_value=fill),
    )


def test_series_align_positional_and_labelled():
    check(lambda: (pd.Series([1, 2, 3]), pd.Series([4, 5])), lambda x, y: x.align(y))
    check(
        lambda: (pd.Series([1, 2, 3], index=[2, 1, 5]), pd.Series([4, 5])),
        lambda x, y: x.align(y),
    )
    check(
        lambda: (pd.Series([1, 2, 3], index=[2, 1, 5]), pd.Series([4, 5])),
        lambda x, y: x.align(y, join="left"),
    )


@pytest.mark.parametrize("join", ["outer", "inner", "left", "right"])
def test_series_align_multiindex(join):
    def make():
        return (
            pd.Series([1, 2], index=pd.MultiIndex.from_tuples([(1, "a"), (2, "b")])),
            pd.Series([3, 4], index=pd.MultiIndex.from_tuples([(2, "b"), (0, "z")])),
        )

    check(make, lambda x, y: x.align(y, join=join))


def test_series_align_errors():
    check(lambda: _pair([1], [1]), lambda x, y: x.align([1, 2]))
    check(lambda: _pair([1], [2]), lambda x, y: x.align(y, join="sideways"))


@pytest.mark.parametrize("join", ["outer", "inner", "left", "right"])
def test_series_align_a_frame(join):
    check(
        lambda: (_s([3, 1, 2]), _f([1, 9])),
        lambda s, d: s.align(d, join=join),
    )
    check(
        lambda: (_s([3, 1, 2]), _f([1, 9])),
        lambda s, d: s.align(d, join=join, axis=0),
    )


def _frames():
    return (
        pd.DataFrame({"x": [1, 2, 3], "y": [4, 5, 6]}, index=pd.Index([3, 1, 2], name="k")),
        pd.DataFrame({"y": [1, 2], "z": [3, 4]}, index=pd.Index([1, 9], name="k")),
    )


@pytest.mark.parametrize("join", ["outer", "inner", "left", "right"])
@pytest.mark.parametrize("axis", [None, 0, 1, "index", "columns"])
def test_frame_align(join, axis):
    check(_frames, lambda a, b: a.align(b, join=join, axis=axis))


@pytest.mark.parametrize("fill", [0, 1.5])
@pytest.mark.parametrize("axis", [None, 0, 1])
def test_frame_align_fill_value(fill, axis):
    check(_frames, lambda a, b: a.align(b, axis=axis, fill_value=fill))
    check(
        lambda: (
            pd.DataFrame({"x": [1.0, NAN, 3.0]}, index=[1, 2, 3]),
            pd.DataFrame({"x": [5, 6]}, index=[2, 9]),
        ),
        lambda a, b: a.align(b, fill_value=fill),
    )


@pytest.mark.parametrize(
    "kind",
    [
        "unique",
        "dup",
        "dups mono",
        "dups unsorted",
        "same",
        "perm",
        "subset",
        "decreasing",
        "text",
        "nan",
    ],
)
@pytest.mark.parametrize("join", ["outer", "inner", "left", "right"])
def test_frame_align_row_labels(kind, join):
    a, b = PAIRS[kind]

    def make():
        return (
            pd.DataFrame({"x": list(range(len(a)))}, index=pd.Index(a, name="k")),
            pd.DataFrame({"y": list(range(len(b)))}, index=pd.Index(b, name="k")),
        )

    check(make, lambda p, q: p.align(q, join=join))


@pytest.mark.parametrize("join", ["outer", "inner", "left", "right"])
def test_frame_align_equal_columns_in_another_order(join):
    check(
        lambda: (
            pd.DataFrame({"b": [1, 2], "a": [3, 4]}),
            pd.DataFrame({"a": [1, 2], "b": [3, 4]}),
        ),
        lambda p, q: p.align(q, join=join, axis=1),
    )


def test_frame_align_without_an_index():
    check(
        lambda: (pd.DataFrame({"x": [1, 2, 3]}), pd.DataFrame({"y": [4, 5]})),
        lambda p, q: p.align(q),
    )


@pytest.mark.parametrize("join", ["outer", "inner", "left", "right"])
def test_frame_align_a_series(join):
    check(lambda: (_f([3, 1, 2]), _s([1, 9])), lambda d, s: d.align(s, join=join, axis=0))
    check(
        lambda: (_f([3, 1, 2]), pd.Series([2, 1, 5], index=["c", "a", "q"])),
        lambda d, s: d.align(s, join=join, axis=1),
    )


def test_frame_align_a_series_needs_an_axis():
    check(lambda: (_f([3, 1, 2]), _s([1, 9])), lambda d, s: d.align(s))
    check(lambda: (_f([3, 1, 2]), _s([1, 9])), lambda d, s: d.align([1, 2], axis=0))


@pytest.mark.parametrize("join", ["outer", "inner", "left", "right"])
def test_frame_align_multiindex(join):
    def make():
        return (
            pd.DataFrame({"a": [1, 2]}, index=pd.MultiIndex.from_tuples([(1, "a"), (2, "b")])),
            pd.DataFrame({"b": [3, 4]}, index=pd.MultiIndex.from_tuples([(2, "b"), (0, "z")])),
        )

    check(make, lambda p, q: p.align(q, join=join))


def test_align_results_carry_the_joined_labels_natively():
    a = Series.from_pandas(_pair([3, 1, 2], [2, 5, 3])[0])
    b = Series.from_pandas(_pair([3, 1, 2], [2, 5, 3])[1])
    left, right = a.align(b)
    assert left.index.to_list() == [1, 2, 3, 5]
    assert right.index.to_list() == [1, 2, 3, 5]
    assert left.to_list() == [2.0, 3.0, 1.0, None]
    assert right.to_list() == [None, 10.0, 30.0, 20.0]
    total = left + right
    assert total.to_list() == [None, 13.0, 31.0, None]


# ---- exact distances, mixed time units, string tolerance --------------------------

T0 = pd.Timestamp("2020-01-01")


def _ns(*offsets):
    return pd.DatetimeIndex([T0 + pd.Timedelta(o, "ns") for o in offsets]).as_unit("ns")


def test_nanosecond_tolerance_is_exact():
    ix = _ns(0, 1000, 2000)
    tg = _ns(130, 1100, 1501)
    for method in ("nearest", "ffill", "bfill"):
        for tol in (10, 110, 499, 500, 501):
            check(
                lambda: (pd.Series([1, 2, 3], index=ix),),
                lambda s, method=method, tol=tol: s.reindex(
                    tg, method=method, tolerance=pd.Timedelta(tol, "ns")
                ),
            )


def test_nanosecond_nearest_near_the_midpoint():
    ix = _ns(0, 1000, 2000)
    check(
        lambda: (pd.Series([1, 2, 3], index=ix),),
        lambda s: s.reindex(_ns(499, 500, 501, 1499, 1500, 1501), method="nearest"),
    )


@pytest.mark.parametrize("tol", [0, 1, 2, 3])
def test_big_integer_labels_with_a_tolerance(tol):
    big = 2**60
    check(
        lambda: (pd.Series([1, 2], index=[big, big + 3]),),
        lambda s, tol=tol: s.reindex([big + 1, big + 2, big + 4], method="nearest", tolerance=tol),
    )
    check(
        lambda: (pd.Series([1, 2], index=[big, big + 3]),),
        lambda s: s.reindex([big + 1, big + 2, big + 4], method="nearest"),
    )
    check(
        lambda: (pd.Series([1, 2], index=[big, big + 3]),),
        lambda s, tol=tol: s.reindex([big + 1, big + 2, big + 4], method="ffill", tolerance=tol),
    )


UNITS = ["s", "ms", "us", "ns"]


def _stamps(unit, *offsets_ns):
    return pd.DatetimeIndex([T0 + pd.Timedelta(o, "ns") for o in offsets_ns]).as_unit(unit)


@pytest.mark.parametrize("index_unit", UNITS)
@pytest.mark.parametrize("key_unit", UNITS)
def test_time_units_of_labels_and_keys_differ(index_unit, key_unit):
    sec = 10**9
    labels = _stamps(index_unit, 0, 10 * sec, 20 * sec)
    exact = _stamps(key_unit, 10 * sec, 0, 5 * sec, 21 * sec)
    fine = _stamps("ns", 10 * sec + 1, 10 * sec, 4 * sec + 500)
    for keys in (exact, fine):
        make = lambda: (pd.Series([1, 2, 3], index=labels),)  # noqa: E731
        check(make, lambda s, keys=keys: s.reindex(keys))
        for method in ("ffill", "bfill", "nearest"):
            check(make, lambda s, keys=keys, method=method: s.reindex(keys, method=method))
            check(
                make,
                lambda s, keys=keys, method=method: s.reindex(
                    keys, method=method, tolerance=pd.Timedelta(5, "s")
                ),
            )


@pytest.mark.parametrize("index_unit", UNITS)
@pytest.mark.parametrize("key_unit", UNITS)
def test_loc_with_keys_of_another_time_unit(index_unit, key_unit):
    sec = 10**9
    labels = _stamps(index_unit, 0, 10 * sec, 20 * sec)
    for keys in (_stamps(key_unit, 10 * sec, 0), _stamps("ns", 10 * sec + 1, 10 * sec)):
        make = lambda: (pd.Series([1, 2, 3], index=labels),)  # noqa: E731
        check(make, lambda s, keys=keys: s.loc[keys[0]])
        check(make, lambda s, keys=keys: s.loc[list(keys)])


def test_string_tolerance():
    ix = pd.to_datetime(["2020-01-01 00:00:00", "2020-01-01 00:00:10", "2020-01-01 00:00:20"])
    tg = pd.to_datetime(["2020-01-01 00:00:04", "2020-01-01 00:00:06", "2020-01-01 00:00:30"])
    for tol in ("5s", "4s", "10s", "1min", "500ms"):
        check(
            lambda: (pd.Series([1, 2, 3], index=ix),),
            lambda s, tol=tol: s.reindex(tg, method="nearest", tolerance=tol),
        )


def test_a_text_target_against_number_labels_with_a_method_is_a_type_error():
    for method in ("ffill", "bfill", "nearest"):
        check(
            lambda: (pd.Series([1, 2], index=[10, 20]),),
            lambda s, method=method: s.reindex(["a"], method=method),
        )


@pytest.mark.parametrize("descending", [False, True])
@pytest.mark.parametrize("method", ["ffill", "bfill", "nearest"])
def test_sorted_targets_walk_matches_pandas(method, descending):
    rng = __import__("numpy").random.default_rng(7)
    labels = sorted(set(rng.integers(0, 400, 120).tolist()), reverse=descending)
    targets = sorted(rng.integers(-20, 420, 300).tolist(), reverse=descending)
    unsorted_targets = list(rng.permutation(targets))
    for tg in (targets, unsorted_targets):
        for kw in ({}, {"tolerance": 3}, {"limit": 2}):
            if method == "nearest" and "limit" in kw:
                continue
            check(
                lambda: (pd.Series(range(len(labels)), index=labels),),
                lambda s, tg=tg, kw=kw: s.reindex(tg, method=method, **kw),
            )


def test_fill_value_with_missing_rows_matches_pandas():
    for fv in (0, -1, 2.5):
        check(
            lambda: (pd.Series([1, 2, 3], index=[10, 20, 30]),),
            lambda s, fv=fv: s.reindex([20, 25, 10, 40], fill_value=fv),
        )
        check(
            lambda: (pd.Series([1.5, None, 3.0], index=[10, 20, 30]),),
            lambda s, fv=fv: s.reindex([20, 25, 10, 40], fill_value=fv),
        )


@pytest.mark.parametrize("method", ["ffill", "bfill"])
def test_an_empty_target_with_a_limit_on_a_decreasing_index_is_empty(method):
    check(
        lambda: (pd.Series([1.0, 2.0, 3.0], index=[30, 20, 10]),),
        lambda s: s.reindex([], method=method, limit=2),
    )


# ---- large inputs: the lookups run in parallel chunks ----------------------------


def _big_labels(count, seed, step=3):
    import numpy as np

    rng = np.random.default_rng(seed)
    return (rng.permutation(count) * step).tolist()


@pytest.mark.parametrize("method", [None, "ffill", "bfill"])
@pytest.mark.parametrize("order", ["ascending", "descending"])
@pytest.mark.parametrize("keys", ["sorted", "unsorted"])
def test_series_reindex_of_many_keys_matches_pandas(method, order, keys):
    import numpy as np

    labels = sorted(_big_labels(40_000, 1))
    if order == "descending":
        labels = labels[::-1]
    rng = np.random.default_rng(2)
    target = rng.integers(-20, 3 * 40_000 + 20, 50_000)
    if keys == "sorted":
        target = np.sort(target) if order == "ascending" else np.sort(target)[::-1]
    check(
        lambda: (_s(labels),),
        lambda s: s.reindex(target.tolist() if isinstance(s, pd.Series) else target, method=method),
    )


@pytest.mark.parametrize("keys", ["list", "numpy"])
def test_series_loc_of_many_labels_matches_pandas(keys):
    import numpy as np

    labels = _big_labels(40_000, 3)
    rng = np.random.default_rng(4)
    picked = rng.choice(np.array(labels), 30_000, replace=False)
    key = picked.tolist() if keys == "list" else picked
    check(lambda: (_s(labels),), lambda s: s.loc[key])
    with pytest.raises(KeyError):
        Series.from_pandas(_s(labels)).loc[np.array([1, 2, 5]) if keys == "numpy" else [1, 2, 5]]


@pytest.mark.parametrize("join", ["left", "right", "inner", "outer"])
def test_series_align_of_sorted_unique_integer_labels_matches_pandas(join):
    import numpy as np

    rng = np.random.default_rng(5)
    left = np.unique(rng.integers(0, 90_000, 40_000)).tolist()
    right = np.unique(rng.integers(30_000, 120_000, 40_000)).tolist()
    check(lambda: (_s(left), _s(right, "w")), lambda a, b: a.align(b, join=join))


@pytest.mark.parametrize("join", ["left", "inner", "outer"])
def test_series_align_with_repeated_or_unsorted_labels_matches_pandas(join):
    check(lambda: (_s([5, 1, 3, 9]), _s([3, 4, 5], "w")), lambda a, b: a.align(b, join=join))
    check(lambda: (_s([1, 2, 2, 9]), _s([2, 4, 9], "w")), lambda a, b: a.align(b, join=join))


@pytest.mark.parametrize("join", ["left", "right", "inner", "outer"])
def test_frame_align_keeps_the_kept_side_label_type(join):
    check(
        lambda: (
            pd.DataFrame({"x": [1, 2, 3]}, index=pd.Index([1, 2, 3], name="k")),
            pd.DataFrame({"y": [4, 5, 6]}, index=pd.Index([1.0, 2.0, 4.0], name="k")),
        ),
        lambda p, q: p.align(q, join=join),
    )


@pytest.mark.parametrize("join", ["left", "right", "inner", "outer"])
def test_align_keeps_large_int_labels_exact(join):
    big = 2**53 + 1
    check(lambda: (_s([big, 5]), _s([float(2**53), 7.5], "w")), lambda a, b: a.align(b, join=join))
    check(
        lambda: (_s([big, 5]).to_frame(), _s([float(2**53), 7.5], "w").to_frame()),
        lambda a, b: a.align(b, join=join),
    )


@pytest.mark.parametrize("join", ["left", "right", "inner", "outer"])
def test_series_align_on_equal_labels_of_other_types_keeps_each_type(join):
    check(lambda: (_s([1, 2, 3]), _s([1.0, 2.0, 3.0], "w")), lambda a, b: a.align(b, join=join))


def _two_levels(numbers, values):
    return pd.Series(
        values,
        index=pd.MultiIndex.from_arrays([numbers, list("xyz")[: len(numbers)]], names=["k", "s"]),
    )


@pytest.mark.parametrize("join", ["left", "right", "inner", "outer"])
@pytest.mark.parametrize("flip", [False, True])
def test_align_of_several_levels_keeps_the_pandas_level_type(join, flip):
    def make():
        a = _two_levels([1, 2, 3], [1.0, 2.0, 3.0])
        b = _two_levels([1.0, 2.0, 4.0], [4.0, 5.0, 6.0])
        return (b, a) if flip else (a, b)

    check(make, lambda a, b: a.align(b, join=join))
    check(lambda: tuple(s.to_frame("v") for s in make()), lambda a, b: a.align(b, join=join))
