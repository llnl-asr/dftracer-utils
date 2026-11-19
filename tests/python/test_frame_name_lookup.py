# SPDX-License-Identifier: MIT
"""Name lookups on a wide frame stay right after the handle's columns change."""

import pytest

from dftracer.utils import DataFrame, Series

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

WIDTH = 100


def _wide():
    return DataFrame({f"c{i}": Series([i, i + 1]) for i in range(WIDTH)})


def _prime(df):
    for i in range(WIDTH):
        assert df[f"c{i}"].to_list() == [i, i + 1]
    assert "c50" in df and "zz" not in df


def test_pop_in_the_middle_moves_the_lookups():
    df = _wide()
    _prime(df)
    df.pop("c50")
    assert "c50" not in df
    assert df["c51"].to_list() == [51, 52]
    assert df["c99"].to_list() == [99, 100]
    assert list(df.columns)[50] == "c51"


def test_insert_in_the_middle_moves_the_lookups():
    df = _wide()
    _prime(df)
    df.insert(40, "fresh", Series([7, 8]))
    assert df["fresh"].to_list() == [7, 8]
    assert list(df.columns).index("fresh") == 40
    assert df["c40"].to_list() == [40, 41]
    assert df["c99"].to_list() == [99, 100]


def test_setitem_of_a_new_column_is_found():
    df = _wide()
    _prime(df)
    df["fresh"] = Series([5, 6])
    assert df["fresh"].to_list() == [5, 6]
    assert df["c0"].to_list() == [0, 1]


def test_pop_then_insert_back_under_another_name():
    df = _wide()
    _prime(df)
    column = df.pop("c50")
    df.insert(50, "renamed", column)
    assert "c50" not in df
    assert df["renamed"].to_list() == [50, 51]
    assert list(df.columns)[50] == "renamed"


def test_rename_result_does_not_disturb_the_original():
    df = _wide()
    _prime(df)
    other = df.rename({"c50": "moved"})
    assert "moved" in other and "c50" not in other
    assert "c50" in df and "moved" not in df
    assert other["moved"].to_list() == [50, 51]
    assert df["c50"].to_list() == [50, 51]
