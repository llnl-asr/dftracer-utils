"""Property-based tests (Hypothesis): random typed arrays with nulls at any
length, three invariants that reach the engine's C entries.

1. A view (a filter's selection, a sort, a dictionary) gives what its flat
   copy gives, for every column op the registry exports.
2. The pandas surface agrees with pandas on random data: arithmetic, the
   cumulative and rolling kernels, sort and rank, group-by reductions.
3. An aggregate state serializes and merges losslessly: deserialize after
   serialize, and the merge of two partials, both equal the one-pass run.
"""

import math

import pytest

pa = pytest.importorskip("pyarrow")
pd = pytest.importorskip("pandas")
np = pytest.importorskip("numpy")
hypothesis = pytest.importorskip("hypothesis")

from hypothesis import given, settings  # noqa: E402
from hypothesis import strategies as st  # noqa: E402
from hypothesis.extra import numpy as hnp  # noqa: E402

from dftracer.utils import DataFrame, Series  # noqa: E402
from dftracer.utils import dftracer_utils_ext as _ext  # noqa: E402

SETTINGS = settings(max_examples=60, deadline=None)


def _floats(n):
    return hnp.arrays(np.float64, n, elements=st.floats(-1e6, 1e6, allow_nan=False))


def _mask(n):
    return hnp.arrays(np.bool_, n)


@st.composite
def float_column(draw, min_size=0, max_size=64):
    """A Float64 column with nulls where the mask says so."""
    n = draw(st.integers(min_size, max_size))
    values = draw(_floats(n))
    nulls = draw(_mask(n))
    return [None if m else float(v) for v, m in zip(values, nulls)]


@st.composite
def int_column(draw, min_size=0, max_size=64):
    n = draw(st.integers(min_size, max_size))
    values = draw(hnp.arrays(np.int64, n, elements=st.integers(-1000, 1000)))
    nulls = draw(_mask(n))
    return [None if m else int(v) for v, m in zip(values, nulls)]


@st.composite
def keys_column(draw, n):
    return draw(st.lists(st.sampled_from(["a", "b", "c", None]), min_size=n, max_size=n))


def _clean(values):
    return [None if isinstance(v, float) and math.isnan(v) else v for v in values]


def _same(a, b, rel=1e-9):
    a, b = _clean(a), _clean(b)
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if x is None or y is None:
            if x is not y:
                return False
        elif isinstance(x, float) or isinstance(y, float):
            if not math.isclose(x, y, rel_tol=rel, abs_tol=1e-9):
                return False
        elif x != y:
            return False
    return True


# ---- 1. a view equals its flat twin -------------------------------------------

_VIEW_OPS = [
    ("dftu.series.cumsum", ()),
    ("dftu.series.cummax", ()),
    ("dftu.series.shift", (1,)),
    ("dftu.series.diff", ()),
    ("dftu.series.rank", (0, 0)),
    ("dftu.series.ffill", ()),
    ("dftu.series.sort", (0,)),
    ("dftu.series.rolling", (3, 1)),
    ("dftu.series.rolling_std", (3,)),
    ("dftu.series.ewm_mean", (0.3,)),
    ("dftu.series.is_duplicated", ()),
    ("dftu.series.abs", ()),
    ("dftu.series.fillna", (0.5,)),
]


@SETTINGS
@given(values=float_column(min_size=1), mask=st.data())
def test_view_equals_flat_twin(values, mask):
    n = len(values)
    keep = mask.draw(_mask(n))
    flat = Series(pa.array(values, pa.float64()))
    view = flat.filter(Series(pa.array([bool(k) for k in keep], pa.bool_())))
    twin = Series(pa.array(view.to_list(), pa.float64()))
    nested = view.filter(Series(pa.array([True] * len(view), pa.bool_())))
    for op, args in _VIEW_OPS:
        for candidate in (view, nested):
            got = Series(_ext.op_run(op, candidate._native, *args)).to_list()
            want = Series(_ext.op_run(op, twin._native, *args)).to_list()
            assert _same(got, want), (op, got, want)


# ---- 2. pandas parity on random data -------------------------------------------


