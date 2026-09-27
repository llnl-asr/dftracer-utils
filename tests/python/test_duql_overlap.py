"""duql `lookup ... overlap` against a nested-loop reference."""

import gzip
import json
import random

import pytest

import dftracer.utils as dftu

pytest.importorskip("dftracer.utils.dftracer_utils_ext")


def _events(seed, phases, calls):
    rng = random.Random(seed)

    def span(name, tid):
        return {
            "ph": "X",
            "name": name,
            "cat": "c",
            "pid": rng.randint(1, 3),
            "tid": tid,
            "ts": rng.randrange(0, 5000),
            "dur": rng.randrange(0, 80 if name == "phase" else 15),
        }

    ev = [span("phase", i) for i in range(phases)]
    ev += [span("call", 10_000 + i) for i in range(calls)]
    return ev


def _reference(events):
    ph = [e for e in events if e["name"] == "phase"]
    out = []
    for c in (e for e in events if e["name"] == "call"):
        hits = [
            p["tid"]
            for p in ph
            if p["pid"] == c["pid"]
            and p["ts"] < c["ts"] + c["dur"]
            and c["ts"] < p["ts"] + p["dur"]
        ]
        out.extend((c["tid"], h) for h in hits) if hits else out.append((c["tid"], None))
    return out


def _viewer(tmp_path, events):
    path = tmp_path / "t.pfw.gz"
    path.write_bytes(gzip.compress(("\n".join(json.dumps(e) for e in events) + "\n").encode()))
    return dftu.TraceViewer(str(path))


LET = 'let ph = where name == "phase" | select pid, ts, dur, ptid = tid; '


@pytest.mark.parametrize("seed", [1, 2, 3])
def test_overlap_matches_a_nested_loop(tmp_path, seed):
    events = _events(seed, 400, 3000)
    got = (
        _viewer(tmp_path, events)
        .duql(LET + 'where name == "call" | lookup ph on pid overlap | select tid, ptid')
        .collect()
        .to_dict()
    )
    assert list(zip(got["tid"], got["ptid"])) == _reference(events)
    assert any(p is not None for p in got["ptid"]) and any(p is None for p in got["ptid"])


def test_overlap_into_keeps_one_row_per_call(tmp_path):
    events = _events(5, 200, 1000)
    got = (
        _viewer(tmp_path, events)
        .duql(
            LET + 'where name == "call" | lookup ph on pid overlap into m | select tid, len(m) as n'
        )
        .collect()
        .to_dict()
    )
    want = {}
    for tid, hit in _reference(events):
        want[tid] = want.get(tid, 0) + (hit is not None)
    assert dict(zip(got["tid"], got["n"])) == want
    assert len(got["tid"]) == len(want)
