# SPDX-License-Identifier: MIT
"""A float NaN key of the pandas ``groupby`` is missing like a null: left out
with ``dropna`` (the default), one group with the null keys without it. The
Polars ``group_by`` keeps NaN as its own group. Every form runs on real pandas
and on ours over the same frame."""

import math

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame, col  # noqa: E402

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

NAN = float("nan")
RAW = {
    "k": [1.0, NAN, None, 2.0, NAN, 1.0, 2.0],
    "j": ["a", "a", "b", "b", None, "a", "b"],
    "w": [1.5, NAN, 2.5, 1.5, 3.5, 1.5, NAN],
    "v": [1, 2, 3, 4, 5, 6, 7],
}
KEYS = {"k": "k", "k+j": ["k", "j"], "j+k": ["j", "k"]}


def _norm(x):
    if isinstance(x, tuple):
        return tuple(_norm(i) for i in x)
    return None if x is None or x is pd.NA or (isinstance(x, float) and math.isnan(x)) else x


def _groups(result):
    """{key: value} of a Series or one-column frame result, keys normalized."""
    if isinstance(result, pd.DataFrame):
        result = result.iloc[:, 0]
    return {_norm(k): _norm(v) for k, v in result.items()}


def _ours(result):
    return _groups(result.to_pandas())


@pytest.mark.parametrize("dropna", [True, False])
@pytest.mark.parametrize("keys", KEYS)
@pytest.mark.parametrize("how", ["sum", "count", "mean", "min", "max", "first", "nunique"])
def test_aggregates_match_pandas(how, keys, dropna):
    ref = pd.DataFrame(RAW)
    ours = DataFrame(RAW)
    by = KEYS[keys]
    got = getattr(ours.groupby(by, dropna=dropna)["v"], how)()
    want = getattr(ref.groupby(by, dropna=dropna)["v"], how)()
    assert _ours(got) == _groups(want)


@pytest.mark.parametrize("dropna", [True, False])
@pytest.mark.parametrize("keys", KEYS)
def test_size_matches_pandas(keys, dropna):
    ref = pd.DataFrame(RAW)
    by = KEYS[keys]
    got = DataFrame(RAW).groupby(by, dropna=dropna).size().to_pandas()
    want = ref.groupby(by, dropna=dropna).size()
    assert _groups(got) == _groups(want)


@pytest.mark.parametrize("dropna", [True, False])
def test_as_index_false_and_the_group_count_match(dropna):
    ref = pd.DataFrame(RAW)
    got = DataFrame(RAW).groupby("k", dropna=dropna, as_index=False)["v"].sum()
    want = ref.groupby("k", dropna=dropna, as_index=False)["v"].sum()
    assert len(got) == len(want)
    assert sorted(map(str, (_norm(x) for x in got["v"].to_list()))) == sorted(
        map(str, (_norm(x) for x in want["v"].tolist()))
    )


@pytest.mark.parametrize("keys", ["k", "k+j"])
@pytest.mark.parametrize("value", ["j", "w"])
def test_per_group_value_counts_match_pandas(keys, value):
    ref = pd.DataFrame(RAW)
    by = KEYS[keys]
    if value in (by if isinstance(by, list) else [by]):
        pytest.skip("the counted column is a key")
    got = DataFrame(RAW).groupby(by)[value].value_counts().to_pandas()
    want = ref.groupby(by)[value].value_counts()
    assert {k: v for k, v in _groups(got).items()} == {k: v for k, v in _groups(want).items()}


def test_a_series_key_is_cleaned_like_a_column():
    ref = pd.DataFrame(RAW)
    ours = DataFrame(RAW)
    for dropna in (True, False):
        got = ours.groupby(ours["k"] * 2, dropna=dropna)["v"].sum().to_pandas()
        want = ref.groupby(ref["k"] * 2, dropna=dropna)["v"].sum()
        assert _groups(got) == _groups(want)


def test_the_polars_group_by_keeps_nan_as_its_own_group():
    ours = DataFrame({"k": [1.0, NAN, 2.0, NAN, 1.0], "v": [1, 2, 3, 4, 5]})
    out = ours.group_by("k").agg(col("v").count().alias("n")).to_dict()
    got = {("nan" if k != k else k): n for k, n in zip(out["k"], out["n"])}
    assert got == {1.0: 2, "nan": 2, 2.0: 1}


KEEP = {
    "nan": {"a": [0.0, NAN, 1.0, NAN, 0.0], "s": ["x", "x", None, "y", "x"], "b": [1, 2, 3, 4, 5]},
    "null": {
        "a": [0.0, None, 1.0, None, 0.0],
        "s": ["x", "x", None, "y", "x"],
        "b": [1, 2, 3, 4, 5],
    },
}
OPS = {
    "transform": lambda g: g["b"].transform("sum"),
    "cumcount": lambda g: g.cumcount(),
    "ngroup": lambda g: g.ngroup(),
    "rank": lambda g: g["b"].rank(),
    "shift": lambda g: g["b"].shift(1),
    "cumsum": lambda g: g["b"].cumsum(),
    "diff": lambda g: g["b"].diff(),
    "cummax": lambda g: g["b"].cummax(),
}


