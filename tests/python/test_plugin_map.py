#!/usr/bin/env python3
"""End-to-end tests for keyed accumulators.

A jit-authored plugin declares a keyed accumulator; the host folds each batch
into the engine's AggState through DFTU_SVC_AGG, merges the same-named
accumulator across workers and finalizes it to a frame surfaced to Python. The
graph below is then built in bulk from the columns (numpy + scipy), no per-edge
Python.
"""

import gzip
import shutil

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins, unnest

np = pytest.importorskip("numpy")
sparse = pytest.importorskip("scipy.sparse")
pa = pytest.importorskip("pyarrow")

_HAS_CXX = bool(shutil.which("c++") or shutil.which("clang++") or shutil.which("g++"))
_needs_cxx = pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler for the jit backend")


def _write_trace(path: str, n: int, pids, files) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i in range(n):
            pid = pids[i % len(pids)]
            fhash = files[i % len(files)]
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{1000 + i},"dur":{10 + i},"ph":"X",'
                f'"args":{{"fhash":"{fhash}","ret":{i}}}}}\n'
            )


@_needs_cxx
def test_process_file_edges_builds_adjacency(tmp_path):
    @jit.plugin
    class ProcessFileEdges:
        process_file_edges = jit.map(key=(jit.i64, jit.str_), value=jit.count())

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.process_file_edges[(e.pid, e.fhash)] += 1

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    _write_trace(str(tmp_path / "trace.pfw.gz"), n, pids, files)

    plugins = Plugins([ProcessFileEdges])
    run = plugins.run(str(tmp_path))

    assert "process_file_edges" in run.results
    tbl = pa.table(run.results["process_file_edges"])
    assert tbl.column_names == ["k0", "k1", "value"]

    k0 = tbl.column("k0").to_numpy(zero_copy_only=False)
    k1 = tbl.column("k1").to_numpy(zero_copy_only=False)
    val = tbl.column("value").to_numpy(zero_copy_only=False)

    # Every file event contributed 1, so the weights sum to the event count.
    assert int(val.sum()) == n

    # Build the weighted process<->file adjacency from bulk columns.
    procs, pi = np.unique(k0, return_inverse=True)
    fhs, fi = np.unique(k1, return_inverse=True)
    adj = sparse.coo_matrix((val, (pi, fi)), shape=(procs.size, fhs.size))
    assert adj.shape == (len(pids), len(files))
    assert int(adj.sum()) == n


@_needs_cxx
def test_process_file_edges_wide_builds_product_columns(tmp_path):
    @jit.plugin
    class Wide:
        edges = jit.map(
            key=(jit.i64, jit.str_),
            value=dict(v0=jit.count(), v1=jit.sum()),
        )

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.edges[(e.pid, e.fhash)].v0 += 1
                self.edges[(e.pid, e.fhash)].v1 += e.dur

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    _write_trace(str(tmp_path / "trace.pfw.gz"), n, pids, files)

    plugins = Plugins([Wide])
    run = plugins.run(str(tmp_path))

    tbl = pa.table(run.results["edges"])
    assert tbl.column_names == ["k0", "k1", "v0", "v1"]

    v0 = tbl.column("v0").to_numpy(zero_copy_only=False)
    v1 = tbl.column("v1").to_numpy(zero_copy_only=False)

    # v0 is the per-edge event count; v1 the per-edge total duration.
    assert int(v0.sum()) == n
    assert float(v1.sum()) == float(sum(10 + i for i in range(n)))


