# SPDX-License-Identifier: MIT
"""``drop_nans`` with a subset keeps the buffers when no NaN is there, and
``hasnans`` counts a NaN as pandas does."""

import pytest

from dftracer.utils import DataFrame, Series

NAN = float("nan")


def _addr(frame, name):
    return int(frame[name].to_numpy().__array_interface__["data"][0])


@pytest.mark.parametrize("subset", ["a", ["a"], ["a", "b"]])
def test_drop_nans_with_a_subset_and_no_nan_keeps_the_buffers(subset):
    df = DataFrame({"a": [1.0, 2.0, 3.0], "b": [1, 2, 3]})
    out = df.drop_nans(subset)
    assert out["a"].to_list() == [1.0, 2.0, 3.0]
    assert _addr(out, "a") == _addr(df, "a")
    assert _addr(out, "b") == _addr(df, "b")


def test_drop_nans_with_a_subset_still_drops_a_nan_row():
    df = DataFrame({"a": [1.0, NAN, 3.0], "b": [1, 2, 3]})
    assert df.drop_nans(["a"])["b"].to_list() == [1, 3]
    assert df.drop_nans(["b"])["b"].to_list() == [1, 2, 3]


@pytest.mark.parametrize("values", [[1.0, NAN, 3.0], [1.0, None], [1, 2], [1.0, 2.0]])
def test_hasnans_matches_pandas(values):
    pd = pytest.importorskip("pandas")
    assert Series.from_list(values).hasnans == pd.Series(values).hasnans
