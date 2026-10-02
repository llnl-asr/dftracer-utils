"""duql wildcard paths run through the TraceViewer and the builder, each
against the same query with the paths spelled out."""

import gzip
import json

import pytest

import dftracer.utils as dftu
from dftracer.utils.duql import c, fn, source

N = 60


def _record(i):
    return {
        "ph": "X",
        "name": "ev%d" % i,
        "cat": "POSIX",
        "pid": 1,
        "tid": 1,
        "ts": 1000 + 10 * i,
        "dur": 5,
        "args": {
            "counters": {"a": {"p50": i}, "b": {"p50": i % 7}, "c": {"p50": 2 * i}},
            "pos": [i + k for k in range(12)],
        },
    }


@pytest.fixture(scope="module")
def path(tmp_path_factory):
    p = str(tmp_path_factory.mktemp("wild") / "wild.pfw.gz")
    with gzip.open(p, "wt") as f:
        for i in range(N):
            f.write(json.dumps(_record(i)) + "\n")
    with dftu.Indexer(files=[p]) as indexer:
        indexer.ensure_indexed()
    return p


def _table(path, q):
    return dftu.TraceViewer(path).duql(q).collect().to_dict()


def test_select_expands_to_one_column_per_match(path):
    got = _table(path, "select name, args.counters.*.p50")
    want = _table(
        path,
        "select name, args.counters.a.p50, args.counters.b.p50, args.counters.c.p50",
    )
    assert got == want
    assert list(got) == [
        "name",
        "args.counters.a.p50",
        "args.counters.b.p50",
        "args.counters.c.p50",
    ]


def test_positions_are_in_numeric_order(path):
    got = _table(path, "select args.pos.*")
    assert list(got) == ["args.pos.%d" % k for k in range(12)]


def test_any_and_all_over_a_pattern(path):
    anyq = _table(path, "where any(args.counters.*.p50) > 100 | select name")
    anyw = _table(
        path,
        "where args.counters.a.p50 > 100 or args.counters.b.p50 > 100 or"
        " args.counters.c.p50 > 100 | select name",
    )
    assert anyq == anyw
    assert anyq["name"] == ["ev%d" % i for i in range(51, N)]
    allq = _table(path, "where all(args.counters.*.p50) < 3 | select name")
    assert allq["name"] == ["ev0", "ev1"]


def test_builder_renders_and_runs_a_pattern(path):
    q = source(path).where(fn.any(c("args.counters.*.p50")) > 100).select("name")
    assert "any(args.counters.*.p50) > 100" in q.text()
    assert q.collect().to_dict()["name"] == ["ev%d" % i for i in range(51, N)]
    sel = source(path).select("name", "args.counters.*.p50").take(1)
    assert list(sel.collect().to_dict()) == [
        "name",
        "args.counters.a.p50",
        "args.counters.b.p50",
        "args.counters.c.p50",
    ]


def test_a_pattern_with_no_match_names_itself(path):
    with pytest.raises(Exception, match=r"no field matches the pattern 'args\.nope\.\*'"):
        _table(path, "select args.nope.*")
