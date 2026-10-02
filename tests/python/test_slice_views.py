#!/usr/bin/env python3
"""Series and DataFrame row ranges equal pyarrow's slice for every layout."""

import pytest

from dftracer.utils import DataFrame, Series

pa = pytest.importorskip("pyarrow")

STRINGS = [
    "read",
    None,
    "a string that is longer than twelve bytes",
    "",
    "write",
    None,
    "twelve bytes",
    "x",
    "another string that is longer than twelve",
    "0123456789abcdef",
    "y",
    None,
    "z",
    "close",
    "open",
    "seek",
    "stat",
    "fsync",
    "mmap",
]
BINARIES = [None if s is None else s.encode() for s in STRINGS]

RANGES = [(o, n) for o in (0, 1, 3, 7, 8, 9, 16) for n in (0, 1, 5, 8, 9, 100)]


def _arrays():
    return {
        "string": pa.array(STRINGS, pa.string()),
        "large_string": pa.array(STRINGS, pa.large_string()),
        "binary": pa.array(BINARIES, pa.binary()),
        "string_view": pa.array(STRINGS, pa.string_view()),
        "binary_view": pa.array(BINARIES, pa.binary_view()),
        "dictionary": pa.array(STRINGS).dictionary_encode(),
    }


@pytest.mark.parametrize("kind", sorted(_arrays()))
def test_series_slice_matches_pyarrow(kind):
    arr = _arrays()[kind]
    series = Series.from_arrow(arr)
    for offset, length in RANGES:
        want = arr.slice(offset, length).to_pylist()
        assert series.slice(offset, length).to_list() == want, (offset, length)


def test_slice_outlives_base_and_nests():
    arr = pa.array(STRINGS, pa.string_view())
    series = Series.from_arrow(arr)
    cut = series.slice(2, 12)
    del series
    assert cut.to_list() == STRINGS[2:14]
    assert cut.slice(3, 4).to_list() == STRINGS[5:9]


def test_numeric_and_bool_slice_with_nulls():
    ints = [1, None, 3, 4, None, 6, 7, 8, 9, None, 11]
    bools = [True, None, False, True, True, None, False, True, True, True, False]
    for values, typ in ((ints, pa.int64()), (bools, pa.bool_())):
        arr = pa.array(values, typ)
        series = Series.from_arrow(arr)
        for offset, length in RANGES:
            want = arr.slice(offset, length).to_pylist()
            assert series.slice(offset, length).to_list() == want


@pytest.mark.parametrize("kind", ["string", "string_view", "dictionary"])
def test_dataframe_slice_head_tail(kind):
    arr = _arrays()[kind]
    df = DataFrame({"k": Series.from_arrow(arr), "v": Series(list(range(len(STRINGS))))})
    want_k = arr.to_pylist()
    got = df.slice(3, 9)
    assert got["k"].to_list() == want_k[3:12]
    assert got["v"].to_list() == list(range(3, 12))
    assert df.head(4)["k"].to_list() == want_k[:4]
    assert df.tail(4)["k"].to_list() == want_k[-4:]
