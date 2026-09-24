"""TraceViewer.rows(cls): records as schema class instances."""

import gzip
import json
from typing import Optional

import pytest

import dftracer.utils as dftu
from dftracer.utils import schemas
from dftracer.utils.schemas import Json, RecordSchema, field


class RowsAccess(RecordSchema, id="rows_access"):
    op: str
    status: int
    off: Optional[int] = field(path="io.off")
    tags: Optional[Json]


def _access(path):
    lines = []
    for i in range(300):
        rec = {"op": "read" if i % 2 == 0 else "write", "status": 200 + i % 3}
        if i % 3 == 0:
            rec["io"] = {"off": i * 10}
        if i % 5 == 0:
            rec["tags"] = {"b": [1, 2], "a": "x"}
        lines.append(json.dumps(rec))
    path.write_bytes(gzip.compress(("\n".join(lines) + "\n").encode()))
    return str(path)


def test_rows_yield_schema_instances(tmp_path):
    tv = dftu.TraceViewer(_access(tmp_path / "a.ndjson.gz"), record_schema=RowsAccess)
    rows = list(tv.rows(RowsAccess))
    assert len(rows) == 300
    assert all(isinstance(r, RowsAccess) and isinstance(r.status, int) for r in rows)
    by_off = sorted(r.off for r in rows if r.off is not None)
    assert by_off == [i * 10 for i in range(0, 300, 3)]
    assert sum(r.tags == '{"a":"x","b":[1,2]}' for r in rows) == 60
    assert sum(r.tags is None for r in rows) == 240


def test_rows_follow_filters(tmp_path):
    tv = dftu.TraceViewer(_access(tmp_path / "a.ndjson.gz"), record_schema=RowsAccess)
    rows = list(tv.query('op == "read"').rows(RowsAccess))
    assert len(rows) == 150
    assert {r.op for r in rows} == {"read"}


def test_rows_of_a_dftracer_subclass(tmp_path):
    class RowsDft(schemas.DFTracer, id="rows_dft"):
        size: Optional[int] = field(path="args.size")

    events = [
        {
            "ph": "X",
            "name": "read",
            "cat": "POSIX",
            "pid": 1,
            "tid": 1,
            "ts": 10 + i,
            "dur": 2,
            "args": {"size": i},
        }
        for i in range(20)
    ]
    path = tmp_path / "t.pfw.gz"
    path.write_bytes(gzip.compress(("\n".join(json.dumps(e) for e in events) + "\n").encode()))
    rows = list(dftu.TraceViewer(str(path)).rows(RowsDft))
    assert sorted(r.size for r in rows) == list(range(20))


def test_instances_take_keywords():
    r = RowsAccess(op="read", status=200)
    assert (r.op, r.status, r.off, r.tags) == ("read", 200, None, None)
    assert r == RowsAccess(op="read", status=200)
    with pytest.raises(TypeError, match="nope"):
        RowsAccess(nope=1)
