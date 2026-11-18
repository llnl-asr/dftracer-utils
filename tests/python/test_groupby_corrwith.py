# SPDX-License-Identifier: MIT
"""``GroupBy.corrwith``: per group, each column's Pearson correlation with the
paired column of another frame or with a Series, against pandas."""

import math

import pytest

import dftracer.utils as dft

pytest.importorskip("dftracer.utils.dftracer_utils_ext")
pd = pytest.importorskip("pandas")

pytestmark = pytest.mark.filterwarnings("ignore::FutureWarning", "ignore::DeprecationWarning")

DATA = {
    "k": [2, 1, 1, 2, 3, 1, 2, 2, 4, 4],
    "a": [1.0, 2.0, 3.0, 4.0, 5.0, 7.0, 1.0, 9.0, 1.0, 2.0],
    "b": [3.0, 1.0, 2.0, 9.0, 8.0, None, 2.0, 4.0, 5.0, 5.0],
    "s": list("abcdefghij"),
}
OTHER = {
    "k": DATA["k"],
    "a": [2.0, 1.0, 5.0, 4.0, 3.0, 1.0, 1.0, 8.0, 1.0, 2.0],
    "b": [1.0, 2.0, 3.0, 4.0, 6.0, 5.0, 2.0, None, 5.0, 6.0],
}


def _close(got, want):
    assert len(got) == len(want)
    for g, w in zip(got, want):
        g_nan = g is None or (isinstance(g, float) and math.isnan(g))
        w_nan = w is None or (isinstance(w, float) and math.isnan(w))
        assert g_nan == w_nan, (got, want)
        if not g_nan:
            assert g == pytest.approx(w, abs=1e-12)


def test_corrwith_a_frame_matches_pandas():
    got = dft.DataFrame(DATA).groupby("k").corrwith(dft.DataFrame(OTHER))
    want = pd.DataFrame(DATA).groupby("k").corrwith(pd.DataFrame(OTHER))
    assert got["k"].to_list() == list(want.index)
    for c in ("a", "b"):
        _close(got[c].to_list(), want[c].tolist())
    _close(got["s"].to_list(), want["s"].tolist())
    assert got.columns == ["a", "b", "s"] and got.index_names() == ["k"]


def test_corrwith_a_series_and_drop_match_pandas():
    ours = dft.DataFrame({k: v for k, v in DATA.items() if k != "s"})
    theirs = pd.DataFrame({k: v for k, v in DATA.items() if k != "s"})
    got = ours.groupby("k").corrwith(dft.Series(OTHER["a"], name="a"))
    want = theirs.groupby("k").corrwith(pd.Series(OTHER["a"], name="a"))
    for c in ("a", "b"):
        _close(got[c].to_list(), want[c].tolist())
    dropped = dft.DataFrame(DATA).groupby("k").corrwith(dft.DataFrame(OTHER), drop=True)
    assert dropped.columns == ["a", "b"] and dropped.index_names() == ["k"]


def test_corrwith_errors():
    g = dft.DataFrame(DATA).groupby("k")
    with pytest.raises(ValueError, match="pair by position"):
        g.corrwith(dft.DataFrame({"a": [1.0, 2.0]}))
    with pytest.raises(NotImplementedError, match="pearson"):
        g.corrwith(dft.DataFrame(OTHER), method="spearman")
    with pytest.raises(ValueError, match="no column in common"):
        g.corrwith(dft.DataFrame({"z": DATA["a"]}))
