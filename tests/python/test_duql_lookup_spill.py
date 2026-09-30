"""duql `lookup` of a side over the row cap and the memory budget."""

import gzip
import json
import random

import pytest

import dftracer.utils as dftu

pytest.importorskip("dftracer.utils.dftracer_utils_ext")


def _events(seed, calls, phases):
    rng = random.Random(seed)
    ev = [
        {
            "ph": "X",
            "name": "call",
            "cat": "c",
            "pid": rng.randint(1, 4),
            "tid": i,
            "ts": rng.randrange(10_000),
            "dur": rng.randrange(100),
        }
        for i in range(calls)
    ]
    ev += [
        {
            "ph": "X",
            "name": "phase",
            "cat": "c",
            "pid": rng.randint(1, 4),
            "tid": j,
            "ts": rng.randrange(10_000),
            "dur": 1000 + rng.randrange(1000),
        }
        for j in range(phases)
    ]
    return ev


def _viewer(tmp_path, events):
    path = tmp_path / "t.pfw.gz"
    path.write_bytes(gzip.compress(("\n".join(json.dumps(e) for e in events) + "\n").encode()))
    return dftu.TraceViewer(str(path))


@pytest.mark.parametrize("budget", [1024, 0])
def test_lookup_over_the_caps_equals_a_dictionary(tmp_path, monkeypatch, budget):
    monkeypatch.setenv("DUQL_LOOKUP_MAX_ROWS", "10")
    monkeypatch.setenv("DUQL_LOOKUP_MAX_BYTES", "1024")
    events = _events(3, 2500, 1200)
    viewer = _viewer(tmp_path, events)
    if budget:
        viewer = viewer.memory_budget(budget)
    got = (
        viewer.duql(
            'let p = where name == "phase" | select tid, pdur = dur; '
            'where name == "call" | lookup p on tid | select tid, pdur'
        )
        .collect()
        .to_dict()
    )
    want = {e["tid"]: e["dur"] for e in events if e["name"] == "phase"}
    calls = [e["tid"] for e in events if e["name"] == "call"]
    assert sorted(got["tid"]) == sorted(calls)
    for tid, pdur in zip(got["tid"], got["pdur"]):
        assert pdur == want.get(tid)
    assert any(t in want for t in calls) and any(t not in want for t in calls)


def test_lookup_into_over_the_caps_counts_matches(tmp_path, monkeypatch):
    monkeypatch.setenv("DUQL_LOOKUP_MAX_ROWS", "10")
    events = _events(4, 2000, 900)
    got = (
        _viewer(tmp_path, events)
        .memory_budget(2048)
        .duql(
            'let p = where name == "phase" | select pid, pt = tid; '
            'where name == "call" | lookup p on pid into m | select tid, len(m) as n'
        )
        .collect()
        .to_dict()
    )
    per_pid = {}
    for e in events:
        if e["name"] == "phase":
            per_pid[e["pid"]] = per_pid.get(e["pid"], 0) + 1
    want = {e["tid"]: per_pid.get(e["pid"], 0) for e in events if e["name"] == "call"}
    assert dict(zip(got["tid"], got["n"])) == want


@pytest.mark.parametrize("budget", [1024, 0])
def test_sub_queries_over_the_caps_equal_a_reference(tmp_path, monkeypatch, budget):
    monkeypatch.setenv("DUQL_LOOKUP_MAX_ROWS", "10")
    monkeypatch.setenv("DUQL_LOOKUP_MAX_BYTES", "1024")
    events = _events(5, 2000, 800)
    viewer = _viewer(tmp_path, events)
    if budget:
        viewer = viewer.memory_budget(budget)
    long_dur = {e["dur"] for e in events if e["dur"] > 1500}
    got = (
        viewer.duql(
            'where name == "phase" and not (dur in (from data | where dur > 1500 '
            "| select dur)) | select tid"
        )
        .collect()
        .to_dict()
    )
    want = [e["tid"] for e in events if e["name"] == "phase" and e["dur"] not in long_dur]
    assert sorted(got["tid"]) == sorted(want)

    got = (
        viewer.duql(
            'where name == "call" | derive n = (from data | where name == "phase" '
            "and pid == ^.pid | agg { c = count() }) | select tid, n"
        )
        .collect()
        .to_dict()
    )
    per_pid = {}
    for e in events:
        if e["name"] == "phase":
            per_pid[e["pid"]] = per_pid.get(e["pid"], 0) + 1
    want = {e["tid"]: per_pid.get(e["pid"], 0) for e in events if e["name"] == "call"}
    assert dict(zip(got["tid"], got["n"])) == want


def test_a_top_level_in_reads_distinct_rows_over_the_caps(tmp_path, monkeypatch):
    monkeypatch.setenv("DUQL_LOOKUP_MAX_ROWS", "10")
    events = _events(6, 1500, 600)
    got = (
        _viewer(tmp_path, events)
        .duql('where pid in (from data | where name == "phase" | select pid) | select tid')
        .collect()
        .to_dict()
    )
    pids = {e["pid"] for e in events if e["name"] == "phase"}
    assert len(got["tid"]) == sum(e["pid"] in pids for e in events)
