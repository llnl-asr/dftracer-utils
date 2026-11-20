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
