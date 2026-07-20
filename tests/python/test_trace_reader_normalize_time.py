#!/usr/bin/env python3
"""Tests for TraceReader normalize_time (CM time_metric scaling)."""

import gzip
import os

import pytest

import dftracer.utils as dft_utils

from .common import Environment

# Two X events in a trace declaring time_metric=NS. Native ts/dur are
# nanoseconds; microseconds = /1000, milliseconds = /1_000_000.
_EVENTS = [
    ("read", 5_000_000_000, 2_000_000),
    ("write", 5_000_003_000, 1_000_000),
]


def _write_ns_trace(env, gzipped=False, metric="NS"):
    name = "ns_trace.pfw.gz" if gzipped else "ns_trace.pfw"
    path = os.path.join(env.temp_dir, name)
    lines = ["[\n"]
    if metric is not None:
        lines.append(
            '{"id":0,"name":"CM","cat":"dft","pid":0,"tid":0,"ph":"M",'
            '"args":{"name":"time_metric","value":"%s"}}\n' % metric
        )
    for i, (nm, ts, dur) in enumerate(_EVENTS, start=1):
        lines.append(
            '{"id":%d,"name":"%s","cat":"posix","pid":1,"tid":1,'
            '"ts":%d,"dur":%d,"ph":"X"}\n' % (i, nm, ts, dur)
        )
    opener = gzip.open if gzipped else open
    with opener(path, "wt", encoding="utf-8") as f:
        f.writelines(lines)
    env.test_files.append(path)
    return path


def _json_events(path, **kwargs):
    with dft_utils.TraceReader(path) as reader:
        out = []
        for ev in reader.read_json(**kwargs):
            d = ev.to_dict()
            if d.get("name") in ("read", "write"):
                out.append((d["name"], d.get("ts"), d.get("dur")))
    return out


class TestNormalizeTimeJson:
    def test_native_default_is_unchanged(self):
        with Environment() as env:
            path = _write_ns_trace(env)
            assert _json_events(path) == _EVENTS

    def test_microseconds(self):
        with Environment() as env:
            path = _write_ns_trace(env)
            assert _json_events(path, normalize_time="us") == [
                ("read", 5_000_000, 2_000),
                ("write", 5_000_003, 1_000),
            ]

    def test_microseconds_via_enum(self):
        with Environment() as env:
            path = _write_ns_trace(env)
            assert _json_events(path, normalize_time=dft_utils.TimeUnit.US) == [
                ("read", 5_000_000, 2_000),
                ("write", 5_000_003, 1_000),
            ]

    def test_milliseconds(self):
        with Environment() as env:
            path = _write_ns_trace(env)
            assert _json_events(path, normalize_time="ms") == [
                ("read", 5_000, 2),
                ("write", 5_000, 1),
            ]

    def test_target_equals_native_is_noop(self):
        with Environment() as env:
            path = _write_ns_trace(env)
            assert _json_events(path, normalize_time="ns") == _EVENTS

    def test_absent_cm_defaults_to_us(self):
        # No CM: native unit is us, so requesting us must not rescale.
        with Environment() as env:
            path = _write_ns_trace(env, metric=None)
            assert _json_events(path, normalize_time="us") == _EVENTS

    def test_scaling_survives_query_filtering_out_cm(self):
        # A query that drops the CM metadata line must still resolve NS via
        # the header probe, so scaling stays correct.
        with Environment() as env:
            path = _write_ns_trace(env)
            got = _json_events(path, normalize_time="us", query='name == "read"')
            assert got == [("read", 5_000_000, 2_000)]

    def test_gzipped_trace(self):
        with Environment() as env:
            path = _write_ns_trace(env, gzipped=True)
            assert _json_events(path, normalize_time="us") == [
                ("read", 5_000_000, 2_000),
                ("write", 5_000_003, 1_000),
            ]

    def test_invalid_unit_rejected(self):
        with Environment() as env:
            path = _write_ns_trace(env)
            with pytest.raises(ValueError):
                _json_events(path, normalize_time="fortnights")


class TestNormalizeTimeArrow:
    def _arrow_events(self, path, **kwargs):
        pa = pytest.importorskip("pyarrow")
        with dft_utils.TraceReader(path) as reader:
            out = []
            for cap in reader.iter_arrow(normalize=True, **kwargs):
                d = pa.record_batch(cap).to_pydict()
                n = len(d["name"])
                for i in range(n):
                    if d["name"][i] in ("read", "write"):
                        out.append((d["name"][i], d["ts"][i], d.get("dur", [None] * n)[i]))
        return out

    def test_arrow_native_default(self):
        with Environment() as env:
            path = _write_ns_trace(env)
            assert self._arrow_events(path) == _EVENTS

    def test_arrow_microseconds(self):
        with Environment() as env:
            path = _write_ns_trace(env)
            assert self._arrow_events(path, normalize_time="us") == [
                ("read", 5_000_000, 2_000),
                ("write", 5_000_003, 1_000),
            ]

    def test_arrow_us_survives_query(self):
        with Environment() as env:
            path = _write_ns_trace(env)
            got = self._arrow_events(path, normalize_time="us", query='name == "read"')
            assert got == [("read", 5_000_000, 2_000)]
