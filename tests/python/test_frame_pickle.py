# SPDX-License-Identifier: MIT
"""A DataFrame pickles with every protocol; protocol 5 carries its column buffers out of band."""

import gc
import pickle
import threading

import numpy as np
import pandas as pd
import pyarrow as pa
import pytest

from dftracer.utils import DataFrame, concat
from dftracer.utils import dftracer_utils_ext as _ext

CHUNKED = 5
DICTIONARY = 2


def sample() -> DataFrame:
    json_field = pa.field("j", pa.string(), metadata={"ARROW:extension:name": "arrow.json"})
    table = pa.table(
        {
            "i": pa.array([1, None, 3, 4], pa.int64()),
            "d": pa.array([1.5, -2.25, None, 1e300]),
            "s": pa.array(["", "alpha", None, "\u00e9"]),
            "b": pa.array([True, None, False, True]),
            "j": pa.array(['{"a":1}', "[1]", None, "true"]),
            "ts": pa.array([1, 2, None, 4], pa.timestamp("ms", tz="Europe/Paris")),
            "dec": pa.array([1, 2, None, 4], pa.decimal128(20, 3)),
            "tags": pa.array([["a", "b"], [], None, ["c"]], pa.list_(pa.string())),
            "st": pa.array([{"n": 1, "x": "a"}, None, {"n": 3, "x": None}, {"n": 4, "x": "d"}]),
            "row": pa.array([10, 11, 12, 13], pa.int64()),
        }
    )
    table = table.set_column(4, json_field, table.column("j"))
    return DataFrame.from_arrow(table).set_index("row")


def assert_same(got: DataFrame, want: DataFrame) -> None:
    assert got.columns == want.columns
    assert got.to_arrow().equals(want.to_arrow())
    pd.testing.assert_frame_equal(got.to_pandas(), want.to_pandas())
    assert got["j"].is_json == want["j"].is_json


@pytest.mark.parametrize("protocol", [2, 4, 5])
def test_round_trip_by_protocol(protocol):
    f = sample()
    back = pickle.loads(pickle.dumps(f, protocol=protocol))
    assert isinstance(back, DataFrame)
    assert_same(back, f)
    assert back["j"].is_json and not back["s"].is_json
    assert back.to_pandas().index.tolist() == [10, 11, 12, 13]


def test_out_of_band_round_trip():
    f = sample()
    buffers = []
    stream = pickle.dumps(f, protocol=5, buffer_callback=buffers.append)
    assert len(buffers) > 0
    assert_same(pickle.loads(stream, buffers=buffers), f)


def test_non_contiguous_columns_round_trip():
    f = sample()
    parts = [f.slice(0, 2), f.slice(2, 2)]
    chunked = concat(parts).collect()
    assert all(chunked[c].encoding == CHUNKED for c in ["i", "s"])
    frames = {
        "chunked": chunked,
        "sliced": f.slice(1, 2),
        "dictionary": DataFrame.from_arrow(
            pa.table({"k": pa.array(["x", "y", None, "x"]).dictionary_encode()})
        ),
    }
    assert frames["dictionary"]["k"].encoding == DICTIONARY
    for name, frame in frames.items():
        buffers = []
        stream = pickle.dumps(frame, protocol=5, buffer_callback=buffers.append)
        back = pickle.loads(stream, buffers=buffers)
        assert back.to_arrow().cast(frame.to_arrow().schema).equals(frame.to_arrow()), name


def test_large_frame_sends_its_columns_with_no_copy():
    n = 21_000_000
    f = DataFrame.from_pandas(pd.DataFrame({"x": np.arange(n, dtype=np.float64)}))
    column = np.frombuffer(f["x"], dtype=np.float64)
    buffers = []
    stream = pickle.dumps(f, protocol=5, buffer_callback=buffers.append)
    assert len(stream) < 1 << 20
    sent = [np.frombuffer(b.raw(), dtype=np.uint8) for b in buffers]
    assert sum(b.nbytes for b in sent) == n * 8
    assert any(b.ctypes.data == column.ctypes.data for b in sent)
    back = pickle.loads(stream, buffers=buffers)
    got = np.frombuffer(back["x"], dtype=np.float64)
    assert got.ctypes.data == column.ctypes.data
    assert got[-1] == n - 1


def test_loaded_frame_keeps_the_received_buffers_alive():
    f = sample()
    buffers = []
    stream = pickle.dumps(f, protocol=5, buffer_callback=buffers.append)
    received = [bytearray(b.raw()) for b in buffers]
    back = pickle.loads(stream, buffers=received)
    del received, buffers
    gc.collect()
    assert_same(back, f)


def test_received_buffers_are_released_when_another_thread_drops_the_frame():
    f = sample()
    buffers = []
    stream = pickle.dumps(f, protocol=5, buffer_callback=buffers.append)
    received = [bytearray(b.raw()) for b in buffers]
    box = [pickle.loads(stream, buffers=received)]
    held = max(received, key=len)
    with pytest.raises(BufferError):
        held.extend(b"x")
    thread = threading.Thread(target=box.clear)
    thread.start()
    thread.join()
    gc.collect()
    for b in received:
        b.extend(b"x")


def test_bad_header_or_buffers_raise():
    header, buffers = _ext._dataframe_to_buffers(sample())
    raw = [bytes(b) for b in buffers]
    assert DataFrame(_ext._dataframe_from_buffers(header, raw)).columns == sample().columns
    with pytest.raises(ValueError, match="not a DataFrame"):
        _ext._dataframe_from_buffers(header[: len(header) // 2], raw)
    with pytest.raises(ValueError, match="not a DataFrame"):
        _ext._dataframe_from_buffers(b"DFTB\x09\x00\x00\x00" + header[8:], raw)
    with pytest.raises(ValueError, match="not a DataFrame"):
        _ext._dataframe_from_buffers(header, raw[:-1])
    longest = max(range(len(raw)), key=lambda i: len(raw[i]))
    short = list(raw)
    short[longest] = short[longest][:-1]
    with pytest.raises(ValueError, match="not a DataFrame"):
        _ext._dataframe_from_buffers(header, short)
