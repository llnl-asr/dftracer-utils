#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Arrow C Stream Interop: ``pa.chunked_array(series)``, ``pa.table(frame)`` and
``pa.RecordBatchReader.from_stream(plan)``.

The ``LazyFrame`` stream runs the plan in a background task and releases the
GIL while ``get_next`` waits, so a consumer that holds the GIL still reads it.
"""

import subprocess
import sys

import pytest

pa = pytest.importorskip("pyarrow")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import DataFrame, col  # noqa: E402


def _table():
    return pa.table(
        {
            "a": pa.array([1, 2, 3, 4], pa.int64()),
            "b": pa.array([1.5, 2.5, 3.5, 4.5], pa.float64()),
            "s": pa.array(["x", "y", "x", "z"], pa.string()),
        }
    )


def _frame():
    return DataFrame.from_arrow(_table())


def test_series_stream_is_a_chunked_array():
    column = dft.Series.from_arrow(_table().column("a"))
    assert pa.chunked_array(column).to_pylist() == [1, 2, 3, 4]


def test_dataframe_stream_is_a_table():
    table = _table()
    assert pa.table(_frame()).equals(table)


def test_lazyframe_stream_matches_collect():
    plan = _frame().lazy().filter(col("a") > 2)
    assert pa.table(plan).equals(pa.table(plan.collect()))


def test_a_lazyframe_reader_closes_early():
    reader = pa.RecordBatchReader.from_stream(_frame().lazy())
    batch = reader.read_next_batch()
    assert batch.num_rows == 4
    reader.close()


def test_polars_reads_every_stream():
    pl = pytest.importorskip("polars")
    # pl.DataFrame is the Polars entry for a C stream; from_arrow reads one as a
    # struct Series (Polars 2.1).
    assert pl.DataFrame(_frame()).to_dict(as_series=False) == {
        "a": [1, 2, 3, 4],
        "b": [1.5, 2.5, 3.5, 4.5],
        "s": ["x", "y", "x", "z"],
    }
    assert pl.DataFrame(_frame().lazy()).height == 4


_HOLD_GIL_SCRIPT = """
import ctypes
import numpy as np
from dftracer.utils import DataFrame, col

class Stream(ctypes.Structure):
    _fields_ = [(n, ctypes.c_void_p) for n in
                ("get_schema", "get_next", "get_last_error", "release", "private_data")]

class Array(ctypes.Structure):
    _fields_ = [(n, ctypes.c_int64) for n in
                ("length", "null_count", "offset", "n_buffers", "n_children")] + [
                (n, ctypes.c_void_p) for n in
                ("buffers", "children", "dictionary", "release", "private_data")]

frame = DataFrame.from_numpy({"a": np.arange(100_000, dtype=np.int64)}, ["a"])
cap = frame.lazy().filter(col("a") > 10).__arrow_c_stream__()
del frame

get_pointer = ctypes.pythonapi.PyCapsule_GetPointer
get_pointer.restype = ctypes.c_void_p
get_pointer.argtypes = [ctypes.py_object, ctypes.c_char_p]
stream = Stream.from_address(get_pointer(cap, b"arrow_array_stream"))

# PYFUNCTYPE keeps the GIL held across the call, as a C consumer that never
# releases it would.
get_next = ctypes.PYFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)(
    stream.get_next)
rows = 0
while True:
    out = Array()
    assert get_next(ctypes.addressof(stream), ctypes.addressof(out)) == 0
    if not out.release:
        break
    rows += out.length
    ctypes.PYFUNCTYPE(None, ctypes.c_void_p)(out.release)(ctypes.addressof(out))
del cap
print(rows)
"""


def test_a_stream_consumer_that_holds_the_gil_does_not_deadlock():
    pytest.importorskip("numpy")
    result = subprocess.run(
        [sys.executable, "-c", _HOLD_GIL_SCRIPT],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "99989"


@pytest.mark.parametrize("kind", [pa.string(), pa.large_string(), pa.binary(), pa.large_binary()])
def test_a_column_from_two_batches_exports_as_one_valid_array(kind):
    def batch(values):
        return pa.record_batch({"s": pa.array(values, kind)})

    first, second = (["a", "bb"], ["ccc", None]), (["dddddddddddddddd"], ["e"])
    if kind in (pa.binary(), pa.large_binary()):
        first = tuple([None if v is None else v.encode() for v in part] for part in first)
        second = tuple([None if v is None else v.encode() for v in part] for part in second)
    table = pa.Table.from_batches([batch(p) for p in first + second])
    got = pa.array(DataFrame.from_arrow(table)["s"])
    got.validate(full=True)
    assert got.type == kind
    assert got.to_pylist() == table.column("s").to_pylist()


@pytest.mark.parametrize(
    "kind",
    [
        pa.list_(pa.int64()),
        pa.large_list(pa.int64()),
        pa.list_(pa.int64(), 2),
        pa.map_(pa.string(), pa.int64()),
    ],
    ids=["list", "large_list", "fixed_size_list", "map"],
)
def test_a_nested_column_from_two_batches_imports_and_exports(kind):
    if pa.types.is_map(kind):
        first, second = [[("a", 1)], None], [[("b", 2), ("c", 3)]]
    else:
        first, second = [[1, 2], None], [[3, 4]]
    table = pa.Table.from_batches(
        [pa.record_batch({"x": pa.array(v, kind)}) for v in (first, second)]
    )
    frame = DataFrame.from_arrow(table)
    # the batches stay chunks: nothing is joined on import
    assert frame["x"].encoding == 5
    exported = pa.table(frame).column("x")
    got = exported.combine_chunks()
    got.validate(full=True)
    assert got.type == kind
    assert got.to_pylist() == table.column("x").to_pylist()
    # one array of the whole column is joined, its text in its own type
    whole = pa.array(frame["x"])
    whole.validate(full=True)
    assert whole.type == kind
    assert whole.to_pylist() == table.column("x").to_pylist()


def test_a_nested_column_from_many_batches_shares_their_values():
    kind = pa.struct([("a", pa.list_(pa.int64())), ("b", pa.string())])
    batches = [
        pa.record_batch({"x": pa.array([{"a": [i, i + 1], "b": f"s{i}"}, None], kind)})
        for i in range(4)
    ]
    table = pa.Table.from_batches(batches)
    frame = DataFrame.from_arrow(table)
    assert frame["x"].encoding == 5
    out = pa.table(frame).column("x")
    assert out.type == kind
    assert out.num_chunks == table.column("x").num_chunks
    for mine, theirs in zip(out.chunks, table.column("x").chunks):
        values = mine.field("a").values.buffers()[1]
        assert values.address == theirs.field("a").values.buffers()[1].address
    assert out.to_pylist() == table.column("x").to_pylist()
