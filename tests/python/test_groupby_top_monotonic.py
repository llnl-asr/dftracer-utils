# SPDX-License-Identifier: MIT
"""``SeriesGroupBy.nlargest`` / ``nsmallest`` and ``is_monotonic_increasing`` /
``is_monotonic_decreasing`` against pandas: ties, nulls, NaN, unsorted keys,
groups shorter than ``n`` and one-row groups."""

import math

import pytest

import dftracer.utils as dft

pytest.importorskip("dftracer.utils.dftracer_utils_ext")
pd = pytest.importorskip("pandas")

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

NAN = float("nan")
KEYS = [3, 1, 1, 2, 1, 2, 3, 1, 2, 1, 4]
FLOATS = [5.0, 2.0, NAN, 7.0, 2.0, 7.0, 5.0, 9.0, None, 2.0, 1.0]
INTS = [5, 2, None, 7, 2, 7, 5, 9, 1, 2, 4]


def _frame(values):
    return dft.DataFrame({"k": KEYS, "v": values}), pd.DataFrame({"k": KEYS, "v": values})


def _same(got, want):
    assert len(got) == len(want)
    for g, w in zip(got, want):
        g_nan = g is None or (isinstance(g, float) and math.isnan(g))
        w_nan = w is None or (isinstance(w, float) and math.isnan(w))
        assert g_nan == w_nan
        if not g_nan:
            assert g == w


@pytest.mark.parametrize("values", [FLOATS, INTS], ids=["floats", "ints"])
@pytest.mark.parametrize("n", [1, 2, 3, 10, 0])
@pytest.mark.parametrize("method", ["nlargest", "nsmallest"])
def test_top_matches_pandas(values, n, method):
    ours, theirs = _frame(values)
    got = getattr(ours.groupby("k")["v"], method)(n)
    want = getattr(theirs.groupby("k")["v"], method)(n)
    assert got.columns == ["k", "index", "v"]
    assert got["k"].to_list() == [k for k, _ in want.index]
    assert got["index"].to_list() == [i for _, i in want.index]
    _same(got["v"].to_list(), want.tolist())


def test_top_refuses_other_shapes():
    ours = dft.DataFrame({"k": [1, 1, 2], "v": [1.0, 3.0, 2.0], "index": [10, 11, 12]})
    with pytest.raises(TypeError, match="select one column"):
        ours.groupby("k").nlargest(1)
    for keep in ("last", "all"):
        with pytest.raises(NotImplementedError, match="only 'first'"):
            ours.groupby("k")["v"].nlargest(1, keep=keep)
    with pytest.raises(ValueError, match="already named 'index'"):
        ours.groupby("k")["index"].nlargest(1)


MONO_KEYS = [1, 1, 1, 2, 2, 2, 3, 4, 4, 5, 5, 5, 5]
MONO = {
    "floats": [1.0, 2.0, 2.0, 5.0, 4.0, 4.0, 7.0, 1.0, NAN, 3.0, 3.0, 3.0, 3.0],
    "ints_with_null": [1, 2, 2, 5, 4, 4, 7, 1, None, 3, 3, 3, 3],
    "decreasing": [9, 8, 8, 9, 8, 7, 1, 5, 4, 2, 2, 1, 1],
}


@pytest.mark.parametrize("name", MONO)
@pytest.mark.parametrize("attr", ["is_monotonic_increasing", "is_monotonic_decreasing"])
def test_monotonic_matches_pandas(name, attr):
    values = MONO[name]
    ours = dft.DataFrame({"k": MONO_KEYS, "v": values})
    theirs = pd.DataFrame({"k": MONO_KEYS, "v": values})
    got = getattr(ours.groupby("k")["v"], attr)
    want = getattr(theirs.groupby("k")["v"], attr)
    assert got.columns == ["k", "v"]
    assert got["k"].to_list() == list(want.index)
    assert got["v"].to_list() == [bool(x) for x in want.tolist()]


def test_monotonic_unsorted_keys_and_a_large_frame():
    keys = [(i * 7919) % 211 for i in range(100_000)]
    vals = [float((i // 211) + (1 if k % 5 == 0 and i > 50_000 else 0)) for i, k in enumerate(keys)]
    ours = dft.DataFrame({"k": keys, "v": vals})
    theirs = pd.DataFrame({"k": keys, "v": vals})
    for attr in ("is_monotonic_increasing", "is_monotonic_decreasing"):
        got = getattr(ours.groupby("k")["v"], attr)
        want = getattr(theirs.groupby("k")["v"], attr)
        assert got["v"].to_list() == [bool(x) for x in want.tolist()]


@pytest.mark.parametrize("values", [FLOATS, INTS], ids=["floats", "ints"])
def test_unique_is_the_first_seen_distinct_values_per_group(values):
    ours, theirs = _frame(values)
    got = ours.groupby("k")["v"].unique()
    want = theirs.groupby("k")["v"].unique()
    assert got["k"].to_list() == list(want.index)
    for g, w in zip(got["v"].to_list(), want.tolist()):
        _same(list(g), list(w))


def test_unique_on_text_and_with_unsorted_keys():
    keys = [2, 1, 2, 1, 2, 3, 1]
    words = ["b", "a", "b", "c", None, "a", "a"]
    got = dft.DataFrame({"k": keys, "w": words}).groupby("k")["w"].unique()
    want = pd.DataFrame({"k": keys, "w": words}).groupby("k")["w"].unique()
    assert got["k"].to_list() == list(want.index)
    for g, w in zip(got["w"].to_list(), want.tolist()):
        assert list(g) == [None if x is None or x is pd.NA or x != x else x for x in w]
