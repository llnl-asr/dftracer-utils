#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test cases for the TraceWriter Python binding."""

import gzip
import json
import math
import os
import struct

import pytest

import dftracer.utils as dftu_utils
from dftracer.utils.trace import TraceViewer, TraceWriter


def read_lines(path):
    if str(path).endswith(".gz"):
        with gzip.open(path, "rb") as f:
            data = f.read()
    else:
        with open(path, "rb") as f:
            data = f.read()
    return data.decode("utf-8").splitlines()


def write_all(w):
    w.write_event("read", "POSIX", 100, 7, 11, 12, {"level": 2, "fhash": "ab"})
    w.write_counter("cpu", "sys", 200, {"user": 12.5}, pid=11, tid=12)
    w.write_aggregated("read", "POSIX", 300, {"count": 3}, pid=11, tid=12)
    w.write_metadata("HH", {"name": "host", "value": "n0"}, pid=11, tid=12)


def test_exported():
    assert dftu_utils.trace.TraceWriter is TraceWriter
    assert "TraceWriter" in dftu_utils.trace.__all__
    assert "TraceWriter" not in dftu_utils.__all__


def test_every_method_writes_one_json_line(tmp_path):
    path = tmp_path / "all.pfw"
    with TraceWriter(path, compress=False) as w:
        write_all(w)
        w.write_raw(b'{"name":"raw1","ph":1}\n')
        w.write_raw(bytearray(b'{"name":"raw2","ph":1}\n'))
        w.write_raw(memoryview(b'{"name":"raw3","ph":1}\n'))
        w.write_raw('{"name":"raw4","ph":1}\n')
    lines = read_lines(path)
    assert len(lines) == 8
    records = [json.loads(line) for line in lines]
    assert [r["ph"] for r in records[:4]] == [1, 2, 3, 4]
    assert all(isinstance(r["ph"], int) for r in records)
    event, counter, aggregated, metadata = records[:4]
    assert event["name"] == "read"
    assert event["cat"] == "POSIX"
    assert (event["ts"], event["dur"], event["pid"], event["tid"]) == (100, 7, 11, 12)
    assert event["args"] == {"level": 2, "fhash": "ab"}
    assert counter["args"] == {"user": 12.5}
    assert (counter["pid"], counter["tid"], counter["ts"]) == (11, 12, 200)
    assert aggregated["args"] == {"count": 3}
    assert metadata["ts"] == 0
    assert metadata["name"] == "HH"
    assert [r["name"] for r in records[4:]] == ["raw1", "raw2", "raw3", "raw4"]


def test_defaults_and_no_args(tmp_path):
    path = tmp_path / "defaults.pfw"
    with TraceWriter(str(path), compress=False) as w:
        w.write_event("a", "b", 1, 2)
        w.write_counter("a", "b", 1)
        w.write_metadata("m")
    records = [json.loads(line) for line in read_lines(path)]
    assert records[0]["pid"] == 0 and records[0]["tid"] == 0
    assert all("args" not in r for r in records)


def test_gzip_output_matches_plain_output(tmp_path):
    with TraceWriter(tmp_path / "a.pfw.gz", level=1, threads=2) as w:
        write_all(w)
    with TraceWriter(tmp_path / "a.pfw", compress=False) as w:
        write_all(w)
    assert read_lines(tmp_path / "a.pfw.gz") == read_lines(tmp_path / "a.pfw")


def test_args_nested_values(tmp_path):
    path = tmp_path / "nested.pfw"
    args = {
        "none": None,
        "yes": True,
        "neg": -5,
        "big": 2**64 - 1,
        "pi": 3.25,
        "text": 'a"b\ncé',
        "list": [1, 2.5, "x", None, [True, {"k": []}]],
        "dict": {"a": {"b": [1, 2]}, "c": ()},
        "tuple": (1, 2),
    }
    with TraceWriter(path, compress=False) as w:
        w.write_event("e", "c", 1, 2, args=args)
    (record,) = [json.loads(line) for line in read_lines(path)]
    expected = dict(args)
    expected["tuple"] = [1, 2]
    expected["dict"] = {"a": {"b": [1, 2]}, "c": []}
    assert record["args"] == expected


def test_nan_and_infinity_are_strings(tmp_path):
    path = tmp_path / "special.pfw"
    args = {
        "nan": math.nan,
        "inf": math.inf,
        "ninf": -math.inf,
        "nested": [math.nan, {"x": math.inf}, -math.inf],
    }
    with TraceWriter(path, compress=False) as w:
        w.write_counter("c", "sys", 1, args)
    (record,) = [json.loads(line) for line in read_lines(path)]
    assert record["args"] == {
        "nan": "NaN",
        "inf": "Infinity",
        "ninf": "-Infinity",
        "nested": ["NaN", {"x": "Infinity"}, "-Infinity"],
    }


