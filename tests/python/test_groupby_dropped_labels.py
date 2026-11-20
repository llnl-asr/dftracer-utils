# SPDX-License-Identifier: MIT
"""The pandas ``groupby`` over a frame with null or NaN keys: the rows that
``apply``, ``filter`` and ``get_group`` return keep the labels they had, and
every group transform and window has the dtype pandas gives it."""

import pytest

pd = pytest.importorskip("pandas")

from dftracer.utils import DataFrame  # noqa: E402

pytestmark = pytest.mark.filterwarnings(
    "ignore::FutureWarning", "ignore::DeprecationWarning", "ignore::UserWarning"
)

NAN = float("nan")
RAW = {
    "k": [1.0, NAN, None, 2.0, NAN, 1.0, 2.0, 3.0, 1.0],
    "j": ["a", "a", "b", "b", None, "a", "b", "c", "d"],
    "id": [10, 11, 12, 13, 14, 15, 16, 17, 18],
    "v": [1, 2, 3, 4, 5, 6, 7, 8, 9],
}
KEYS = {"k": "k", "k+j": ["k", "j"]}
GROUP = {"k": 1.0, "k+j": (1.0, "a")}


def _frames(indexed):
    ref, ours = pd.DataFrame(RAW), DataFrame(RAW)
    if indexed:
        return ref.set_index("id"), ours.set_index("id")
    return ref, ours


OLD_PANDAS = int(pd.__version__.split(".")[0]) < 3


def _labels(frame):
    frame = frame.to_pandas() if hasattr(frame, "to_pandas") else frame
    # pandas 2 shows a missing text label as <NA>, pandas 3 as nan.
    return [str(x).replace("<NA>", "nan") for x in frame.index.tolist()]


def _values(frame):
    frame = frame.to_pandas() if hasattr(frame, "to_pandas") else frame
    if isinstance(frame, pd.Series):
        frame = frame.to_frame()
    return [[str(x) for x in row] for row in frame.reset_index(drop=True).values.tolist()]


@pytest.mark.parametrize("indexed", [False, True], ids=["plain", "indexed"])
@pytest.mark.parametrize("dropna", [True, False])
@pytest.mark.parametrize("by", KEYS)
@pytest.mark.parametrize(
    "op",
    [
        "apply_series",
        "apply_frame",
        "filter_traced",
        "filter_python",
        "get_group",
    ],
)
def test_rows_keep_their_labels(op, by, dropna, indexed):
    if OLD_PANDAS and op.startswith("filter") and by == "k" and not dropna:
        pytest.skip("pandas 2 leaves the NaN-key group out of a filter, as a bug")
    ref, ours = _frames(indexed)
    calls = {
        "apply_series": lambda g: g.apply(lambda x: x["v"] * 2),
        "apply_frame": lambda g: g.apply(lambda x: x[["v"]]),
        "filter_traced": lambda g: g.filter(lambda x: x["v"].sum() > 8),
        "filter_python": lambda g: g.filter(lambda x: len(x) > 1 and x["v"].sum() > 8),
        "get_group": lambda g: g.get_group(GROUP[by]),
    }
    want = calls[op](ref.groupby(KEYS[by], dropna=dropna))
    got = calls[op](ours.groupby(KEYS[by], dropna=dropna))
    assert _labels(got) == _labels(want)
    assert _values(got) == _values(want)


@pytest.mark.skipif(OLD_PANDAS, reason="pandas 2 has no group for a NaN key to get")
def test_get_group_of_the_missing_group():
    ref = pd.DataFrame(RAW).groupby("k", dropna=False).get_group(NAN)
    got = DataFrame(RAW).groupby("k", dropna=False).get_group(NAN)
    assert _labels(got) == _labels(ref)
    assert _values(got) == _values(ref)


DT = {"k": [1, 1, 2, 2, 2], "v": [1, 2, 3, 4, 5], "f": [1.5, 2.5, 3.5, 4.5, 5.5]}
DT_NULL = {"k": [1.0, None, 2.0, 2.0, NAN], "v": [1, 2, 3, 4, 5], "f": [1.5, 2.5, 3.5, 4.5, 5.5]}
OPS = {
    "shift": lambda g: g["v"].shift(),
    "shift_fill": lambda g: g["v"].shift(fill_value=0),
    "shift_back": lambda g: g["v"].shift(-1),
    "shift_float": lambda g: g["f"].shift(),
    "diff": lambda g: g["v"].diff(),
    "pct_change": lambda g: g["v"].pct_change(),
    "cumsum": lambda g: g["v"].cumsum(),
    "cummax": lambda g: g["v"].cummax(),
    "cummin": lambda g: g["v"].cummin(),
    "cumcount": lambda g: g.cumcount(),
    "ngroup": lambda g: g.ngroup(),
    "rank": lambda g: g["v"].rank(),
    "transform_sum": lambda g: g["v"].transform("sum"),
    "transform_mean": lambda g: g["v"].transform("mean"),
    "transform_count": lambda g: g["v"].transform("count"),
    "ffill": lambda g: g["v"].ffill(),
    "head": lambda g: g["v"].head(1),
    "rolling_sum": lambda g: g["v"].rolling(2).sum(),
    "rolling_max": lambda g: g["v"].rolling(2).max(),
    "rolling_mean": lambda g: g["v"].rolling(2).mean(),
    "expanding_sum": lambda g: g["v"].expanding().sum(),
    "expanding_count": lambda g: g["v"].expanding().count(),
    "expanding_max": lambda g: g["v"].expanding().max(),
    "expanding_mean": lambda g: g["v"].expanding().mean(),
}


