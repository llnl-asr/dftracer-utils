#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""sink_trace, sink_table, write_trace and write_table: staging and commit,
the nrows slot, parts, events output, the index and crash behavior."""

import gzip
import json
import os
import shutil
import signal
import subprocess
import sys
import textwrap
import time
import zlib

import pytest

pa = pytest.importorskip("pyarrow")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import DataFrame, SinkSummary, TraceWriteOptions, col  # noqa: E402
from dftracer.utils import dftracer_utils_ext as ext  # noqa: E402
from dftracer.utils.index import Indexer  # noqa: E402
from dftracer.utils.trace import TraceViewer  # noqa: E402


def frame(n=10):
    return DataFrame.from_dict(
        {
            "i": list(range(n)),
            "f": [i / 4 for i in range(n)],
            "s": [f"k{i % 3}" for i in range(n)],
        }
    )


def lines(path):
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as f:
        return f.read().splitlines()


def zlib_multi_member(path):
    """Inflate every gzip member with zlib alone."""
    data = open(path, "rb").read()
    out = b""
    while data:
        d = zlib.decompressobj(wbits=31)
        out += d.decompress(data)
        data = d.unused_data
        assert d.eof
    return out.decode()


def header_of(path):
    return json.loads(lines(path)[0])["args"]


def test_a_streamed_sink_patches_the_row_count_and_every_reader_reads_it(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    summary = frame(25).lazy().filter(col("i") >= 5).sink_table(path)
    assert summary == SinkSummary(root=path, rows=20, kind="records", part_count=1)
    assert header_of(path)["nrows"] == 20
    assert header_of(path)["format_version"] == ext.TABLE_FORMAT_VERSION
    text = zlib_multi_member(path)
    assert text.splitlines() == lines(path)
    cli = subprocess.run(["gzip", "-dc", path], capture_output=True, text=True, check=True)
    assert cli.stdout.splitlines() == lines(path)
    assert TraceViewer(path).collect().num_rows == 20


def test_a_plain_streamed_sink_patches_the_slot_only(tmp_path):
    path = str(tmp_path / "t.pfw")
    frame(7).lazy().sink_table(path)
    assert header_of(path)["nrows"] == 7
    assert not os.path.exists(tmp_path / ".dftindex")
    assert TraceViewer(path).collect().num_rows == 7


def test_an_eager_write_has_the_row_count_at_once(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    summary = frame(6).write_table(path)
    assert summary.rows == 6 and summary.kind == "records"
    assert header_of(path)["nrows"] == 6
    assert header_of(path)["table"] == "t"


def test_a_header_above_one_stored_block_and_zero_rows(tmp_path):
    big = tmp_path / "big.pfw.gz"
    meta = {"blob": "x" * 150_000}
    frame(3).write_table(str(big), meta=meta)
    assert header_of(str(big))["meta"] == meta
    assert zlib_multi_member(str(big)).splitlines() == lines(str(big))
    assert TraceViewer(str(big)).collect().num_rows == 3
    # The same header streamed: the slot and the CRC32 patch still hold.
    streamed = tmp_path / "streamed.pfw.gz"
    frame(4).lazy().sink_table(str(streamed), meta=meta)
    assert header_of(str(streamed))["nrows"] == 4
    assert zlib_multi_member(str(streamed)).splitlines() == lines(str(streamed))

    empty = tmp_path / "empty.pfw.gz"
    summary = frame(5).lazy().filter(col("i") > 99).sink_table(str(empty))
    assert summary.rows == 0
    assert header_of(str(empty))["nrows"] == 0
    assert len(lines(str(empty))) == 1
    assert TraceViewer(str(empty)).collect().num_rows == 0
    plain_empty = tmp_path / "empty.pfw"
    frame(5).write_table(str(plain_empty), options=TraceWriteOptions(build_index=False))
    assert len(lines(str(plain_empty))) == 1 + 5


def test_a_reader_refuses_a_slot_that_was_never_patched(tmp_path):
    path = tmp_path / "t.pfw"
    frame(3).write_table(str(path))
    text = path.read_text()
    patched = text.replace('"nrows":3 ', '"nrows":-1', 1)
    assert patched != text
    path.write_text(patched)
    with pytest.raises(dft.errors.DFTUtilsParseError, match="-1"):
        TraceViewer(str(path)).collect()


def test_mode_error_refuses_and_overwrite_replaces(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    frame(3).write_table(path)
    with pytest.raises(FileExistsError):
        frame(4).write_table(path)
    assert TraceViewer(path).collect().num_rows == 3
    frame(4).write_table(path, mode="overwrite")
    assert TraceViewer(path).collect().num_rows == 4
    assert sorted(os.listdir(tmp_path)) == [".dftindex", "t.pfw.gz"]

    folder = str(tmp_path / "parts")
    frame(5).write_table(folder, part_size=2)
    with pytest.raises(FileExistsError):
        frame(5).write_table(folder, part_size=2)
    frame(3).write_table(folder, part_size=2, mode="overwrite")
    assert TraceViewer(folder).collect().num_rows == 3
    assert not any(n.startswith(".parts.") for n in os.listdir(tmp_path))


def _dead_pid():
    child = subprocess.Popen([sys.executable, "-c", "pass"])
    child.wait()
    return child.pid


def test_the_next_write_removes_leftover_staging_siblings(tmp_path):
    gone = _dead_pid()
    for name in (f".t.pfw.gz.tmp-{gone}-1", f".t.pfw.gz.old-{gone}-2"):
        os.mkdir(tmp_path / name)
        (tmp_path / name / "x").write_text("x")
    keep = tmp_path / ".other.pfw.gz.tmp-9"
    keep.write_text("keep")
    frame(2).write_table(str(tmp_path / "t.pfw.gz"))
    assert sorted(n for n in os.listdir(tmp_path) if n.startswith(".")) == [
        ".dftindex",
        ".other.pfw.gz.tmp-9",
    ]


def test_part_size_makes_numbered_parts_read_in_order(tmp_path):
    folder = str(tmp_path / "out")
    summary = frame(23).write_table(folder, part_size=2)
    assert summary.part_count == 12 and summary.rows == 23
    parts = sorted(n for n in os.listdir(folder) if n.startswith("part-"))
    assert parts[0] == "part-00000.pfw.gz" and parts[-1] == "part-00011.pfw.gz"
    assert len(parts) == 12
    assert [header_of(os.path.join(folder, p))["nrows"] for p in parts] == [2] * 11 + [1]
    assert TraceViewer(folder).collect()["i"].to_list() == list(range(23))


def test_a_folder_of_plain_parts_is_refused(tmp_path):
    folder = str(tmp_path / "plain")
    for options in ({"compress": False}, TraceWriteOptions(compress=False)):
        with pytest.raises(ValueError, match="write a single .pfw file"):
            frame(4).write_table(folder, part_size=2, options=options)
        with pytest.raises(ValueError, match="write a single .pfw file"):
            frame(4).lazy().sink_table(folder, part_size=2, options=options)
    assert os.listdir(tmp_path) == []


def test_a_plain_file_in_a_folder_is_not_read_and_not_indexed(tmp_path):
    folder = tmp_path / "mixed"
    frame(3).write_table(str(folder / "good.pfw.gz"))
    frame(2).write_table(str(folder / "raw.pfw"))
    assert TraceViewer(str(folder)).collect().num_rows == 3
    assert Indexer(str(folder), index_dir=str(tmp_path / "idx")).status().total_files == 1
    with pytest.raises(dft.errors.DFTUtilsIOError, match="not gzip"):
        Indexer(str(folder / "raw.pfw")).build()


def test_arguments_are_checked(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    f = frame(3)
    with pytest.raises(ValueError, match="part_size needs a folder"):
        f.write_table(path, part_size=2)
    with pytest.raises(ValueError, match="unknown sink option"):
        f.write_table(path, options={"nope": 1})
    with pytest.raises(ValueError, match="contradicts"):
        f.write_table(path, options=TraceWriteOptions(compress=False))
    with pytest.raises(ValueError, match="contradicts"):
        f.write_table(str(tmp_path / "p.pfw"), options=TraceWriteOptions(compress=True))
    with pytest.raises(ValueError, match="partition_by needs a folder"):
        f.write_table(path, partition_by=["s"])
    with pytest.raises(ValueError, match="not columns of the plan"):
        f.write_table(str(tmp_path / "dir"), partition_by=["nope"])
    with pytest.raises(ValueError, match="at least one column"):
        f.write_table(str(tmp_path / "dir"), partition_by=[])
    with pytest.raises(ValueError, match="does not take partition_by"):
        f.write(str(tmp_path / "dir"), format="csv", partition_by=["s"])
    with pytest.raises(ValueError, match="event envelope"):
        f.write_trace(path)
    with pytest.raises(ValueError, match="mode"):
        f.write_table(path, mode="append")
    with pytest.raises(ValueError, match="cannot be indexed"):
        f.write_table(str(tmp_path / "q.pfw"), options=TraceWriteOptions(build_index=True))
    assert os.listdir(tmp_path) == []


def test_name_and_meta_go_into_the_header(tmp_path):
    path = str(tmp_path / "stem.pfw.gz")
    frame(2).write_table(path)
    assert header_of(path)["table"] == "stem"
    frame(2).write_table(path, name="custom", meta={"run": [1, 2]}, mode="overwrite")
    args = header_of(path)
    assert args["table"] == "custom" and args["meta"] == {"run": [1, 2]}
    sunk = str(tmp_path / "lazy.pfw.gz")
    frame(2).lazy().sink_table(sunk, name="n", meta={"k": 1})
    assert header_of(sunk)["table"] == "n" and header_of(sunk)["meta"] == {"k": 1}


def test_a_lazy_sink_runs_in_collect_all_and_commits_on_its_own(tmp_path):
    a, b = str(tmp_path / "a.pfw.gz"), str(tmp_path / "b.pfw.gz")
    pending = [frame(2).lazy().sink_table(a, lazy=True), frame(3).lazy().sink_table(b, lazy=True)]
    assert not os.path.exists(a)
    first, second = dft.collect_all(pending)
    assert (first.rows, second.rows) == (2, 3)
    assert os.path.exists(a) and os.path.exists(b)


def _trace(path, n=40):
    with gzip.open(path, "wt") as f:
        for i in range(n):
            f.write(
                json.dumps(
                    {
                        "ph": 1,
                        "name": "read" if i % 2 else "write",
                        "cat": "POSIX",
                        "pid": 1,
                        "tid": 2,
                        "ts": 1000 + i,
                        "dur": 5 + i,
                        "args": {"size": i * 10, "fname": f"/f{i}"},
                    }
                )
                + "\n"
            )


def test_events_output_re_reads_as_a_trace_with_ph_ts_and_dur(tmp_path):
    src = str(tmp_path / "in.pfw.gz")
    _trace(src)
    out = str(tmp_path / "events.pfw.gz")
    summary = TraceViewer(src).filter('name == "read"').sink_trace(out)
    assert summary.kind == "events" and summary.rows == 20
    records = [json.loads(line) for line in lines(out)]
    assert all(r["ph"] == 1 for r in records)
    assert records[0]["ts"] == 1001 and records[0]["dur"] == 6
    assert records[0]["args"] == {"fname": "/f1", "size": 10}
    back = TraceViewer(out).collect()
    assert back.num_rows == 20
    assert back["dur"].to_list() == [6 + 2 * k for k in range(20)]
    # The same plan as records, forced.
    forced = str(tmp_path / "records.pfw.gz")
    assert TraceViewer(src).sink_table(forced).kind == "records"
    assert header_of(forced)["nrows"] == 40


def test_a_viewer_sink_trace_writes_each_event_line_as_the_trace_holds_it(tmp_path):
    src = tmp_path / "odd.pfw.gz"
    want = [
        json.dumps(
            {
                "name": "read",
                "cat": "POSIX",
                "pid": 1,
                "tid": 1,
                "ts": 1000 + i,
                "dur": 5,
                "ph": "X",
                "host": f"n{i}",
                "args": {"deep": {"k": [i, "x"]}, "big": 123456789012345678901234567890},
            },
            separators=(",", ":"),
        )
        for i in range(30)
    ]
    want[3] = want[3].replace('"x"', '"\\u00e9"')
    with gzip.open(src, "wt") as fh:
        fh.write("\n".join(want) + "\n")
    out = str(tmp_path / "same.pfw.gz")
    summary = TraceViewer(str(src)).sink_trace(out)
    assert summary.kind == "events" and summary.rows == 30 and summary.part_count == 1
    assert sorted(lines(out)) == sorted(want)
    folder = str(tmp_path / "parts")
    parts = TraceViewer(str(src)).sink_trace(folder, part_size=12)
    assert parts.part_count == 3 and parts.rows == 30
    got = [
        line
        for name in sorted(os.listdir(folder))
        if name.endswith(".pfw.gz")
        for line in lines(os.path.join(folder, name))
    ]
    assert sorted(got) == sorted(want)


def test_the_index_is_built_prunes_and_marks_the_rows_complete(tmp_path):
    n = 60_000
    path = str(tmp_path / "big.pfw.gz")
    f = DataFrame.from_dict({"i": list(range(n)), "s": [f"k{i % 100}" for i in range(n)]})
    f.lazy().sink_table(path, options=TraceWriteOptions(member_size="64KB"))
    assert os.path.isdir(tmp_path / ".dftindex")
    ext._table_read_counters(True)
    got = TraceViewer(path).filter(col("i") == 12345).collect()
    counts = ext._table_read_counters(True)
    assert got["i"].to_list() == [12345]
    assert counts["indexed_files"] == 1
    assert counts["chunks_skipped"] > 0 and counts["chunks_read"] >= 1


def test_without_an_index_the_data_still_reads(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    frame(30).lazy().sink_table(path, options=TraceWriteOptions(build_index=False))
    assert not os.path.exists(tmp_path / ".dftindex")
    assert TraceViewer(path).filter(col("i") > 25).collect().num_rows == 4


def test_a_killed_write_leaves_nothing_at_the_target(tmp_path):
    target = tmp_path / "big.pfw.gz"
    script = tmp_path / "child.py"
    script.write_text(
        textwrap.dedent(
            """
            import sys
            from dftracer.utils import DataFrame
            n = 6_000_000
            f = DataFrame.from_dict({"i": list(range(n)), "s": ["x"] * n})
            f.lazy().sink_table(sys.argv[1])
            """
        )
    )
    child = subprocess.Popen([sys.executable, str(script), str(target)], cwd=str(tmp_path))
    staging = None
    deadline = time.time() + 120
    while time.time() < deadline and child.poll() is None:
        found = [n for n in os.listdir(tmp_path) if n.startswith(".big.pfw.gz.tmp-")]
        if found and os.path.getsize(tmp_path / found[0]) > 0:
            staging = found[0]
            break
        time.sleep(0.01)
    if staging is None:
        child.wait()
        pytest.skip("the write ended before it could be killed")
    child.send_signal(signal.SIGKILL)
    child.wait()
    assert not target.exists()
    frame(2).write_table(str(target))
    assert not (tmp_path / staging).exists()
    assert TraceViewer(str(target)).collect().num_rows == 2
    shutil.rmtree(tmp_path / ".dftindex", ignore_errors=True)


def test_dataframe_write_builds_no_table_header_for_events(tmp_path, monkeypatch):
    import dftracer.utils._table as table

    def refuse(*args, **kwargs):
        raise AssertionError("the table header is built for an events write")

    monkeypatch.setattr(table, "_write_header", refuse)
    events = DataFrame.from_dict(
        {
            "name": ["read", "write"],
            "cat": ["io", "io"],
            "pid": [1, 1],
            "tid": [1, 2],
            "ts": [10, 20],
            "ph": [1, 1],
        }
    )
    out = str(tmp_path / "events.pfw.gz")
    assert events.write_trace(out).kind == "events"
    assert TraceViewer(out).collect().num_rows == 2


def _carried_source(tmp_path):
    pd = pytest.importorskip("pandas")
    np = pytest.importorskip("numpy")
    df = pd.DataFrame(
        {
            "i": np.arange(8, dtype="int64"),
            "c": pd.Categorical(["a", "b"] * 4, categories=["a", "b", "z"]),
            "f": np.arange(8, dtype="float64"),
            "s": ["x", "y"] * 4,
        }
    )
    path = str(tmp_path / "src.pfw.gz")
    DataFrame.from_pandas(df).write_table(path)
    return path


def _fields(path):
    return {f["name"]: f for f in header_of(path)["fields"]}


def test_a_sink_keeps_the_field_details_of_the_columns_it_keeps(tmp_path):
    src = _carried_source(tmp_path)
    selected = str(tmp_path / "select.pfw.gz")
    TraceViewer(src).select("f", "c", "i").sink_table(selected)
    fields = header_of(selected)["fields"]
    assert [f["name"] for f in fields] == ["f", "c", "i"]
    assert fields[0]["dtype"] == "float64" and fields[2]["dtype"] == "int64"
    assert fields[1]["dtype"] == "category" and fields[1]["categories"] == ["a", "b", "z"]

    dropped = str(tmp_path / "drop.pfw.gz")
    TraceViewer(src).drop("c").sink_table(dropped)
    assert [(f["name"], f["dtype"]) for f in header_of(dropped)["fields"]] == [
        ("i", "int64"),
        ("f", "float64"),
        ("s", header_of(src)["fields"][3]["dtype"]),
    ]
    assert TraceViewer(dropped).collect()["i"].to_list() == list(range(8))


def test_a_sink_drops_the_details_of_a_column_whose_type_changed(tmp_path):
    from dftracer.utils._sink import _carried_header

    carried = header_of(_carried_source(tmp_path))
    schema = {
        "i": dft.DType.FLOAT64,
        "c": dft.DType.STRING,
        "f": dft.DType.FLOAT64,
        "s": dft.DType.INT64,
    }
    made = {f["name"]: f for f in _carried_header(carried, schema, "t", None, None)["fields"]}
    assert made["i"]["dtype"] == "Float64"
    assert made["s"]["dtype"] == "Int64" and "elem" not in made["s"]
    assert made["f"]["dtype"] == "float64"
    assert made["c"]["dtype"] == "category" and made["c"]["categories"] == ["a", "b", "z"]


@pytest.mark.parametrize("dtype", ["float32", "Float32", "float[pyarrow]"])
def test_a_float32_field_does_not_carry_a_float64_column(dtype):
    from dftracer.utils._sink import _carried_header

    carried = {"fields": [{"name": "x", "dtype": dtype}]}
    wide = _carried_header(carried, {"x": dft.DType.FLOAT64}, "t", None, None)
    assert wide["fields"][0]["dtype"] == "Float64"
    narrow = _carried_header(carried, {"x": dft.DType.FLOAT32}, "t", None, None)
    assert narrow["fields"][0]["dtype"] == dtype


def test_the_carried_index_stays_only_when_every_index_column_survives(tmp_path):
    pd = pytest.importorskip("pandas")
    df = pd.DataFrame({"k": [3, 1, 2], "v": [1.5, 2.5, 3.5]}).set_index("k")
    src = str(tmp_path / "idx.pfw.gz")
    DataFrame.from_pandas(df).write_table(src)
    assert header_of(src)["index"] == ["k"]
    kept = str(tmp_path / "kept.pfw.gz")
    TraceViewer(src).select("v", "k").sink_table(kept)
    assert header_of(kept)["index"] == ["k"]
    lost = str(tmp_path / "lost.pfw.gz")
    TraceViewer(src).select("v").sink_table(lost)
    assert header_of(lost)["index"] == [] and header_of(lost)["index_names"] == []


@pytest.mark.parametrize("kind", ["set", "list"])
def test_a_sink_over_a_table_with_raw_columns_reads_back_equal(tmp_path, kind):
    pd = pytest.importorskip("pandas")
    from .test_table_io import save_table

    column = {
        "set": pd.Series([{1, 2}, {3}, set(), {4}], dtype=object),
        "list": pd.Series([[1, 2], [3], [], [4, 5]], dtype=object),
    }[kind]
    df = pd.DataFrame({"i": [1, 2, 3, 4], "x": column})
    src = str(tmp_path / "src.pfw.gz")
    save_table(df, src, name="t")
    out = str(tmp_path / "out.pfw.gz")
    TraceViewer(src).sink_table(out)
    want = TraceViewer(src).collect().to_pandas()
    got = TraceViewer(out).collect().to_pandas()
    assert got["x"].tolist() == want["x"].tolist() == df["x"].tolist()
    narrowed = str(tmp_path / "narrow.pfw.gz")
    TraceViewer(src).select("x").sink_table(narrowed)
    assert TraceViewer(narrowed).collect().to_pandas()["x"].tolist() == df["x"].tolist()


_SINK_BUDGET = 32 << 20
_SINK_PEAK_FACTOR = 8

_STREAM_SCRIPT = textwrap.dedent(
    """
    import resource, sys
    from dftracer.utils import DataFrame, col
    from dftracer.utils.trace import TraceViewer
    kind, path, out, budget = sys.argv[1:5]
    plan = TraceViewer(path).lazy().memory_budget(int(budget))
    if kind == "records":
        plan = plan.filter(col("i") >= 0)
    def peak():
        # On Linux ru_maxrss also holds the peak of the process that spawned
        # this one, so read this image's own high-water mark.
        if sys.platform == "darwin":
            return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        with open("/proc/self/status") as status:
            return next(int(l.split()[1]) for l in status if l.startswith("VmHWM:")) * 1024
    DataFrame.from_dict({"i": [1]}).write_table(out + ".warm.pfw.gz")
    before = peak()
    summary = plan.sink_table(out) if kind == "records" else plan.sink_trace(out)
    print(summary.rows, summary.kind, before, peak())
    """
)


def _run_stream(kind, path, out):
    got = subprocess.run(
        [sys.executable, "-I", "-c", _STREAM_SCRIPT, kind, path, out, str(_SINK_BUDGET)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    return int(got[0]), got[1], int(got[2]), int(got[3])


@pytest.mark.skipif(sys.platform == "win32", reason="peak memory is read with resource")
def test_a_records_sink_holds_memory_bounded_by_the_budget(tmp_path):
    from .test_table_viewer import _compressible_table, _row

    rows = 8_000_000
    src = str(tmp_path / "t.pfw.gz")
    _compressible_table(src, rows)
    out = str(tmp_path / "out.pfw.gz")
    count, kind, before, after = _run_stream("records", src, out)
    produced = rows * len(_row(1) + "\n")
    assert count == rows and kind == "records"
    assert produced > 8 * _SINK_BUDGET
    assert after - before < _SINK_PEAK_FACTOR * _SINK_BUDGET
    assert after - before < produced // 2


def _write_events_trace(path, rows, member_lines=50_000):
    import zlib

    line = (
        json.dumps(
            {
                "ph": 1,
                "name": "read",
                "cat": "POSIX",
                "pid": 1,
                "tid": 2,
                "ts": 1000,
                "dur": 5,
                "args": {"size": 10, "fname": "/f1"},
            }
        )
        + "\n"
    )
    with open(path, "wb") as f:
        left = rows
        while left > 0:
            n = min(left, member_lines)
            c = zlib.compressobj(1, zlib.DEFLATED, 31)
            f.write(c.compress((line * n).encode()) + c.flush())
            left -= n
    return len(line)


# A scan unit is decoded through the page heap, which keeps freed pages for a
# while, so a trace that decodes far past the budget needs more than one budget
# of headroom even with one unit in flight.
_EVENTS_PEAK_FACTOR = 10


@pytest.mark.skipif(sys.platform == "win32", reason="peak memory is read with resource")
def test_an_events_sink_holds_memory_bounded_by_the_budget(tmp_path):
    rows = 3_000_000
    src = str(tmp_path / "ev.pfw.gz")
    line_size = _write_events_trace(src, rows)
    out = str(tmp_path / "out.pfw.gz")
    count, kind, before, after = _run_stream("events", src, out)
    produced = rows * line_size
    assert count == rows and kind == "events"
    assert produced > 8 * _SINK_BUDGET
    assert after - before < _EVENTS_PEAK_FACTOR * _SINK_BUDGET


_SLOW_STREAM_SCRIPT = textwrap.dedent(
    """
    import resource, sys, time
    from dftracer.utils.trace import TraceViewer
    path, budget = sys.argv[1:3]
    plan = TraceViewer(path).lazy().memory_budget(int(budget))
    def peak():
        # On Linux ru_maxrss also holds the peak of the process that spawned
        # this one, so read this image's own high-water mark.
        if sys.platform == "darwin":
            return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        with open("/proc/self/status") as status:
            return next(int(l.split()[1]) for l in status if l.startswith("VmHWM:")) * 1024
    before = peak()
    rows = 0
    for frame in plan.stream():
        rows += len(frame)
        time.sleep(0.01)
    print(rows, before, peak())
    """
)


@pytest.mark.skipif(sys.platform == "win32", reason="peak memory is read with resource")
def test_a_stream_held_by_a_slow_consumer_stays_bounded_by_the_budget(tmp_path):
    rows = 3_000_000
    src = str(tmp_path / "ev.pfw.gz")
    line_size = _write_events_trace(src, rows)
    got = subprocess.run(
        [sys.executable, "-I", "-c", _SLOW_STREAM_SCRIPT, src, str(_SINK_BUDGET)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    count, before, after = int(got[0]), int(got[1]), int(got[2])
    assert count == rows
    assert rows * line_size > 8 * _SINK_BUDGET
    assert after - before < _EVENTS_PEAK_FACTOR * _SINK_BUDGET


_COLLECT_SCRIPT = textwrap.dedent(
    """
    import resource, sys
    from dftracer.utils import DataFrame
    from dftracer.utils.trace import TraceViewer
    path, budget = sys.argv[1:3]
    def peak():
        if sys.platform == "darwin":
            return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        with open("/proc/self/status") as status:
            return next(int(l.split()[1]) for l in status if l.startswith("VmHWM:")) * 1024
    DataFrame.from_dict({"i": [1]}).to_pandas()
    before = peak()
    rows = len(TraceViewer(path).lazy().memory_budget(int(budget)).collect())
    print(rows, before, peak())
    """
)

# A unit holds read buffers, a batch of parsed events and row builders however
# small its span, so many small units in flight pass the budget unless each is
# admitted by that fixed cost.
_SMALL_UNIT_PEAK_FACTOR = 4


@pytest.mark.skipif(sys.platform == "win32", reason="peak memory is read with resource")
def test_a_collect_of_small_units_stays_bounded_by_the_budget(tmp_path):
    rows = 3_000_000
    src = str(tmp_path / "ev.pfw.gz")
    line_size = _write_events_trace(src, rows, member_lines=4_000)
    # Index in this process, so the child measures the scan alone.
    TraceViewer(src).lazy().head(1).collect()
    got = subprocess.run(
        [sys.executable, "-I", "-c", _COLLECT_SCRIPT, src, str(_SINK_BUDGET)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    count, before, after = int(got[0]), int(got[1]), int(got[2])
    assert count == rows
    assert 4_000 * line_size < _SINK_BUDGET // 32
    assert after - before < _SMALL_UNIT_PEAK_FACTOR * _SINK_BUDGET


def test_a_plan_with_no_schema_before_it_runs_streams_without_collecting(tmp_path):
    plan = frame(7).lazy().describe()
    assert not plan.schema
    out = str(tmp_path / "d.pfw.gz")
    summary = plan.sink_table(out)
    assert summary.kind == "records" and summary.rows > 0
    want = frame(7).lazy().describe().collect()
    got = TraceViewer(out).collect()
    assert got.columns == want.columns and got.num_rows == want.num_rows
    assert header_of(out)["nrows"] == want.num_rows
    with pytest.raises(ValueError, match="event envelope"):
        plan.sink_trace(str(tmp_path / "e.pfw.gz"))


def test_sink_trace_refuses_a_plan_without_the_envelope_and_names_the_columns(tmp_path):
    out = str(tmp_path / "e.pfw.gz")
    with pytest.raises(
        ValueError, match=r"event envelope.*'name'.*'cat'.*'pid'.*'tid'.*'ts'.*'ph'"
    ):
        frame(3).lazy().sink_trace(out)
    partial = DataFrame.from_dict({"name": ["a"], "cat": ["c"], "pid": [1], "tid": [1]})
    with pytest.raises(ValueError, match=r"'ts', 'ph'"):
        partial.lazy().sink_trace(out)
    with pytest.raises(ValueError, match=r"'ts', 'ph'"):
        partial.write_trace(out)
    assert os.listdir(tmp_path) == []


def test_write_trace_of_a_collected_trace_frame_re_reads_as_a_trace(tmp_path):
    src = str(tmp_path / "in.pfw.gz")
    _trace(src)
    collected = TraceViewer(src).filter('name == "read"').collect()
    out = str(tmp_path / "out.pfw.gz")
    summary = collected.write_trace(out)
    assert summary.kind == "events" and summary.rows == 20
    back = TraceViewer(out).collect()
    assert back.num_rows == 20
    assert back["ph"].to_list() == [1] * 20
    assert back["ts"].to_list() == collected["ts"].to_list()
    assert back["dur"].to_list() == collected["dur"].to_list()


def test_a_trace_verb_has_no_name_and_meta_and_no_verb_takes_rows(tmp_path):
    path = str(tmp_path / "t.pfw.gz")
    for call in (
        lambda: frame(2).write_table(path, rows="records"),
        lambda: frame(2).write_trace(path, rows="events"),
        lambda: frame(2).lazy().sink_table(path, rows="records"),
        lambda: frame(2).lazy().sink_trace(path, rows="events"),
        lambda: frame(2).write_trace(path, name="n"),
        lambda: frame(2).lazy().sink_trace(path, meta={}),
    ):
        with pytest.raises(TypeError, match="unexpected keyword argument"):
            call()
    assert os.listdir(tmp_path) == []


def test_the_generic_sink_and_write_need_a_format(tmp_path):
    path = str(tmp_path / "x.csv")
    with pytest.raises(TypeError, match="format"):
        frame(2).lazy().sink(path)
    with pytest.raises(TypeError, match="format"):
        frame(2).write(path)


HIVE_DEFAULT = "__HIVE_DEFAULT_PARTITION__"


def _walk(root):
    return sorted(
        os.path.relpath(os.path.join(d, f), root) for d, _, fs in os.walk(root) for f in fs
    )


def _parts(root):
    return [p for p in _walk(root) if p.endswith(".pfw.gz") and ".dftindex" not in p]


def test_hive_partitions_round_trip_keys_with_special_bytes_and_nulls(tmp_path):
    keys = ["a/b", "c=d", "e f", "é", "%x", "p:q", HIVE_DEFAULT, None, "a/b", ""]
    f = DataFrame.from_dict({"k": keys, "v": list(range(len(keys)))})
    out = str(tmp_path / "out")
    summary = f.write_table(out, partition_by=["k"])
    assert summary.rows == len(keys) and summary.kind == "records"
    folders = sorted({p.split("/")[0] for p in _parts(out)})
    assert "k=a%2Fb" in folders and "k=c%3Dd" in folders and "k=e%20f" in folders
    assert "k=%C3%A9" in folders and "k=%25x" in folders and "k=p%3Aq" in folders
    assert f"k={HIVE_DEFAULT}" in folders
    assert "k=%5F_HIVE_DEFAULT_PARTITION__" in folders
    assert summary.part_count == len(_parts(out)) == len(set(map(str, keys)))
    back = TraceViewer(out).collect().to_pandas().sort_values("v")
    assert back["v"].tolist() == list(range(len(keys)))
    pd = pytest.importorskip("pandas")
    got = [None if v is None or v is pd.NA or v != v else v for v in back["k"]]
    assert got == keys


def test_a_part_holds_its_key_column_and_reads_alone(tmp_path):
    f = DataFrame.from_dict({"k": ["x", "y", "x"], "v": [1, 2, 3]})
    out = str(tmp_path / "out")
    f.write_table(out, partition_by=["k"])
    one = TraceViewer(os.path.join(out, "k=x", "part-00000.pfw.gz")).collect()
    assert one["k"].to_list() == ["x", "x"] and one["v"].to_list() == [1, 3]
    assert header_of(os.path.join(out, "k=y", "part-00000.pfw.gz"))["nrows"] == 1


def test_two_keys_make_nested_folders_and_part_size_rolls_inside_a_partition(tmp_path):
    n = 10
    f = DataFrame.from_dict({"a": ["p"] * n, "b": [i % 2 for i in range(n)], "v": list(range(n))})
    out = str(tmp_path / "out")
    f.write_table(out, partition_by=["a", "b"], part_size=2)
    assert _parts(out) == sorted(f"a=p/b={b}/part-{i:05d}.pfw.gz" for b in (0, 1) for i in range(3))
    back = TraceViewer(out).collect()
    assert sorted(back["v"].to_list()) == list(range(n))


def test_more_than_max_partitions_raises_at_the_first_excess_key(tmp_path):
    out = str(tmp_path / "out")
    f = DataFrame.from_dict({"k": list(range(1025)), "v": list(range(1025))})
    with pytest.raises(Exception, match="at least 1025"):
        f.write_table(out, partition_by=["k"])
    assert not os.path.exists(out)
    ok = DataFrame.from_dict({"k": list(range(1024)), "v": list(range(1024))})
    ok.write_table(out, partition_by=["k"])
    assert len(_parts(out)) == 1024


def _event_frame():
    return DataFrame.from_dict(
        {
            "name": ["a", "b", "c", "d"],
            "cat": ["io"] * 4,
            "pid": [0, 1, 2, 3],
            "tid": [1] * 4,
            "ts": [10, 20, 30, 40],
            "ph": [1] * 4,
        }
    )


def _eager(verb):
    return lambda f, out, **kw: getattr(f, verb)(out, **kw)


def _lazy_plan(verb):
    return lambda f, out, **kw: getattr(f.lazy(), verb)(out, **kw)


def _viewer(verb):
    def run(f, out, **kw):
        src = out + ".src.pfw.gz"
        f.write_trace(src, mode="overwrite")
        return getattr(TraceViewer(src), verb)(out, **kw)

    return run


@pytest.mark.parametrize(
    "run",
    [
        _eager("write_table"),
        _eager("write_trace"),
        _lazy_plan("sink_table"),
        _lazy_plan("sink_trace"),
        _viewer("sink_trace"),
    ],
    ids=["write_table", "write_trace", "lazy_sink_table", "lazy_sink_trace", "viewer_sink_trace"],
)
def test_max_partitions_is_a_limit_the_caller_sets(tmp_path, run):
    out = str(tmp_path / "out")
    f = _event_frame()
    with pytest.raises(Exception, match="at least 3"):
        run(f, out, partition_by=["pid"], max_partitions=2)
    assert not os.path.exists(out)
    run(f, out, partition_by=["pid"], max_partitions=4)
    assert len(_parts(out)) == 4
    with pytest.raises(ValueError, match="max_partitions"):
        run(f, out, partition_by=["pid"], max_partitions=0, mode="overwrite")


def test_a_session_sink_passes_max_partitions(tmp_path):
    src = str(tmp_path / "src.pfw.gz")
    _event_frame().write_trace(src)
    out = str(tmp_path / "out")
    viewer = TraceViewer(src)
    session = viewer.session()
    handle = session.sink_table(viewer, out, partition_by=["pid"], max_partitions=2)
    with pytest.raises(Exception, match="at least 3"):
        handle.result()
    session = viewer.session()
    handle = session.sink_table(
        viewer, out, partition_by=["pid"], max_partitions=4, mode="overwrite"
    )
    assert handle.result().part_count == 4


def test_more_partitions_than_open_files_still_write_every_row(tmp_path):
    n = 3000
    f = DataFrame.from_dict({"k": [i % 40 for i in range(n)], "v": list(range(n))})
    out = str(tmp_path / "out")
    f.write_table(
        out, partition_by=["k"], max_open_files=3, options=TraceWriteOptions(member_size=2000)
    )
    assert len(_parts(out)) == 40
    back = TraceViewer(out).collect()
    assert sorted(back["v"].to_list()) == list(range(n))
    for p in _parts(out):
        assert header_of(os.path.join(out, p))["nrows"] == n // 40
    with pytest.raises(ValueError, match="max_open_files"):
        f.write_table(str(tmp_path / "z"), partition_by=["k"], max_open_files=0)


def test_members_compressed_in_parallel_land_in_row_order_in_each_part(tmp_path):
    n = 20000
    f = DataFrame.from_dict({"k": [i % 3 for i in range(n)], "v": list(range(n))})
    out = str(tmp_path / "out")
    f.write_table(out, partition_by=["k"], options=TraceWriteOptions(member_size=1000))
    for key in range(3):
        part = TraceViewer(os.path.join(out, f"k={key}")).collect()
        assert part["v"].to_list() == list(range(key, n, 3))


_LATE_FAILURE_SCRIPT = textwrap.dedent(
    """
    import os, sys, tempfile
    from dftracer.utils import DataFrame, TraceWriteOptions
    n = 600_000
    k = [i % 3 for i in range(n)]
    for j in range(1, 5):
        k[n - j] = 100 + j
    f = DataFrame.from_dict(
        {"k": k, "v": list(range(n)), "s": [f"payload-{i % 977}-xxxxxxxxxxxxxxxxxxxx" for i in range(n)]}
    )
    d = tempfile.mkdtemp()
    try:
        f.write_table(
            os.path.join(d, "o"),
            partition_by=["k"],
            max_partitions=3,
            options=TraceWriteOptions(member_size="64KB"),
        )
        print("no error")
    except Exception as e:
        print("ERR", os.listdir(d))
    """
)


def test_a_write_that_fails_late_while_the_index_is_fed_stops_cleanly(tmp_path):
    # The failure comes at the last rows, with members of every partition
    # still waiting for the index. The process must survive each time.
    for _ in range(8):
        got = subprocess.run(
            [sys.executable, "-I", "-c", _LATE_FAILURE_SCRIPT],
            capture_output=True,
            text=True,
        )
        assert got.returncode == 0, got.stderr
        assert "ERR []" in got.stdout


_FAILING_PLAN_SCRIPT = textwrap.dedent(
    """
    import os, tempfile
    import numpy as np
    import dftracer.utils as dft
    from dftracer.utils import DataFrame, col
    from dftracer.utils.trace import TraceViewer
    d = tempfile.mkdtemp()
    n = 600_000
    src = os.path.join(d, "src.pfw.gz")
    DataFrame.from_dict({
        "name": ["a"] * n, "cat": ["io"] * n, "pid": [i % 5 for i in range(n)],
        "tid": [1] * n, "ts": list(range(n)), "ph": [1] * n, "dur": [1] * n,
    }).write_trace(src)
    ok_batches = [0]
    start = [0]

    @dft.udf(batch=True, returns=np.uint64)
    def bomb(x):
        ok_batches[0] += 1
        if ok_batches[0] - start[0] >= 3:
            raise ValueError("past the threshold")
        return x

    view = TraceViewer(src)
    plan = view.lazy().with_columns(j=bomb(col("ts")))
    runs = (
        lambda o: plan.sink_table(o, partition_by=["pid"]),
        lambda o: plan.sink_table(o + ".pfw.gz"),
        lambda o: view.duql(
            f'from "{src}" | derive j = bomb(ts) | sink table "{o}" partition_by "pid"'
        ).collect(),
    )
    for k, run in enumerate(runs):
        start[0] = ok_batches[0]
        try:
            run(os.path.join(d, f"out{k}"))
            print("WROTE")
        except Exception as e:
            assert "past the threshold" in str(e), str(e)
            assert ok_batches[0] - start[0] >= 3
            print("ERR", sorted(x for x in os.listdir(d) if x not in (".dftindex", "src.pfw.gz")))
    """
)


def test_a_plan_that_fails_after_some_batches_leaves_nothing_on_every_sink_path():
    got = subprocess.run(
        [sys.executable, "-I", "-c", _FAILING_PLAN_SCRIPT],
        capture_output=True,
        text=True,
    )
    assert got.returncode == 0, got.stderr
    lines = [x for x in got.stdout.splitlines() if x.startswith(("ERR", "WROTE"))]
    assert lines == ["ERR []"] * 3, got.stdout + got.stderr


def test_keys_that_differ_only_in_case_raise_before_anything_is_committed(tmp_path):
    out = str(tmp_path / "out")
    f = DataFrame.from_dict({"k": ["File", "x", "file"], "v": [1, 2, 3]})
    with pytest.raises(Exception, match="k=File and k=file differ only in case"):
        f.write_table(out, partition_by=["k"])
    assert os.listdir(tmp_path) == []
    nested = DataFrame.from_dict({"a": ["X", "x"], "b": [1, 2], "v": [1, 2]})
    with pytest.raises(Exception, match="differ only in case"):
        nested.write_table(out, partition_by=["a", "b"])
    assert os.listdir(tmp_path) == []


def test_an_over_long_partition_name_raises_before_anything_is_committed(tmp_path):
    out = str(tmp_path / "out")
    f = DataFrame.from_dict({"k": ["é" * 100], "v": [1]})
    with pytest.raises(Exception, match="over the limit"):
        f.write_table(out, partition_by=["k"])
    assert os.listdir(tmp_path) == []


def test_partitioned_trace_output_is_events_and_partition_needs_a_folder(tmp_path):
    events = DataFrame.from_dict(
        {
            "name": ["r", "w", "r"],
            "cat": ["POSIX"] * 3,
            "pid": [1, 1, 1],
            "tid": [1, 1, 1],
            "ts": [1, 2, 3],
            "ph": [1, 1, 1],
            "dur": [5, 6, 7],
        }
    )
    out = str(tmp_path / "ev")
    s = events.write_trace(out, partition_by=["name"])
    assert s.kind == "events" and s.rows == 3
    assert sorted({p.split("/")[0] for p in _parts(out)}) == ["name=r", "name=w"]
    assert TraceViewer(out).collect().num_rows == 3
    with pytest.raises(ValueError, match="partition_by needs a folder"):
        events.write_trace(str(tmp_path / "x.pfw.gz"), partition_by=["name"])


def test_a_partitioned_folder_has_one_shared_index_that_prunes(tmp_path):
    n = 40_000
    f = DataFrame.from_dict(
        {
            "k": [f"g{i % 4}" for i in range(n)],
            "i": list(range(n)),
            "s": [f"v{i % 50}" for i in range(n)],
        }
    )
    out = str(tmp_path / "out")
    f.lazy().sink_table(out, partition_by=["k"], options=TraceWriteOptions(member_size="16KB"))
    assert os.path.isdir(os.path.join(out, ".dftindex"))
    stores = [d for d, ds, _ in os.walk(out) if os.path.basename(d) == ".dftindex"]
    assert len(stores) == 1
    ext._table_read_counters(True)
    got = TraceViewer(out).filter(col("i") == 12345).collect()
    counts = ext._table_read_counters(True)
    assert got["i"].to_list() == [12345]
    assert counts["indexed_files"] >= 1 and counts["chunks_skipped"] > 0


def test_overwrite_replaces_a_partitioned_folder_atomically(tmp_path):
    out = str(tmp_path / "out")
    DataFrame.from_dict({"k": ["a", "b"], "v": [1, 2]}).write_table(out, partition_by=["k"])
    with pytest.raises(FileExistsError):
        DataFrame.from_dict({"k": ["c"], "v": [3]}).write_table(out, partition_by=["k"])
    DataFrame.from_dict({"k": ["c"], "v": [3]}).write_table(
        out, partition_by=["k"], mode="overwrite"
    )
    assert _parts(out) == ["k=c/part-00000.pfw.gz"]
    assert [n for n in os.listdir(tmp_path) if n.startswith(".out.")] == []


def test_a_killed_partitioned_write_leaves_nothing_at_the_target(tmp_path):
    target = tmp_path / "big"
    script = tmp_path / "child.py"
    script.write_text(
        textwrap.dedent(
            """
            import sys
            from dftracer.utils import DataFrame
            n = 6_000_000
            f = DataFrame.from_dict({"k": [i % 8 for i in range(n)], "s": ["x"] * n})
            f.lazy().sink_table(sys.argv[1], partition_by=["k"])
            """
        )
    )
    child = subprocess.Popen([sys.executable, str(script), str(target)], cwd=str(tmp_path))
    staging = None
    deadline = time.time() + 120
    while time.time() < deadline and child.poll() is None:
        found = [n for n in os.listdir(tmp_path) if n.startswith(".big.tmp-")]
        if found and any(f for _, _, fs in os.walk(tmp_path / found[0]) for f in fs):
            staging = found[0]
            break
        time.sleep(0.01)
    if staging is None:
        child.wait()
        pytest.skip("the write ended before it could be killed")
    child.send_signal(signal.SIGKILL)
    child.wait()
    assert not target.exists()
    DataFrame.from_dict({"k": ["a"], "v": [1]}).write_table(str(target), partition_by=["k"])
    assert not (tmp_path / staging).exists()
    assert TraceViewer(str(target)).collect().num_rows == 1


_PARTITION_SCRIPT = textwrap.dedent(
    """
    import resource, sys
    from dftracer.utils import DataFrame, col
    from dftracer.utils.trace import TraceViewer
    path, out, budget = sys.argv[1:4]
    plan = TraceViewer(path).lazy().memory_budget(int(budget)).filter(col("i") >= 0)
    def peak():
        # On Linux ru_maxrss also holds the peak of the process that spawned
        # this one, so read this image's own high-water mark.
        if sys.platform == "darwin":
            return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        with open("/proc/self/status") as status:
            return next(int(l.split()[1]) for l in status if l.startswith("VmHWM:")) * 1024
    DataFrame.from_dict({"i": [1]}).write_table(out + ".warm.pfw.gz")
    before = peak()
    summary = plan.sink_table(out, partition_by=["k"])
    print(summary.rows, summary.part_count, before, peak())
    """
)


@pytest.mark.skipif(sys.platform == "win32", reason="peak memory is read with resource")
def test_a_partitioned_sink_larger_than_the_budget_stays_bounded(tmp_path):
    rows = 1_500_000
    src = str(tmp_path / "in.pfw.gz")
    DataFrame.from_dict(
        {
            "i": list(range(rows)),
            "k": [i % 200 for i in range(rows)],
            "s": [f"payload-{i % 977}-xxxxxxxxxxxxxxxxxxxx" for i in range(rows)],
        }
    ).write_table(src)
    out = str(tmp_path / "out")
    got = subprocess.run(
        [sys.executable, "-I", "-c", _PARTITION_SCRIPT, src, out, str(_SINK_BUDGET)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    count, parts, before, after = (int(x) for x in got)
    assert count == rows and parts == 200
    assert rows * 90 > 4 * _SINK_BUDGET
    assert after - before < _EVENTS_PEAK_FACTOR * _SINK_BUDGET


def test_concat_joins_plans_at_the_top_level(tmp_path):
    a = DataFrame.from_dict({"x": [1, 2]})
    b = DataFrame.from_dict({"x": [3]})
    out = dft.concat([a.lazy(), b, a.lazy().filter(col("x") > 1)]).collect()
    assert out["x"].to_list() == [1, 2, 3, 2]
    path = str(tmp_path / "c.pfw.gz")
    assert dft.concat([a, b]).sink_table(path).rows == 3
    with pytest.raises(ValueError, match="at least one"):
        dft.concat([])


def test_a_duql_trace_or_table_sink_writes_what_the_verbs_write(tmp_path):
    src = str(tmp_path / "in.pfw.gz")
    DataFrame.from_dict(
        {
            "name": ["r", "w", "r", "w"],
            "cat": ["POSIX"] * 4,
            "pid": [1, 1, 2, 2],
            "tid": [1, 1, 1, 1],
            "ts": [1, 2, 3, 4],
            "ph": [1, 1, 1, 1],
            "dur": [5, 6, 7, 8],
        }
    ).write_trace(src)
    out = str(tmp_path / "out")
    got = (
        TraceViewer(src)
        .duql(f'from "{src}" | sink trace "{out}" partition_by "pid" mode "overwrite"')
        .collect()
    )
    assert got["rows"].to_list() == [4]
    assert sorted(_parts(out)) == ["pid=1/part-00000.pfw.gz", "pid=2/part-00000.pfw.gz"]
    assert TraceViewer(out).collect().num_rows == 4
    tab = str(tmp_path / "tab.pfw.gz")
    got = TraceViewer(src).duql(f'from "{src}" | select name, dur | sink table "{tab}"').collect()
    assert got["rows"].to_list() == [4]
    assert TraceViewer(tab).collect().num_rows == 4
    with pytest.raises(Exception, match="unknown sink option"):
        TraceViewer(src).duql(f'from "{src}" | sink table "{tmp_path}/z" nope "1"').collect()