def test_doubles_round_trip_bit_exact(tmp_path):
    import random

    rng = random.Random(7)
    values = []
    while len(values) < 2000:
        (d,) = struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))
        if math.isfinite(d):
            values.append(d)
    values += [0.0, -0.0, 1.0, 5e-324, 1.7976931348623157e308]
    path = tmp_path / "doubles.pfw"
    with TraceWriter(path, compress=False) as w:
        for v in values:
            w.write_counter("c", "sys", 1, {"v": v, "n": [v]})
    back = [json.loads(line)["args"] for line in read_lines(path)]
    for v, got in zip(values, back):
        assert struct.pack("<d", got["v"]) == struct.pack("<d", v)
        assert struct.pack("<d", got["n"][0]) == struct.pack("<d", v)


def test_bad_arguments(tmp_path):
    with TraceWriter(tmp_path / "bad.pfw", compress=False) as w:
        with pytest.raises(TypeError):
            w.write_event("e", "c", 1, 2, args=[1])
        with pytest.raises(TypeError):
            w.write_event("e", "c", 1, 2, args={1: 2})
        with pytest.raises(TypeError):
            w.write_event("e", "c", 1, 2, args={"a": object()})
        with pytest.raises(TypeError):
            w.write_event("e", "c", 1, 2, args={"a": [object()]})
        with pytest.raises(TypeError):
            w.write_event("e", "c", 1, 2, args={"a": {1: 2}})
        with pytest.raises(TypeError):
            w.write_event(b"e", "c", 1, 2)
        with pytest.raises(TypeError):
            w.write_counter("e", 5, 1)
        with pytest.raises(TypeError):
            w.write_metadata(None)
        with pytest.raises(TypeError):
            w.write_event("e", "c", 1, 2, args={"a": [1, [2, object()]]})
        with pytest.raises(OverflowError):
            w.write_event("e", "c", -1, 2)
        with pytest.raises(OverflowError):
            w.write_event("e", "c", 1, 2, args={"a": 2**64})
        with pytest.raises(ValueError):
            w.write_raw(b"no newline")
        w.write_event("ok", "c", 1, 2)
    assert len(read_lines(tmp_path / "bad.pfw")) == 1


def test_write_after_close_raises(tmp_path):
    w = TraceWriter(tmp_path / "closed.pfw", compress=False)
    w.write_event("e", "c", 1, 2)
    w.close()
    w.close()
    for call in (
        lambda: w.write_event("e", "c", 1, 2),
        lambda: w.write_counter("e", "c", 1),
        lambda: w.write_aggregated("e", "c", 1),
        lambda: w.write_metadata("e"),
        lambda: w.write_raw(b"x\n"),
    ):
        with pytest.raises(ValueError, match="closed"):
            call()
    assert len(read_lines(tmp_path / "closed.pfw")) == 1


def test_exception_in_block_discards_the_file(tmp_path):
    path = tmp_path / "discard.pfw.gz"
    with pytest.raises(RuntimeError, match="boom"):
        with TraceWriter(path) as w:
            w.write_event("e", "c", 1, 2)
            raise RuntimeError("boom")
    assert not os.path.exists(path)
    with pytest.raises(ValueError):
        w.write_event("e", "c", 1, 2)


def test_unwritable_path_raises(tmp_path):
    with pytest.raises(dftu_utils.errors.DFTUtilsError):
        TraceWriter(tmp_path / "missing_dir" / "x.pfw.gz")


def test_large_raw_and_many_records_split_into_members(tmp_path):
    path = tmp_path / "big.pfw.gz"
    big = b"".join(b'{"i":%d}\n' % i for i in range(20000))
    with TraceWriter(path, member_size=4096) as w:
        for i in range(2000):
            w.write_event("e", "c", i, 1, args={"i": i})
        w.write_raw(big)
    assert len(read_lines(path)) == 22000


def test_trace_viewer_reads_phase_counts(tmp_path):
    path = tmp_path / "view.pfw.gz"
    with TraceWriter(path) as w:
        w.write_raw("[\n")
        for i in range(5):
            w.write_event("read", "POSIX", 1000 + i * 10, 5, 1, 2)
        for i in range(3):
            w.write_counter("cpu", "sys", 2000 + i, {"v": 1.5}, 1, 2)
        for i in range(2):
            w.write_aggregated("agg", "POSIX", 3000 + i, {"count": 4}, 1, 2)

    def count(phase):
        return TraceViewer(str(path)).phase(phase).collect().height

    assert count("events") == 5
    assert count("counters") == 3
    assert count("aggregated") == 2
