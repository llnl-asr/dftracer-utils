#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Null dictionary entries read the same as null values: group keys, counts."""

import pytest

from dftracer.utils import DataFrame, Series


def _keys(distinct, repeat):
    return ([f"a{i}" for i in range(distinct)] + ["b", "a", "", None]) * repeat


def _groups(keys):
    frame = DataFrame.from_dict({"v": [1] * len(keys)}).with_column("k", keys)
    out = frame.group_by("k", "sum:v", dropna=False)
    return sorted(zip(out["k"].to_list(), out["sum_v"].to_list()), key=repr)


@pytest.mark.parametrize("distinct,repeat", [(5, 400), (3000, 100)])
def test_group_by_reads_a_null_entry_as_the_null_key(distinct, repeat):
    keys = _keys(distinct, repeat)
    flat = Series(keys).str.extract("a([0-9]*)", 1)
    dictionary = Series(keys).dictionary_encode().str.extract("a([0-9]*)", 1)
    assert dictionary.encoding == 2
    expected = _groups(Series(flat.to_list()))
    got = _groups(dictionary)
    assert got == expected
    values = dict(got)
    assert None in values
    assert "" in values


def test_group_by_on_a_dictionary_with_own_nulls_and_empty_strings():
    keys = ["a", "", None, "b", "", None, "a"] * 4000
    dictionary = Series(keys).dictionary_encode()
    assert dictionary.encoding == 2
    assert _groups(dictionary) == _groups(Series(keys))


def test_null_count_sees_null_dictionary_entries():
    s = Series(["a1", "b", "a2", "b"]).dictionary_encode().str.extract("[0-9]", 0)
    assert s.encoding == 2
    assert s.null_count == 2
    assert s.to_list() == ["1", None, "2", None]


def test_null_count_through_a_nested_arrow_dictionary():
    pa = pytest.importorskip("pyarrow")
    inner = pa.DictionaryArray.from_arrays(pa.array([0, None, 1], pa.int32()), pa.array(["x", "y"]))
    outer = pa.DictionaryArray.from_arrays(pa.array([2, 1, 0, 2], pa.int32()), inner)
    s = Series.from_arrow(outer)
    assert s.null_count == 1
    assert s.to_list() == ["y", None, "x", "y"]