@pytest.mark.parametrize("dropna", [True, False])
@pytest.mark.parametrize("by", ["a", ["a", "s"]], ids=str)
@pytest.mark.parametrize("kind", KEEP)
@pytest.mark.parametrize("op", OPS)
def test_group_wise_transforms_keep_every_row(op, kind, by, dropna):
    """A row whose key was dropped is missing in the result, which keeps the
    length of the frame."""
    ref = pd.DataFrame(KEEP[kind])
    want = OPS[op](ref.groupby(by, dropna=dropna))
    got = OPS[op](DataFrame(KEEP[kind]).groupby(by, dropna=dropna))
    got = got.to_pandas()
    column = got.iloc[:, 0] if isinstance(got, pd.DataFrame) else got
    assert len(column) == len(want) == 5
    assert [_norm(x) for x in column.tolist()] == [_norm(x) for x in want.tolist()]


FULL = {"nan": KEEP["nan"], "null": KEEP["null"]}
WINDOWS = {
    "expanding_sum": lambda g: g["v"].expanding().sum(),
    "expanding_max": lambda g: g["v"].expanding().max(),
    "rolling_sum": lambda g: g["v"].rolling(2).sum(),
    "rolling_max": lambda g: g["v"].rolling(2).max(),
}


@pytest.mark.parametrize("kind", ["nan", "null"])
@pytest.mark.parametrize("window", WINDOWS)
def test_windows_leave_dropped_key_rows_out(window, kind):
    """pandas rolling and expanding keep only the rows with a key; a dropped
    key must not crash."""
    data = {"k": KEEP[kind]["a"], "v": KEEP[kind]["b"]}
    ref = WINDOWS[window](pd.DataFrame(data).groupby("k"))
    got = WINDOWS[window](DataFrame(data).groupby("k"))
    got = got.to_pandas()
    column = got.iloc[:, 0] if isinstance(got, pd.DataFrame) else got
    assert got.index.tolist() == ref.index.tolist()
    assert [_norm(x) for x in column.tolist()] == [_norm(x) for x in ref.tolist()]


@pytest.mark.parametrize("kind", ["nan", "null"])
@pytest.mark.parametrize(
    "op", ["transform", "cumcount", "ngroup", "cumsum", "shift", "diff", "cummax"]
)
def test_spread_integer_results_are_float64(op, kind):
    ref = OPS[op](pd.DataFrame(KEEP[kind]).groupby("a"))
    got = OPS[op](DataFrame(KEEP[kind]).groupby("a")).to_pandas()
    column = got.iloc[:, 0] if isinstance(got, pd.DataFrame) else got
    assert str(column.dtype) == str(ref.dtype) == "float64"


@pytest.mark.parametrize("kind", ["nan", "null"])
@pytest.mark.parametrize(
    "op", ["transform", "cumcount", "ngroup", "cumsum", "shift", "rank", "diff"]
)
@pytest.mark.parametrize("dropna", [True, False])
def test_every_key_missing(op, kind, dropna):
    raw = {"k": [NAN if kind == "nan" else None] * 3, "v": [1, 2, 3]}
    ref = OPS_V[op](pd.DataFrame(raw).groupby("k", dropna=dropna))
    got = OPS_V[op](DataFrame(raw).groupby("k", dropna=dropna)).to_pandas()
    column = got.iloc[:, 0] if isinstance(got, pd.DataFrame) else got
    assert len(column) == len(ref) == 3
    assert [_norm(x) for x in column.tolist()] == [_norm(x) for x in ref.tolist()]
    if dropna:
        assert str(column.dtype) == str(ref.dtype)


OPS_V = {
    "transform": lambda g: g["v"].transform("sum"),
    "cumcount": lambda g: g.cumcount(),
    "ngroup": lambda g: g.ngroup(),
    "cumsum": lambda g: g["v"].cumsum(),
    "shift": lambda g: g["v"].shift(1),
    "rank": lambda g: g["v"].rank(),
    "diff": lambda g: g["v"].diff(),
}


@pytest.mark.parametrize("kind", ["nan", "null"])
def test_every_key_missing_size_and_sum(kind):
    raw = {"k": [NAN if kind == "nan" else None] * 3, "v": [1, 2, 3]}
    for f in (lambda g: g.size(), lambda g: g["v"].sum()):
        ref = f(pd.DataFrame(raw).groupby("k"))
        got = f(DataFrame(raw).groupby("k"))
        got = got.to_pandas() if hasattr(got, "to_pandas") else got
        assert len(got) == len(ref) == 0


def test_a_take_past_the_end_is_an_index_error():
    with pytest.raises(IndexError):
        DataFrame({"a": [1, 2, 3]}).take([0, 7])
