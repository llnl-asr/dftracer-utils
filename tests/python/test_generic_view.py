"""TraceViewer over JSON lines files of no trace format (the generic schema)."""

import gzip

import pyarrow as pa
import pytest

import dftracer.utils as dftu

OPS = ["read", "write", "close"]
RECORDS = 900


def _ndjson(path):
    with gzip.open(path, "wt") as out:
        for i in range(RECORDS):
            out.write('{"op":"%s","lat":%d,"io":{"off":%d}}\n' % (OPS[i % 3], i % 50, i * 4096))
    with dftu.Indexer(files=[path]) as ix:
        ix.ensure_indexed()
    return path


@pytest.fixture
def trace(tmp_path):
    return _ndjson(str(tmp_path / "g.ndjson.gz"))


def test_rows_are_named_by_path(trace):
    t = pa.table(dftu.TraceViewer(trace).collect())
    assert t.num_rows == RECORDS
    assert set(t.column_names) == {"op", "lat", "io.off"}
    assert set(dftu.TraceViewer(trace).schema) == {"op", "lat", "io.off"}


def test_filters_and_groups_read_paths(trace):
    rows = pa.table(dftu.TraceViewer(trace).duql('op == "read"').collect())
    assert rows.num_rows == RECORDS // 3

    t = pa.table(dftu.TraceViewer(trace).group_by("op").agg("count", "sum:lat").collect())
    got = {r["op"]: (r["count"], r["sum_lat"]) for r in t.to_pylist()}
    want = {
        op: (RECORDS // 3, sum(i % 50 for i in range(RECORDS) if OPS[i % 3] == op)) for op in OPS
    }
    assert got == want


def test_time_operations_need_a_time_role(trace):
    with pytest.raises(dftu.DFTUtilsValueError, match="needs a time role"):
        dftu.TraceViewer(trace).time_bucket(100)
