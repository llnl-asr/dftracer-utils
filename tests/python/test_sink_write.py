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
    with pytest.raises(ValueError, match="partition_by is not supported yet"):
        f.write_table(path, partition_by=["s"])
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
        ("s", "object"),
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
        value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return value if sys.platform == "darwin" else value * 1024
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


def _write_events_trace(path, rows):
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
            n = min(left, 50_000)
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
        value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return value if sys.platform == "darwin" else value * 1024
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