@_needs_cxx
def test_name_edges_resolves_str_key_column(tmp_path):
    @jit.plugin
    class NameEdges:
        name_edges = jit.map(key=(jit.i64, jit.str_), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.name_edges[(e.pid, e.name)] += 1

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    _write_trace(str(tmp_path / "trace.pfw.gz"), n, pids, files)

    plugins = Plugins([NameEdges])
    run = plugins.run(str(tmp_path))

    tbl = pa.table(run.results["name_edges"])
    assert tbl.column_names == ["k0", "k1", "value"]

    # k1 is the STR key component resolved to a string column, not raw ids.
    assert pa.types.is_string(tbl.schema.field("k1").type)
    assert set(tbl.column("k1").to_pylist()) == {"read"}

    val = tbl.column("value").to_numpy(zero_copy_only=False)
    assert int(val.sum()) == n


@_needs_cxx
def test_process_file_set_builds_list_string_column(tmp_path):
    @jit.plugin
    class FileSet:
        process_file_set = jit.map(key=(jit.i64,), value=jit.set())

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.process_file_set[(e.pid,)].observe(e.fhash)

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    _write_trace(str(tmp_path / "trace.pfw.gz"), n, pids, files)

    plugins = Plugins([FileSet])
    run = plugins.run(str(tmp_path))

    tbl = pa.table(run.results["process_file_set"])
    assert tbl.column_names == ["k0", "value"]

    # value is a String column: the set of files each pid touched, sorted and
    # joined by the set separator.
    assert pa.types.is_string(tbl.schema.field("value").type)

    by_pid = {
        k: v.split("\x1e")
        for k, v in zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist())
    }
    assert by_pid[1] == sorted(files)
    assert by_pid[2] == sorted(files)


def _write_seq_trace(path: str, events) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for pid, name, ts in events:
            f.write(
                f'{{"name":"{name}","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{ts},"dur":1,"ph":"X","args":{{}}}}\n'
            )


@_needs_cxx
def test_process_event_seq_builds_ts_ordered_list(tmp_path):
    @jit.plugin
    class EventSeq:
        process_event_seq = jit.map(key=(jit.i64,), value=jit.list())

        @jit.each_event
        def step(self, e):
            self.process_event_seq[(e.pid,)].append(e.name, order_by=e.ts)

    # File order differs from ts order: pid 1 -> [a,b,c] by ts.
    events = [
        (1, "c", 30),
        (1, "a", 10),
        (1, "b", 20),
        (2, "y", 5),
        (2, "x", 1),
    ]
    _write_seq_trace(str(tmp_path / "trace.pfw.gz"), events)

    plugins = Plugins([EventSeq])
    run = plugins.run(str(tmp_path))

    tbl = pa.table(run.results["process_event_seq"])
    assert tbl.column_names == ["k0", "value"]

    vtype = tbl.schema.field("value").type
    assert pa.types.is_list(vtype)
    assert pa.types.is_string(vtype.value_type)

    by_pid = {k: v for k, v in zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist())}
    assert by_pid[1] == ["a", "b", "c"]
    assert by_pid[2] == ["x", "y"]


@_needs_cxx
def test_many_keys_stay_one_accumulator(tmp_path):
    # One accumulator per name regardless of key cardinality: 300 distinct
    # (pid, file) keys all land in the same finalized frame.
    keys = 300
    with gzip.open(str(tmp_path / "trace.pfw.gz"), "wt", encoding="utf-8") as f:
        for i in range(keys):
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{i},"tid":1,'
                f'"ts":{1000 + i},"dur":{10 + i},"ph":"X",'
                f'"args":{{"fhash":"file{i}","ret":{i}}}}}\n'
            )

    @jit.plugin
    class Edges:
        process_file_edges = jit.map(key=(jit.i64, jit.str_), value=jit.count())

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.process_file_edges[(e.pid, e.fhash)] += 1

    plugins = Plugins([Edges])
    tbl = pa.table(plugins.run(str(tmp_path)).results["process_file_edges"])

    assert tbl.column_names == ["k0", "k1", "value"]
    assert tbl.num_rows == keys
    assert int(tbl.column("value").to_numpy(zero_copy_only=False).sum()) == keys
    k0 = tbl.column("k0").to_numpy(zero_copy_only=False)
    assert len(set(int(x) for x in k0)) == keys


def _write_dur_trace(path: str, events) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for pid, dur, ts in events:
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{ts},"dur":{dur},"ph":"X","args":{{}}}}\n'
            )


