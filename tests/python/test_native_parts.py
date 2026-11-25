# SPDX-License-Identifier: MIT
"""The native parts of a Series that numpy and pandas read with no copy and no
pyarrow: _dictionary_parts, _series_from_dictionary, _values_buffer and
_null_bytes."""

import pytest

np = pytest.importorskip("numpy")
ext = pytest.importorskip("dftracer.utils.dftracer_utils_ext")

from dftracer.utils import Series  # noqa: E402


def _shares(view, series):
    return np.shares_memory(np.asarray(view), np.asarray(series))


def test_dictionary_parts_values_and_sharing():
    col = Series.from_list(["b", "a", "b", None, "a"]).dictionary_encode()
    codes, values = col._dictionary_parts()
    assert values.to_list()[:2] == ["b", "a"]
    nulls = list(codes._null_bytes())
    got = [
        None if n else values.item(int(c))
        for n, c in zip(nulls, np.asarray(codes._values_buffer()))
    ]
    assert got == ["b", "a", "b", None, "a"]
    assert nulls == [0, 0, 0, 1, 0]
    again, _ = col._dictionary_parts()
    assert np.shares_memory(np.asarray(codes._values_buffer()), np.asarray(again._values_buffer()))


def test_dictionary_parts_rejects_other_encodings():
    with pytest.raises(TypeError):
        Series.from_list([1, 2, 3])._dictionary_parts()


def test_series_from_dictionary_roundtrip_shares_codes():
    codes = Series.from_numpy(np.array([2, 0, 1, 0], dtype=np.int32))
    values = Series.from_list(["x", "y", "z"])
    col = ext._series_from_dictionary(codes, values)
    assert col.to_list() == ["z", "x", "y", "x"]
    out_codes, out_values = col._dictionary_parts()
    assert np.shares_memory(
        np.asarray(out_codes._values_buffer()), np.asarray(codes._values_buffer())
    )
    assert out_values.to_list() == ["x", "y", "z"]


def test_series_from_dictionary_null_and_negative_codes():
    codes = ext._series_from_numpy(
        np.array([0, 1, -1, 1], dtype=np.int32),
        np.array([False, False, False, True]),
    )
    col = ext._series_from_dictionary(codes, Series.from_list(["a", "b"]))
    assert col.to_list() == ["a", "b", None, None]


@pytest.mark.parametrize(
    "values", [Series.from_list(["a"]), Series.from_list([1])], ids=["str", "int"]
)
def test_series_from_dictionary_with_no_values_is_all_null(values):
    empty = values.head(0)
    codes = Series.from_numpy(np.array([-1, -1, -1], dtype=np.int32))
    col = ext._series_from_dictionary(codes, empty)
    assert col.to_list() == [None, None, None]
    assert col.dtype == empty.dtype


def test_series_from_dictionary_errors():
    values = Series.from_list(["a", "b"])
    with pytest.raises(ValueError):
        ext._series_from_dictionary(Series.from_numpy(np.array([0, 2], dtype=np.int32)), values)
    with pytest.raises(TypeError):
        ext._series_from_dictionary(Series.from_numpy(np.array([0, 1], dtype=np.int64)), values)
    with pytest.raises(TypeError):
        ext._series_from_dictionary(Series.from_list(["a"]).dictionary_encode(), values)


@pytest.mark.parametrize(
    "dtype",
    [
        "int8",
        "int16",
        "int32",
        "int64",
        "uint8",
        "uint16",
        "uint32",
        "uint64",
        "float32",
        "float64",
    ],
)
def test_values_buffer_numeric(dtype):
    array = np.arange(6).astype(dtype)
    col = Series.from_numpy(array)
    view = col._values_buffer()
    assert isinstance(view, memoryview) and view.readonly
    assert np.asarray(view).dtype == np.dtype(dtype)
    assert np.array_equal(np.asarray(view), array)
    assert _shares(view, col)


def test_values_buffer_with_nulls():
    col = ext._series_from_numpy(
        np.array([1.5, 2.5, 3.5]),
        np.array([False, True, False]),
    )
    with pytest.raises(BufferError):
        memoryview(col)
    view = col._values_buffer()
    out = np.asarray(view)
    assert out[0] == 1.5 and out[2] == 3.5
    assert list(col._null_bytes()) == [0, 1, 0]


@pytest.mark.parametrize("rows", [1, 3])
def test_a_values_buffer_keeps_the_column_it_was_made_from(rows):
    col = Series.from_numpy(np.arange(10, 10 + rows, dtype=np.int64))
    view = col._values_buffer()
    exporter = view.obj
    view.release()
    col._replace(Series.from_numpy(np.arange(4 - rows, dtype=np.int64)))
    again = np.asarray(memoryview(exporter))
    assert again.tolist() == list(range(10, 10 + rows))
    assert len(col) == 4 - rows


def test_values_buffer_errors():
    with pytest.raises(BufferError):
        Series.from_list(["a", "b"])._values_buffer()
    with pytest.raises(BufferError):
        Series.from_list(["a", "b"]).dictionary_encode()._values_buffer()
    with pytest.raises(BufferError):
        Series.from_list([True, False])._values_buffer()


def test_null_bytes():
    assert Series.from_list([1, 2, 3])._null_bytes() is None
    col = Series.from_list([1, None, 3, None, None, 6, 7, 8, None, 10])
    assert list(col._null_bytes()) == [0, 1, 0, 1, 1, 0, 0, 0, 1, 0]
    strings = Series.from_list(["a", None, "b"])
    assert list(strings._null_bytes()) == [0, 1, 0]
    encoded = strings.dictionary_encode()
    assert list(encoded._null_bytes()) == [0, 1, 0]


def test_null_mask_counts_a_null_dictionary_value():
    pa = pytest.importorskip("pyarrow")
    entries = pa.DictionaryArray.from_arrays(pa.array([0, 1, 0], pa.int32()), pa.array(["a", None]))
    assert list(Series.from_arrow(entries)._null_bytes()) == [0, 1, 0]


def test_null_mask_of_a_chunked_column_reads_each_chunk():
    pa = pytest.importorskip("pyarrow")
    chunked = Series.from_arrow(pa.chunked_array([pa.array([1, None]), pa.array([None, 4, 5])]))
    assert chunked.encoding == 5
    assert list(chunked._null_bytes()) == [0, 1, 1, 0, 0]
    plain = Series.from_arrow(pa.chunked_array([pa.array([1, 2]), pa.array([3])]))
    assert plain._null_bytes() is None