@pytest.mark.parametrize("raw", [DT, DT_NULL], ids=["clean", "missing_keys"])
@pytest.mark.parametrize("dropna", [True, False])
@pytest.mark.parametrize("op", OPS)
def test_transform_dtypes(op, dropna, raw):
    want = OPS[op](pd.DataFrame(raw).groupby("k", dropna=dropna))
    got = OPS[op](DataFrame(raw).groupby("k", dropna=dropna)).to_pandas()
    if isinstance(got, pd.DataFrame):
        column = got["v"] if "v" in got.columns else got.iloc[:, 0]
    else:
        column = got
    assert str(column.dtype) == str(want.dtype)
    if op.startswith(("rolling", "expanding")):
        return
    assert column.tolist() == pytest.approx(want.tolist(), nan_ok=True)


PICK_RAW = {"k": [1.0, None, 1.0, 2.0, 2.0, NAN, 3.0], "v": [1, 2, 3, 4, 5, 6, 7]}


@pytest.mark.parametrize("indexed", [False, True], ids=["plain", "indexed"])
@pytest.mark.parametrize("raw", [{"k": [1, 2, 1, 2, 1, 3], "v": [1, 2, 3, 4, 5, 6]}, PICK_RAW])
@pytest.mark.parametrize("dropna", [True, False])
@pytest.mark.parametrize("op", ["head", "tail", "nth"])
def test_head_tail_nth_keep_the_labels(op, dropna, raw, indexed):
    raw = {**raw, "id": [v * 10 for v in raw["v"]]}
    frames = [pd.DataFrame(raw), DataFrame(raw)]
    if indexed:
        frames = [f.set_index("id") for f in frames]
    calls = {
        "head": lambda g: g.head(1),
        "tail": lambda g: g.tail(1),
        "nth": lambda g: g.nth(0),
    }
    want = calls[op](frames[0].groupby("k", dropna=dropna))
    got = calls[op](frames[1].groupby("k", dropna=dropna))
    assert _labels(got) == _labels(want)
    assert _values(got) == _values(want)


def test_cumprod_of_integers_wraps_like_pandas():
    raw = {"k": [1] * 45, "v": [3] * 45}
    got = DataFrame(raw).groupby("k")["v"].cumprod().to_pandas()
    column = got["v"] if hasattr(got, "columns") else got
    want = pd.DataFrame(raw).groupby("k")["v"].cumprod()
    assert str(column.dtype) == "int64"
    assert column.tolist() == want.tolist()


@pytest.mark.parametrize("lazy", [False, True])
@pytest.mark.parametrize("dtype", ["int64", "uint64"])
def test_cumprod_of_integers_is_the_wrapped_integer_product(lazy, dtype):
    raw = {"k": [1] * 45 + [2] * 3, "v": [3] * 45 + [5, 7, 11]}
    frame = DataFrame(raw).cast({"v": dtype}) if dtype != "int64" else DataFrame(raw)
    group = frame.lazy().group_by("k") if lazy else frame.group_by("k")
    got = group.cumprod()
    got = (got.collect() if lazy else got).to_pandas()["v"]
    want = pd.DataFrame(raw).astype({"v": dtype}).groupby("k")["v"].cumprod()
    assert str(got.dtype) == dtype
    assert got.tolist() == want.tolist()


SHIFT_RAW = {
    "k": [1, 1, 2, 2],
    "n": pd.array([1, None, 3, 4], dtype="Int64"),
    "s": ["a", "b", "c", "d"],
    "b": [True, False, True, True],
}


def _shifted(frame, col, fill):
    out = frame.groupby("k")[col].shift(1, fill_value=fill)
    out = out.to_pandas() if hasattr(out, "to_pandas") else out
    column = out[col] if isinstance(out, pd.DataFrame) else out
    return str(column.dtype), column.tolist()


@pytest.mark.parametrize("col,fill", [("s", "x"), ("b", True), ("n", 0), ("n", -5)])
def test_shift_fill_value_as_pandas(col, fill):
    want = _shifted(pd.DataFrame(SHIFT_RAW), col, fill)
    got = _shifted(DataFrame.from_pandas(pd.DataFrame(SHIFT_RAW)), col, fill)
    assert got[1] == want[1]
    assert got[0].lower().replace("string", "str") == want[0].lower().replace("string", "str")


@pytest.mark.parametrize(
    "col,fill", [("s", 1.5), ("s", 0), ("s", True), ("b", 0), ("b", "x"), ("b", 1.5), ("n", "x")]
)
def test_shift_fill_value_that_does_not_fit_is_a_type_error(col, fill):
    frame = DataFrame.from_pandas(pd.DataFrame(SHIFT_RAW))
    with pytest.raises(TypeError):
        frame.groupby("k")[col].shift(1, fill_value=fill)
    # The frame is not changed by the refused call.
    assert frame["s"].to_list() == ["a", "b", "c", "d"]


def test_frame_shift_fill_value_per_column():
    raw = {"k": [1, 1, 2, 2], "s": ["a", "b", "c", "d"], "t": ["w", "x", "y", "z"]}
    want = pd.DataFrame(raw).groupby("k").shift(1, fill_value="-")
    got = DataFrame(raw).groupby("k").shift(1, fill_value="-").to_pandas()
    assert got["s"].tolist() == want["s"].tolist()
    assert got["t"].tolist() == want["t"].tolist()
    with pytest.raises(TypeError):
        DataFrame(raw).groupby("k").shift(1, fill_value=0)


@pytest.mark.parametrize("dropna", [True, False])
def test_get_group_of_a_one_tuple_missing_key_is_a_key_error(dropna):
    with pytest.raises(KeyError):
        pd.DataFrame(PICK_RAW).groupby("k", dropna=dropna).get_group((NAN,))
    with pytest.raises(KeyError):
        DataFrame(PICK_RAW).groupby("k", dropna=dropna).get_group((NAN,))