@_needs_cxx
def test_process_pid_durs_builds_ts_ordered_int64_list(tmp_path):
    @jit.plugin
    class PidDurs:
        process_pid_durs = jit.map(key=(jit.i64,), value=jit.list(of=jit.i64))

        @jit.each_event
        def step(self, e):
            self.process_pid_durs[(e.pid,)].append(e.dur, order_by=e.ts)

    # File order differs from ts order: pid 1 durations by ts -> [20,5,20,8].
    events = [
        (1, 20, 10),
        (1, 5, 20),
        (1, 20, 30),
        (1, 8, 40),
        (2, 3, 1),
        (2, 7, 5),
    ]
    _write_dur_trace(str(tmp_path / "trace.pfw.gz"), events)

    plugins = Plugins([PidDurs])
    run = plugins.run(str(tmp_path))

    tbl = pa.table(run.results["process_pid_durs"])
    assert tbl.column_names == ["k0", "value"]

    vtype = tbl.schema.field("value").type
    assert pa.types.is_list(vtype)
    assert pa.types.is_string(vtype.value_type)

    by_pid = {k: v for k, v in zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist())}
    assert by_pid[1] == ["20", "5", "20", "8"]
    assert by_pid[2] == ["3", "7"]


def test_unnest_string_list():
    tbl = pa.table(
        {"pid": [1, 2], "files": [["a", "b", "c"], ["x"]]},
        schema=pa.schema([("pid", pa.int64()), ("files", pa.list_(pa.string()))]),
    )
    out = unnest(tbl, "files")
    assert out.column_names == ["pid", "files"]
    assert out.num_rows == 4
    assert out.column("pid").to_pylist() == [1, 1, 1, 2]
    assert out.column("files").to_pylist() == ["a", "b", "c", "x"]


def test_unnest_int64_list():
    tbl = pa.table(
        {"pid": [1, 2], "durs": [[20, 5, 8], [3]]},
        schema=pa.schema([("pid", pa.int64()), ("durs", pa.list_(pa.int64()))]),
    )
    out = unnest(tbl, "durs")
    assert out.num_rows == 4
    assert out.column("pid").to_pylist() == [1, 1, 1, 2]
    assert out.column("durs").to_pylist() == [20, 5, 8, 3]


def test_unnest_struct_list_flattens_fields():
    struct_ty = pa.struct([("value", pa.string()), ("count", pa.int64())])
    tbl = pa.table(
        {
            "pid": [1, 2],
            "tk": [
                [{"value": "read", "count": 5}, {"value": "write", "count": 2}],
                [{"value": "open", "count": 9}],
            ],
        },
        schema=pa.schema([("pid", pa.int64()), ("tk", pa.list_(struct_ty))]),
    )
    out = unnest(tbl, "tk")
    assert out.column_names == ["pid", "value", "count"]
    assert out.num_rows == 3
    assert out.column("pid").to_pylist() == [1, 1, 2]
    assert out.column("value").to_pylist() == ["read", "write", "open"]
    assert out.column("count").to_pylist() == [5, 2, 9]


def test_unnest_empty_list_drop_vs_keep_empty():
    tbl = pa.table(
        {"pid": [1, 2, 3], "files": [["a"], [], ["b"]]},
        schema=pa.schema([("pid", pa.int64()), ("files", pa.list_(pa.string()))]),
    )
    dropped = unnest(tbl, "files")
    assert dropped.column("pid").to_pylist() == [1, 3]
    assert dropped.column("files").to_pylist() == ["a", "b"]

    kept = unnest(tbl, "files", keep_empty=True)
    assert kept.column("pid").to_pylist() == [1, 2, 3]
    assert kept.column("files").to_pylist() == ["a", None, "b"]


@_needs_cxx
def test_unnest_aggregated_set_recovers_edges(tmp_path):
    # jit.list is the list-producing verb (jit.set joins its distinct values
    # into one String cell), so it is what unnest explodes back into rows.
    @jit.plugin
    class FileSet:
        process_file_set = jit.map(key=(jit.i64,), value=jit.list())

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.process_file_set[(e.pid,)].append(e.fhash, order_by=e.ts)

    n = 60
    pids = [1, 2]
    files = ["fileA", "fileB", "fileC"]
    _write_trace(str(tmp_path / "trace.pfw.gz"), n, pids, files)

    plugins = Plugins([FileSet])
    run = plugins.run(str(tmp_path))

    # The aggregated per-pid file set explodes back into scannable (pid, file)
    # rows: the inverse of the set-union aggregate.
    out = unnest(pa.table(run.results["process_file_set"]), "value")
    assert out.column_names == ["k0", "value"]
    edges = set(zip(out.column("k0").to_pylist(), out.column("value").to_pylist()))
    assert edges == {(p, f) for p in pids for f in files}
