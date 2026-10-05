"""The first full scan of a trace leaves its path catalog, so the columns of a
trace indexed only by a query list every path."""

import gzip
import json

import pytest

import dftracer.utils as dftu

N = 200


def _record(i):
    return {
        "ph": "X",
        "name": "ev%d" % i,
        "cat": "POSIX",
        "pid": 1,
        "tid": 1,
        "ts": 1000 + 10 * i,
        "dur": 5,
        "args": {"size": i, "counters": {"a": {"p50": i}, "b": {"p50": i % 7}}},
    }


@pytest.fixture
def path(tmp_path):
    p = str(tmp_path / "first.pfw.gz")
    with gzip.open(p, "wt") as f:
        for i in range(N):
            f.write(json.dumps(_record(i)) + "\n")
    return p


def test_columns_after_collect_list_undeclared_paths(path):
    first = dftu.TraceViewer(path)
    assert len(first.collect()) == N
    columns = set(dftu.TraceViewer(path).columns)
    assert {"args.counters.a.p50", "args.counters.b.p50", "args.size"} <= columns


def test_wildcard_after_collect_matches_spelled_out_paths(path):
    dftu.TraceViewer(path).collect()
    wild = dftu.TraceViewer(path).duql("select args.counters.*.p50 | take 20").collect()
    spelled = (
        dftu.TraceViewer(path)
        .duql("select args.counters.a.p50, args.counters.b.p50 | take 20")
        .collect()
    )
    assert wild.columns == spelled.columns
    assert wild.to_dicts() == spelled.to_dicts()


def test_columns_before_any_collect_list_undeclared_paths(path):
    columns = set(dftu.TraceViewer(path).columns)
    assert {"args.counters.a.p50", "args.counters.b.p50", "args.size"} <= columns
