# SPDX-License-Identifier: MIT
import numpy as np
import pandas as pd
import pytest

from dftracer.utils import DataFrame


def frame():
    return DataFrame.from_dict({"c": [3, 1], "a": [9, 8], "b": ["x", "y"]})


def test_columns_sorted_ascending():
    out = frame().sort_index(axis=1)
    assert list(out.columns) == ["a", "b", "c"]
    assert out.to_pandas().to_dict("list") == {"a": [9, 8], "b": ["x", "y"], "c": [3, 1]}


def test_axis_name_columns():
    assert list(frame().sort_index(axis="columns").columns) == ["a", "b", "c"]


def test_descending():
    assert list(frame().sort_index(ascending=False, axis=1).columns) == ["c", "b", "a"]


def test_axis_zero_is_unchanged():
    f = frame()
    assert list(f.sort_index().columns) == ["c", "a", "b"]
    assert f.sort_index(axis=0).to_pandas().equals(f.to_pandas())


def test_values_are_unchanged():
    f = frame()
    sorted_cols = f.sort_index(axis=1)
    for name in f.columns:
        assert sorted_cols[name].to_list() == f[name].to_list()


def test_bad_axis():
    with pytest.raises(ValueError):
        frame().sort_index(axis=2)


_PACKED_KEYS = {
    "small ints": ([2, -1, 2, 0, -1, 2], [3, 1, 1, 2, 0, 0]),
    "int8 and wide": (
        np.array([-128, 127, 0, -128, 127, 0], dtype=np.int8),
        [2**40, 0, 5, 1, 2**40, 5],
    ),
    "uint64 extremes": (
        np.array([2**64 - 1, 0, 2**63, 0, 2**64 - 1, 1], dtype=np.uint64),
        [1, 1, 0, 0, 0, 1],
    ),
    "int64 extremes": ([-(2**63), 2**63 - 1, -1, -(2**63), 0, 2**63 - 1], [0, 1, 0, 1, 1, 0]),
    "nulls": (
        pd.array([3, None, 1, 3, None, 1], dtype="Int64"),
        pd.array([None, 2, 2, 1, 1, None], dtype="Int64"),
    ),
}


@pytest.mark.parametrize("kind", list(_PACKED_KEYS))
@pytest.mark.parametrize("ascending", [[True, True], [False, True], [True, False], [False, False]])
@pytest.mark.parametrize("na_position", ["first", "last"])
def test_two_integer_keys_sort_as_pandas(kind, ascending, na_position):
    a, b = _PACKED_KEYS[kind]
    frame = pd.DataFrame({"a": a, "b": b, "row": range(6)})
    want = frame.sort_values(
        ["a", "b"], ascending=ascending, na_position=na_position, kind="stable"
    )
    got = DataFrame.from_pandas(frame).sort_values(
        ["a", "b"], ascending=ascending, na_position=na_position
    )
    assert got["row"].to_list() == want["row"].tolist()