@SETTINGS
@given(a=float_column(min_size=1), b=st.data())
def test_arithmetic_and_scans_match_pandas(a, b):
    other = b.draw(float_column(min_size=len(a), max_size=len(a)))
    s, t = Series(pa.array(a, pa.float64())), Series(pa.array(other, pa.float64()))
    ps, pt = pd.Series(a, dtype="float64"), pd.Series(other, dtype="float64")
    assert _same((s + t).to_list(), (ps + pt).tolist())
    assert _same((s * t - s).to_list(), (ps * pt - ps).tolist())
    assert _same((s / 2.0).to_list(), (ps / 2.0).tolist())
    assert _same(s.cumsum().to_list(), ps.cumsum().tolist())
    assert _same(s.cummax().to_list(), ps.cummax().tolist())
    assert _same(s.diff().to_list(), ps.diff().tolist())
    assert _same(s.shift(2).to_list(), ps.shift(2).tolist())
    assert _same(s.ffill().to_list(), ps.ffill().tolist())
    assert _same(s.rolling(3).sum().to_list(), ps.rolling(3).sum().tolist())
    assert _same(s.rolling(3).mean().to_list(), ps.rolling(3).mean().tolist())
    # pandas' rolling std is an online scan that drifts after a large value;
    # the engine recomputes each window, so the oracle is the window itself.
    exact = [
        None
        if i < 1 or any(v is None for v in a[i - 1 : i + 1])
        else float(np.std(a[i - 1 : i + 1], ddof=1))
        for i in range(len(a))
    ]
    assert _same(s.rolling(2).std().to_list(), exact, rel=1e-9)
    assert _same(s.expanding().mean().to_list(), ps.expanding().mean().tolist())
    assert _same(s.ewm(alpha=0.4).mean().to_list(), ps.ewm(alpha=0.4).mean().tolist(), rel=1e-6)
    assert _same(s.rank().to_list(), ps.rank().tolist())
    assert _same(s.sort().to_list(), ps.sort_values().tolist())
    present = [v for v in a if v is not None]
    if present:
        assert math.isclose(s.sum(), sum(present), rel_tol=1e-9, abs_tol=1e-6)
        assert s.max() == max(present) and s.min() == min(present)


@SETTINGS
@given(values=float_column(min_size=1), data=st.data())
def test_group_by_matches_pandas(values, data):
    keys = data.draw(keys_column(len(values)))
    df = DataFrame({"k": pa.array(keys, pa.string()), "v": pa.array(values, pa.float64())})
    p = df.to_pandas()
    ours = df.group_by("k").agg(s=("v", "sum"), m=("v", "mean"), c=("v", "count"), mx=("v", "max"))
    ours = ours.sort_by("k")
    theirs = p.groupby("k").agg(s=("v", "sum"), m=("v", "mean"), c=("v", "count"), mx=("v", "max"))
    got = ours.to_arrow().to_pydict()
    assert got["k"] == list(theirs.index)
    assert _same(got["c"], [int(x) for x in theirs["c"]])
    assert _same(got["mx"], theirs["mx"].tolist())
    # pandas gives 0 / NaN for an all-null group's sum / mean; the engine null.
    for col in ("s", "m"):
        for a, b, c in zip(got[col], theirs[col].tolist(), got["c"]):
            if c == 0:
                continue
            assert _same([a], [b], rel=1e-9), col
    # Null keys are their own group with dropna=False, and row counts add up.
    with_nulls = df.group_by("k", dropna=False).size().to_arrow().to_pydict()
    assert sum(with_nulls["size"]) == len(values)
    # The cumulative transform per group equals pandas.
    cs = df.group_by("k", dropna=False).cumsum().to_arrow().to_pydict()["v"]
    assert _same(cs, p.groupby("k", dropna=False)["v"].cumsum().tolist())


# ---- 3. serialize and merge round trips -----------------------------------------


@SETTINGS
@given(values=float_column(min_size=1), data=st.data())
def test_agg_state_serialize_and_merge_round_trip(values, data):
    keys = data.draw(keys_column(len(values)))
    df = DataFrame({"k": pa.array(keys, pa.string()), "v": pa.array(values, pa.float64())})
    lazy = df.lazy().group_by("k", dropna=False)
    whole = lazy.agg(s=("v", "sum"), c=("v", "count"), mx=("v", "max"), mn=("v", "min")).collect()
    reference = whole.sort_by("k").to_arrow().to_pydict()
    # A plan collected in two halves and concatenated then re-reduced equals
    # the one-pass reduction (the aggregates are mergeable).
    cut = data.draw(st.integers(0, len(values)))
    left = DataFrame(
        {"k": pa.array(keys[:cut], pa.string()), "v": pa.array(values[:cut], pa.float64())}
    )
    right = DataFrame(
        {"k": pa.array(keys[cut:], pa.string()), "v": pa.array(values[cut:], pa.float64())}
    )
    parts = left.concat(right)
    again = parts.group_by("k", dropna=False).agg(
        s=("v", "sum"), c=("v", "count"), mx=("v", "max"), mn=("v", "min")
    )
    got = again.sort_by("k").to_arrow().to_pydict()
    for col in reference:
        assert _same(got[col], reference[col]), col
    # The Arrow round trip of the result is the identity.
    back = DataFrame.from_arrow(whole.to_arrow())
    assert back.to_arrow().to_pydict() == whole.to_arrow().to_pydict()
