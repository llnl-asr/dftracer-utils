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


@pytest.fixture
def members():
    return [_member(i) for i in range(MEMBERS)]


def test_whole_file_matches_traceviewer(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(b"".join(members))
    live = LiveTraceViewer(str(p))
    assert _count(live) == _count(TraceViewer(str(p))) == MEMBERS * ROWS
    assert live.skipped_tail_bytes == 0 and live.holes == []
    assert live.verified_bytes == p.stat().st_size


def test_unfinished_tail_is_ignored(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    whole = b"".join(members[:5])
    p.write_bytes(whole + members[5][: len(members[5]) // 2])
    live = LiveTraceViewer(str(p))
    assert _count(live) == 5 * ROWS
    assert live.verified_bytes == len(whole)
    assert live.skipped_tail_bytes == len(members[5]) // 2
    assert live.holes == []


def test_hole_stops_the_read_and_a_filled_hole_resumes(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    before = b"".join(members[:3])
    hole = len(members[3])
    p.write_bytes(before + b"\0" * hole + b"".join(members[4:6]))
    live = LiveTraceViewer(str(p))
    assert _count(live) == 3 * ROWS
    assert live.holes == [(str(p), len(before), hole)]
    # same size, new mtime: the hole fills in
    p.write_bytes(b"".join(members[:6]))
    live.refresh()
    assert live.holes == [] and _count(live) == 6 * ROWS
    assert _count(LiveTraceViewer(str(p))) == 6 * ROWS


def test_growth_between_reads(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(members[0] + members[1][:10])
    first = LiveTraceViewer(str(p))
    assert _count(first) == ROWS
    with open(p, "wb") as f:
        f.write(b"".join(members[:4]))
    assert _count(LiveTraceViewer(str(p))) == 4 * ROWS
    with open(p, "ab") as f:
        f.write(b"".join(members[4:]))
    assert _count(first.refresh()) == MEMBERS * ROWS


def test_inherits_traceviewer_ops(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(b"".join(members[:3]) + members[3][:7])
    live = LiveTraceViewer([str(p)])
    assert isinstance(live, TraceViewer)
    assert _count(live.filter("name == 'read'")) == 3 * ROWS


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


def test_two_files_list_either_order_and_directory(tmp_path):
    a = tmp_path / "trace_host.pfw.gz"
    s = tmp_path / "trace-abc-spill.pfw.gz"
    a.write_bytes(_named(1, "tool.a"))
    s.write_bytes(_named(2, "tool.s"))
    both = ["tool.a", "tool.s"]
    assert _names(LiveTraceViewer([str(a), str(s)])) == both
    assert _names(LiveTraceViewer([str(s), str(a)])) == both
    assert _names(LiveTraceViewer(str(tmp_path))) == both


def test_one_file_grows_while_other_is_finished(tmp_path, members):
    a = tmp_path / "a.pfw.gz"
    b = tmp_path / "b.pfw.gz"
    b.write_bytes(_member(0, 10000) + _member(1, 10000))
    a.write_bytes(members[0] + members[1][:9])
    live = LiveTraceViewer(str(tmp_path))
    assert _count(live) == 3 * ROWS
    a.write_bytes(b"".join(members[:4]))
    assert _count(live.refresh()) == 6 * ROWS


def test_hole_in_one_file_keeps_the_other(tmp_path, members):
    a = tmp_path / "a.pfw.gz"
    b = tmp_path / "b.pfw.gz"
    b.write_bytes(_member(0, 10000) + _member(1, 10000))
    a.write_bytes(members[0] + b"\0" * len(members[1]) + members[2])
    live = LiveTraceViewer([str(a), str(b)])
    assert _count(live) == 3 * ROWS
    assert [h[0] for h in live.holes] == [str(a)]


def test_refresh_picks_up_new_file_in_directory(tmp_path):
    (tmp_path / "trace_host.pfw.gz").write_bytes(_named(1, "tool.a"))
    live = LiveTraceViewer(str(tmp_path))
    assert _names(live) == ["tool.a"]
    (tmp_path / "trace-x-spill.pfw.gz").write_bytes(_named(2, "tool.s"))
    assert _names(live.refresh()) == ["tool.a", "tool.s"]


def test_recurses_into_subdirectories(tmp_path):
    sub = tmp_path / "rank0"
    sub.mkdir()
    (sub / "trace.pfw.gz").write_bytes(_named(1, "tool.a"))
    assert _names(LiveTraceViewer(str(tmp_path))) == ["tool.a"]


def test_replaced_file_drops_the_old_rows(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(b"".join(members[:3]))
    live = LiveTraceViewer(str(p))
    assert _count(live) == 3 * ROWS
    other = _member(0, 10000)
    p.write_bytes(other[: len(other) // 2])
    assert _count(live.refresh()) == 0
    assert live.verified_bytes == 0


def test_nothing_whole_yet_reads_empty_then_grows(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(members[0][:10])
    live = LiveTraceViewer(str(p))
    assert _count(live) == 0 and live.skipped_tail_bytes == 10
    p.write_bytes(members[0])
    assert _count(live.refresh()) == ROWS


def test_reused_snapshot_dir_reads_only_the_named_files(tmp_path, members):
    snaps = tmp_path / "snaps"
    a = tmp_path / "a.pfw.gz"
    b = tmp_path / "b.pfw.gz"
    a.write_bytes(members[0])
    b.write_bytes(_member(0, 10000))
    assert _count(LiveTraceViewer([str(a), str(b)], snapshot_dir=str(snaps))) == 2 * ROWS
    assert _count(LiveTraceViewer([str(b)], snapshot_dir=str(snaps))) == ROWS


def test_snapshot_dir_inside_the_traces_is_not_read_back(tmp_path, members):
    (tmp_path / "a.pfw.gz").write_bytes(members[0])
    snaps = tmp_path / "snaps"
    live = LiveTraceViewer(str(tmp_path), snapshot_dir=str(snaps))
    assert _count(live) == ROWS
    assert _count(live.refresh()) == ROWS


def test_missing_named_file_raises(tmp_path):
    with pytest.raises(Exception, match="no such trace file"):
        LiveTraceViewer(str(tmp_path / "absent.pfw.gz"))


def test_record_schema_survives_refresh_and_an_empty_start(tmp_path, members):
    p = tmp_path / "a.pfw.gz"
    p.write_bytes(members[0][:10])
    live = LiveTraceViewer(str(p), record_schema="dftracer")
    assert _count(live) == 0
    p.write_bytes(members[0])
    assert _count(live.refresh()) == ROWS
