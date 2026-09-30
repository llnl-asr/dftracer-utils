"""duql correlated sub-queries against a per-row reference."""

import gzip
import json
import random

import pytest

import dftracer.utils as dftu

pytest.importorskip("dftracer.utils.dftracer_utils_ext")


def _events(seed, n):
    rng = random.Random(seed)
    return [
        {
            "ph": "X",
            "name": rng.choice(["read", "write", "open", "close"]),
            "cat": "POSIX",
            "pid": rng.randint(1, 4),
            "tid": i,
            "ts": rng.randrange(0, 100_000),
            "dur": rng.randrange(0, 500),
        }
        for i in range(n)
    ]


def _viewer(tmp_path, events):
    path = tmp_path / "t.pfw.gz"
    path.write_bytes(gzip.compress(("\n".join(json.dumps(e) for e in events) + "\n").encode()))
    return dftu.TraceViewer(str(path))


def _by_tid(got, column):
    return dict(zip(got["tid"], got[column]))


@pytest.mark.parametrize("seed", [1, 2, 3])
def test_above_the_mean_of_its_own_name(tmp_path, seed):
    events = _events(seed, 2000)
    mean = {}
    for name in {e["name"] for e in events}:
        durs = [e["dur"] for e in events if e["name"] == name]
        mean[name] = sum(durs) / len(durs)
    got = (
        _viewer(tmp_path, events)
        .duql("where dur > (from data | where name == ^.name | agg { m = mean(dur) }) | select tid")
        .collect()
        .to_dict()
    )
    want = {e["tid"] for e in events if e["dur"] > mean[e["name"]]}
    assert set(got["tid"]) == want and len(got["tid"]) == len(want)
    assert 0 < len(want) < len(events)


@pytest.mark.parametrize("seed", [1, 2])
def test_count_per_key_and_per_pair(tmp_path, seed):
    events = _events(seed, 1500)
    got = (
        _viewer(tmp_path, events)
        .duql(
            "derive n = (from data | where pid == ^.pid | agg { c = count() }), "
            "s = (from data | where pid == ^.pid and name == ^.name | agg { t = sum(dur) }) "
            "| select tid, n, s"
        )
        .collect()
        .to_dict()
    )
    n = _by_tid(got, "n")
    s = _by_tid(got, "s")
    assert len(n) == len(events)
    for e in events:
        assert n[e["tid"]] == sum(o["pid"] == e["pid"] for o in events)
        want = sum(o["dur"] for o in events if o["pid"] == e["pid"] and o["name"] == e["name"])
        assert s[e["tid"]] == pytest.approx(want)


@pytest.mark.parametrize("negated", [False, True])
def test_semi_join_per_key(tmp_path, negated):
    events = _events(7, 1500)
    op = "not in" if negated else "in"
    got = (
        _viewer(tmp_path, events)
        .duql(
            f"where name {op} (from data | where pid == ^.pid and dur > 495 | select name) | select tid"
        )
        .collect()
        .to_dict()
    )

    def hit(e):
        return any(
            o["pid"] == e["pid"] and o["dur"] > 495 and o["name"] == e["name"] for o in events
        )

    want = {e["tid"] for e in events if hit(e) != negated}
    assert set(got["tid"]) == want and len(got["tid"]) == len(want)
    assert 0 < len(want) < len(events)


def test_a_key_with_several_rows_is_an_error(tmp_path):
    events = _events(9, 200)
    with pytest.raises(Exception, match="rows for key"):
        _viewer(tmp_path, events).duql(
            "derive x = (from data | where pid == ^.pid | select dur)"
        ).collect()


def test_a_term_that_mixes_rows_is_an_error(tmp_path):
    with pytest.raises(Exception, match=r"ts \+ \^\.dur < ts2"):
        _viewer(tmp_path, _events(9, 20)).duql(
            "where dur > (from data | where ts + ^.dur < ts2 | agg { m = max(dur) })"
        ).collect()


@pytest.mark.parametrize("where", ["tid >= 0", "(tid >= 0) or false"])
def test_longest_earlier_call(tmp_path, where):
    events = _events(4, 600)
    got = _by_tid(
        _viewer(tmp_path, events)
        .duql(
            f"where {where} | derive m = (from data | where ts < ^.ts | agg {{ m = max(dur) }}) "
            "| select tid, m"
        )
        .collect()
        .to_dict(),
        "m",
    )
    assert len(got) == len(events)
    for e in events:
        earlier = [o["dur"] for o in events if o["ts"] < e["ts"]]
        assert got[e["tid"]] == (max(earlier) if earlier else None)


@pytest.mark.parametrize("where", ["tid >= 0", "(tid >= 0) or false"])
def test_band_per_pid(tmp_path, where):
    events = _events(5, 600)
    got = (
        _viewer(tmp_path, events)
        .duql(
            f"where {where} | derive "
            "n = (from data | where pid == ^.pid and ts between ^.ts - 5000 and ^.ts "
            "| agg { n = count() }), "
            "s = (from data | where pid == ^.pid and ts between ^.ts - 5000 and ^.ts "
            "| agg { s = sum(dur) }), "
            "a = (from data | where pid == ^.pid and ts > ^.ts | agg { a = mean(dur) }) "
            "| select tid, n, s, a"
        )
        .collect()
        .to_dict()
    )
    n, total, mean = _by_tid(got, "n"), _by_tid(got, "s"), _by_tid(got, "a")
    for e in events:
        band = [
            o["dur"]
            for o in events
            if o["pid"] == e["pid"] and e["ts"] - 5000 <= o["ts"] <= e["ts"]
        ]
        later = [o["dur"] for o in events if o["pid"] == e["pid"] and o["ts"] > e["ts"]]
        assert n[e["tid"]] == len(band)
        assert total[e["tid"]] == sum(band)
        assert mean[e["tid"]] == (pytest.approx(sum(later) / len(later)) if later else None)


@pytest.mark.parametrize("negated", [False, True])
def test_an_earlier_row_of_the_same_name(tmp_path, negated):
    events = _events(6, 800)
    op = "not in" if negated else "in"
    got = (
        _viewer(tmp_path, events)
        .duql(
            f'where name == "read" and pid {op} (from data | where name == "write" '
            "and ts < ^.ts | select pid) | select tid"
        )
        .collect()
        .to_dict()
    )

    def hit(e):
        return any(
            o["name"] == "write" and o["ts"] < e["ts"] and o["pid"] == e["pid"] for o in events
        )

    want = {e["tid"] for e in events if e["name"] == "read" and hit(e) != negated}
    assert set(got["tid"]) == want and len(got["tid"]) == len(want)
    assert 0 < len(want)
