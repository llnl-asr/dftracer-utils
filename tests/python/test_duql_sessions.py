"""duql `session` against a plain-Python reference."""

import gzip
import json
import random

import pytest

import dftracer.utils as dftu

pytest.importorskip("dftracer.utils.dftracer_utils_ext")


def _events(seed: int, n: int):
    rng = random.Random(seed)
    return [
        {
            "ph": "X",
            "name": "read",
            "cat": "POSIX",
            "pid": rng.randint(1, 3),
            "tid": rng.randint(1, 2),
            "ts": rng.randrange(0, 2_000_000),
            "dur": rng.randrange(0, 400),
        }
        for _ in range(n)
    ]


def _reference(events, gap, span):
    """(pid, tid, session) -> (count, sum of dur), sessions by the spec rule."""
    order = sorted(
        range(len(events)), key=lambda i: (events[i]["pid"], events[i]["tid"], events[i]["ts"], i)
    )
    out = {}
    key = None
    sid = first = last_end = 0
    for i in order:
        e = events[i]
        k = (e["pid"], e["tid"])
        t, end = e["ts"], e["ts"] + e["dur"]
        if k != key or t - last_end > gap or (span and t - first > span):
            sid = 1 if k != key else sid + 1
            key, first, last_end = k, t, end
        else:
            last_end = max(last_end, end)
        n, d = out.get((*k, sid), (0, 0))
        out[(*k, sid)] = (n + 1, d + e["dur"])
    return out


@pytest.mark.parametrize("gap,span,text", [(500, 0, "500"), (1000, 20000, "1ms max 20ms")])
def test_sessions_match_a_reference(tmp_path, gap, span, text):
    events = _events(7, 5000)
    path = tmp_path / "t.pfw.gz"
    path.write_bytes(gzip.compress(("\n".join(json.dumps(e) for e in events) + "\n").encode()))
    tv = dftu.TraceViewer(str(path))
    got = (
        tv.duql(
            f"session pid, tid gap {text} | group pid, tid, session {{ n = count(), d = sum(dur) }}"
        )
        .collect()
        .to_dict()
    )
    rows = {
        (p, t, s): (n, d)
        for p, t, s, n, d in zip(got["pid"], got["tid"], got["session"], got["n"], got["d"])
    }
    assert rows == _reference(events, gap, span)
    assert len(rows) > 30
