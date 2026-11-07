# SPDX-License-Identifier: MIT
"""LiveTraceViewer: reading a gzip trace that is still being written."""

import gzip
import json

import pytest

from dftracer.utils import LiveTraceViewer, TraceViewer

MEMBERS = 8
ROWS = 20


def _member(i, base=0):
    lines = []
    for j in range(ROWS):
        n = base + i * ROWS + j
        lines.append(
            json.dumps(
                {
                    "id": n,
                    "name": "read",
                    "cat": "POSIX",
                    "pid": 1,
                    "tid": 1,
                    "ts": 1000 + n * 10,
                    "dur": 5,
                    "ph": "X",
                    "args": {},
                }
            )
        )
    return gzip.compress(("\n".join(lines) + "\n").encode())


def _count(viewer):
    return sum(len(b) for b in viewer.stream(batch_size=64))


def _agg_total(viewer):
    table = viewer.group_by("cat").agg("count").collect().to_dict()
    (column,) = [name for name in table if name != "cat"]
    return sum(table[column])


@pytest.fixture
def members():
    return [_member(i) for i in range(MEMBERS)]


def test_whole_file_matches_traceviewer(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(b"".join(members))
    live = LiveTraceViewer(str(p))
    assert _count(live.snapshot()) == _count(TraceViewer(str(p))) == MEMBERS * ROWS
    (status,) = live.status()
    assert status.path == str(p) and status.state == "whole"
    assert status.verified_bytes == p.stat().st_size
    assert status.skipped_bytes == 0 and status.hole_bytes == 0


def test_unfinished_tail_is_ignored(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    whole = b"".join(members[:5])
    p.write_bytes(whole + members[5][: len(members[5]) // 2])
    live = LiveTraceViewer(str(p))
    assert _count(live.snapshot()) == 5 * ROWS
    (status,) = live.status()
    assert status.state == "tail"
    assert status.verified_bytes == len(whole)
    assert status.skipped_bytes == len(members[5]) // 2
    assert status.hole_bytes == 0


def test_hole_stops_the_read_and_a_filled_hole_resumes(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    before = b"".join(members[:3])
    hole = len(members[3])
    p.write_bytes(before + b"\0" * hole + b"".join(members[4:6]))
    live = LiveTraceViewer(str(p))
    assert _count(live.snapshot()) == 3 * ROWS
    (status,) = live.status()
    assert status.state == "hole"
    assert status.verified_bytes == len(before)
    assert status.hole_bytes == hole
    # same size, new mtime: the hole fills in
    p.write_bytes(b"".join(members[:6]))
    assert _count(live.snapshot()) == 6 * ROWS
    assert live.status()[0].state == "whole"
    assert _count(LiveTraceViewer(str(p)).snapshot()) == 6 * ROWS


def test_growth_between_reads(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(members[0] + members[1][:10])
    first = LiveTraceViewer(str(p))
    assert _count(first.snapshot()) == ROWS
    with open(p, "wb") as f:
        f.write(b"".join(members[:4]))
    assert _count(LiveTraceViewer(str(p)).snapshot()) == 4 * ROWS
    with open(p, "ab") as f:
        f.write(b"".join(members[4:]))
    assert _count(first.snapshot()) == MEMBERS * ROWS


def test_old_snapshot_keeps_its_rows_after_the_file_grows(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(b"".join(members[:3]))
    live = LiveTraceViewer(str(p))
    old = live.snapshot()
    assert _count(old) == 3 * ROWS
    with open(p, "ab") as f:
        f.write(b"".join(members[3:]))
    new = live.snapshot()
    assert _count(old) == 3 * ROWS
    assert _count(new) == MEMBERS * ROWS


def test_aggregation_on_an_old_snapshot_gives_the_old_count(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(b"".join(members[:3]))
    live = LiveTraceViewer(str(p))
    old = live.snapshot()
    with open(p, "ab") as f:
        f.write(b"".join(members[3:]))
    new = live.snapshot()
    assert _agg_total(old) == 3 * ROWS
    assert _agg_total(new) == MEMBERS * ROWS


def test_live_viewer_holds_no_query_ops(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(b"".join(members[:3]))
    live = LiveTraceViewer([str(p)])
    assert not hasattr(live, "group_by")
    assert not isinstance(live, TraceViewer)
    assert _count(live.snapshot().filter("name == 'read'")) == 3 * ROWS


def test_traceviewer_unchanged_on_finished_file(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(b"".join(members))
    assert _count(TraceViewer(str(p))) == MEMBERS * ROWS


def _named(i, name):
    row = {
        "id": i,
        "name": name,
        "cat": "POSIX",
        "pid": 1,
        "tid": 1,
        "ts": 1000 + i,
        "dur": 5,
        "ph": "X",
        "args": {},
    }
    return gzip.compress((json.dumps(row) + "\n").encode())


def _names(viewer):
    out = []
    for df in viewer.stream(batch_size=64):
        out.extend(df["name"].to_list() if hasattr(df["name"], "to_list") else list(df["name"]))
    return sorted(out)


def _named(i, name):
    row = {
        "id": i,
        "name": name,
        "cat": "POSIX",
        "pid": 1,
        "tid": 1,
        "ts": 1000 + i,
        "dur": 5,
        "ph": "X",
        "args": {},
    }
    return gzip.compress((json.dumps(row) + "\n").encode())


def _names(viewer):
    out = []
    for df in viewer.stream(batch_size=64):
        out.extend(df["name"].to_list() if hasattr(df["name"], "to_list") else list(df["name"]))
    return sorted(out)


def test_two_files_list_either_order_and_directory(tmp_path):
    a = tmp_path / "trace_host.pfw.gz"
    s = tmp_path / "trace-abc-spill.pfw.gz"
    a.write_bytes(_named(1, "tool.a"))
    s.write_bytes(_named(2, "tool.s"))
    both = ["tool.a", "tool.s"]
    assert _names(LiveTraceViewer([str(a), str(s)]).snapshot()) == both
    assert _names(LiveTraceViewer([str(s), str(a)]).snapshot()) == both
    assert _names(LiveTraceViewer(str(tmp_path)).snapshot()) == both


def test_one_file_grows_while_other_is_finished(tmp_path, members):
    a = tmp_path / "a.pfw.gz"
    b = tmp_path / "b.pfw.gz"
    b.write_bytes(_member(0, 10000) + _member(1, 10000))
    a.write_bytes(members[0] + members[1][:9])
    live = LiveTraceViewer(str(tmp_path))
    assert _count(live.snapshot()) == 3 * ROWS
    a.write_bytes(b"".join(members[:4]))
    assert _count(live.snapshot()) == 6 * ROWS


def test_hole_in_one_file_keeps_the_other(tmp_path, members):
    a = tmp_path / "a.pfw.gz"
    b = tmp_path / "b.pfw.gz"
    b.write_bytes(_member(0, 10000) + _member(1, 10000))
    a.write_bytes(members[0] + b"\0" * len(members[1]) + members[2])
    live = LiveTraceViewer([str(a), str(b)])
    assert _count(live.snapshot()) == 3 * ROWS
    states = {st.path: st.state for st in live.status()}
    assert states == {str(a): "hole", str(b): "whole"}


def test_snapshot_picks_up_new_file_in_directory(tmp_path):
    (tmp_path / "trace_host.pfw.gz").write_bytes(_named(1, "tool.a"))
    live = LiveTraceViewer(str(tmp_path))
    assert _names(live.snapshot()) == ["tool.a"]
    (tmp_path / "trace-x-spill.pfw.gz").write_bytes(_named(2, "tool.s"))
    assert _names(live.snapshot()) == ["tool.a", "tool.s"]


def test_recurses_into_subdirectories(tmp_path):
    sub = tmp_path / "rank0"
    sub.mkdir()
    (sub / "trace.pfw.gz").write_bytes(_named(1, "tool.a"))
    assert _names(LiveTraceViewer(str(tmp_path)).snapshot()) == ["tool.a"]


def test_replaced_file_drops_the_old_rows(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(b"".join(members[:3]))
    live = LiveTraceViewer(str(p))
    assert _count(live.snapshot()) == 3 * ROWS
    other = _member(0, 10000)
    p.write_bytes(other[: len(other) // 2])
    assert _count(live.snapshot()) == 0
    assert live.status()[0].verified_bytes == 0


def test_nothing_whole_yet_reads_empty_then_grows(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(members[0][:10])
    live = LiveTraceViewer(str(p))
    assert _count(live.snapshot()) == 0
    assert live.status()[0].skipped_bytes == 10
    p.write_bytes(members[0])
    assert _count(live.snapshot()) == ROWS


def test_missing_named_file_raises_at_snapshot(tmp_path):
    live = LiveTraceViewer(str(tmp_path / "absent.pfw.gz"))
    with pytest.raises(Exception, match="cannot stat"):
        live.snapshot()


def test_record_schema_survives_snapshots_and_an_empty_start(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(members[0][:10])
    live = LiveTraceViewer(str(p), record_schema="dftracer")
    assert _count(live.snapshot()) == 0
    p.write_bytes(members[0])
    assert _count(live.snapshot()) == ROWS


def test_new_rows_returns_each_row_once(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(members[0])
    live = LiveTraceViewer(str(p))
    first = live.new_rows()
    assert first is not None and _count(first) == ROWS
    assert live.new_rows() is None

    p.write_bytes(b"".join(members[:3]))
    second = live.new_rows()
    assert second is not None
    assert _count(second) == 2 * ROWS and _agg_total(second) == 2 * ROWS
    assert live.new_rows() is None
    assert _count(live.snapshot()) == 3 * ROWS
    assert _count(first) == ROWS


def test_follow_yields_only_new_rows(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(members[0])
    following = LiveTraceViewer(str(p)).follow(interval=0.01)
    assert _count(next(following)) == ROWS
    p.write_bytes(b"".join(members[:2]))
    assert _count(next(following)) == ROWS


def test_follow_refuses_an_interval_that_never_waits(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(members[0])
    with pytest.raises(ValueError):
        next(LiveTraceViewer(str(p)).follow(interval=0))
