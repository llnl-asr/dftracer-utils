"""duql `lookup ... asof` against pandas merge_asof."""

import gzip
import json
import math
import random

import pytest

import dftracer.utils as dftu

pytest.importorskip("dftracer.utils.dftracer_utils_ext")
pd = pytest.importorskip("pandas")

QUERY = (
    'let s = where name == "smp" | select pid, t = ts, v = dur; '
    'where name == "ev" | lookup s on pid asof ts == t {clause} | select tid, pid, ts, v'
)


def _trace(tmp_path, events):
    path = tmp_path / "t.pfw.gz"
    path.write_bytes(gzip.compress(("\n".join(json.dumps(e) for e in events) + "\n").encode()))
    return dftu.TraceViewer(str(path))


def _random_events(seed, n):
    rng = random.Random(seed)
    events = []
    for i in range(n):
        smp = rng.random() < 0.4
        events.append(
            {
                "ph": "X",
                "name": "smp" if smp else "ev",
                "cat": "POSIX",
                "pid": rng.randint(1, 4),
                "tid": i,
                "ts": rng.randrange(0, 400),
                "dur": rng.randrange(0, 1000) if smp else 0,
            }
        )
    return events


def _expected(events, direction, tolerance):
    left = pd.DataFrame([e for e in events if e["name"] == "ev"])
    right = pd.DataFrame([e for e in events if e["name"] == "smp"])
    right = right.rename(columns={"ts": "t", "dur": "v"})[["pid", "t", "v"]]
    right = right.sort_values("t", kind="stable")
    merged = pd.merge_asof(
        left.sort_values("ts", kind="stable"),
        right,
        left_on="ts",
        right_on="t",
        by="pid",
        direction=direction,
        tolerance=tolerance,
    )
    return {
        int(r.tid): (None if math.isnan(r.v) else int(r.v)) for r in merged.itertuples(index=False)
    }


@pytest.mark.parametrize("direction", ["backward", "forward", "nearest"])
@pytest.mark.parametrize("tolerance", [None, 7])
@pytest.mark.parametrize("seed", [1, 2, 3])
def test_asof_matches_merge_asof(tmp_path, direction, tolerance, seed):
    events = _random_events(seed, 3000)
    clause = direction + (f" within {tolerance}" if tolerance is not None else "")
    got = _trace(tmp_path, events).duql(QUERY.format(clause=clause)).collect().to_dict()
    ours = dict(zip(got["tid"], got["v"]))
    assert ours == _expected(events, direction, tolerance)
    assert len(got["tid"]) == sum(e["name"] == "ev" for e in events)
    assert any(v is not None for v in got["v"])


def test_asof_keeps_input_order(tmp_path):
    events = _random_events(9, 2000)
    got = _trace(tmp_path, events).duql(QUERY.format(clause="")).collect().to_dict()
    assert got["tid"] == [e["tid"] for e in events if e["name"] == "ev"]


def test_asof_null_key_and_time(tmp_path):
    events = [
        {"ph": "X", "name": "smp", "pid": 1, "tid": 0, "ts": 10, "dur": 5},
        {"ph": "X", "name": "smp", "pid": 1, "tid": 1, "dur": 6},
        {"ph": "X", "name": "smp", "tid": 2, "ts": 11, "dur": 7},
        {"ph": "X", "name": "ev", "pid": 1, "tid": 3, "ts": 12},
        {"ph": "X", "name": "ev", "tid": 4, "ts": 12},
        {"ph": "X", "name": "ev", "pid": 1, "tid": 5},
    ]
    got = _trace(tmp_path, events).duql(QUERY.format(clause="")).collect().to_dict()
    assert dict(zip(got["tid"], got["v"])) == {3: 5, 4: None, 5: None}


def test_asof_equal_times(tmp_path):
    events = [
        {"ph": "X", "name": "smp", "pid": 1, "tid": 0, "ts": 10, "dur": 1},
        {"ph": "X", "name": "smp", "pid": 1, "tid": 1, "ts": 10, "dur": 2},
        {"ph": "X", "name": "ev", "pid": 1, "tid": 2, "ts": 10},
    ]
    for direction, want in [("backward", 2), ("forward", 1), ("nearest", 2)]:
        got = _trace(tmp_path, events).duql(QUERY.format(clause=direction)).collect().to_dict()
        assert got["v"] == [want], direction


def test_asof_refusals(tmp_path):
    tv = _trace(tmp_path, _random_events(4, 50))
    for clause in ["into m", "within -1", "within 1.5"]:
        with pytest.raises(Exception):
            tv.duql(QUERY.format(clause=clause)).collect()
    with pytest.raises(Exception, match="asof"):
        tv.duql(
            'let s = where name == "smp" | select pid, t = name, v = dur; '
            'where name == "ev" | lookup s on pid asof ts == t'
        ).collect()
