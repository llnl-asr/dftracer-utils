#!/usr/bin/env python3
"""A @jit.plugin inside a Session: attach, the fused scan, and the reads a body
declares."""

import builtins
import gzip
import re

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

from .jit_common import HAS_CXX, trace_dir, write_homog_trace, write_trace

pa = pytest.importorskip("pyarrow")


@jit.plugin
class _SessionCounts:
    hits = jit.map(key=(jit.i64,), value=jit.count())

    @jit.each_event
    def step(self, e):
        self.hits[(e.pid,)] += 1


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_plugin_fused_in_session(tmp_path):
    import dftracer.utils as dftu

    gz = str(tmp_path / "t.pfw.gz")
    write_trace(gz, 10, [100, 200, 300], ["f0"])
    with dftu.Indexer(files=[gz], index_dir=str(tmp_path)) as ix:
        ix.ensure_indexed()

    tv = dftu.TraceViewer(gz, index_path=str(tmp_path))
    with tv.session() as s:
        by_cat = s.collect(tv.group_by("cat").agg("count"))
        counts = s.attach(Plugins([_SessionCounts]))

    # collect and the plugin ran over one shared scan.
    cat = pa.table(by_cat.result()).to_pandas()
    assert int(cat["count"].sum()) == 10

    res = counts.result()["hits"]  # an emitted frame -> our DataFrame
    assert isinstance(res, dftu.DataFrame)
    pdf = pa.table(res).to_pandas()
    assert int(pdf["value"].sum()) == 10  # every event counted on the same scan
    assert len(pdf) == 3  # three distinct pids


@jit.plugin
class _SessionDurs:
    total = jit.map(key=(jit.i64,), value=jit.sum())

    @jit.each_event
    def step(self, e):
        self.total[(e.pid,)] += e.dur


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_session_attach_built_plugin_set(tmp_path):
    import dftracer.utils as dftu

    gz = str(tmp_path / "t.pfw.gz")
    write_trace(gz, 10, [100, 200, 300], ["f0"])
    with dftu.Indexer(files=[gz], index_dir=str(tmp_path)) as ix:
        ix.ensure_indexed()

    plugins = Plugins([_SessionCounts, _SessionDurs])
    tv = dftu.TraceViewer(gz, index_path=str(tmp_path))
    with tv.session() as s:
        by_cat = s.collect(tv.group_by("cat").agg("count"))
        attached = s.attach(plugins)

    assert int(pa.table(by_cat.result()).to_pandas()["count"].sum()) == 10

    res = attached.result()
    # a whole set attaches as a mapping, never collapsed to one value.
    assert isinstance(res, dict)
    assert set(res) == {"hits", "total"}
    hits = pa.table(res["hits"]).to_pandas()
    assert int(hits["value"].sum()) == 10
    assert len(hits) == 3
    total = pa.table(res["total"]).to_pandas()
    assert total["value"].sum() == pytest.approx(sum(10 + i for i in range(10)))


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_session_attach_same_set_twice_folds_it_once_each(tmp_path):
    import dftracer.utils as dftu

    gz = str(tmp_path / "t.pfw.gz")
    write_trace(gz, 4, [100], ["f0"])
    with dftu.Indexer(files=[gz], index_dir=str(tmp_path)) as ix:
        ix.ensure_indexed()

    plugins = Plugins([_SessionCounts])
    tv = dftu.TraceViewer(gz, index_path=str(tmp_path))
    s = tv.session()
    first = s.attach(plugins)
    second = s.attach(plugins)
    # Neither handle sees the other's fold: each counts every event once.
    for handle in (first, second):
        hits = pa.table(handle.result()["hits"]).to_pandas()
        assert int(hits["value"].sum()) == 4


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_session_attach_handle_read_before_execute_triggers_it(tmp_path):
    # Session Handles (collect/sink_json/.../attach) all share one contract: a
    # read before an explicit execute() triggers it rather than raising, so
    # `attached.result()` alone - with no `with` block and no s.execute() call
    # - still resolves correctly.
    import dftracer.utils as dftu

    gz = str(tmp_path / "t.pfw.gz")
    write_trace(gz, 6, [100], ["f0"])
    with dftu.Indexer(files=[gz], index_dir=str(tmp_path)) as ix:
        ix.ensure_indexed()

    plugins = Plugins([_SessionCounts])
    tv = dftu.TraceViewer(gz, index_path=str(tmp_path))
    s = tv.session()
    attached = s.attach(plugins)

    res = attached.result()
    hits = pa.table(res["hits"]).to_pandas()
    assert int(hits["value"].sum()) == 6


@jit.plugin
class _SessionCountsPosix:
    plan_query = 'cat == "POSIX"'
    hits = jit.map(key=(jit.i64,), value=jit.count())

    @jit.each_event
    def step(self, e):
        self.hits[(e.pid,)] += 1


def _write_two_category_traces(tmp_path, n: int):
    posix_dir = tmp_path / "posix"
    stdio_dir = tmp_path / "stdio"
    posix_dir.mkdir()
    stdio_dir.mkdir()
    write_homog_trace(str(posix_dir / "trace.pfw.gz"), n, "read", "POSIX")
    write_homog_trace(str(stdio_dir / "trace.pfw.gz"), n, "fwrite", "STDIO")
    return [str(posix_dir / "trace.pfw.gz"), str(stdio_dir / "trace.pfw.gz")]


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_session_attach_alone_applies_the_plugin_filter(tmp_path):
    import dftracer.utils as dftu

    n = 200
    files = _write_two_category_traces(tmp_path, n)
    with dftu.Indexer(files=files, index_dir=str(tmp_path)) as ix:
        ix.ensure_indexed()

    plugins = Plugins([_SessionCountsPosix])
    tv = dftu.TraceViewer(files, index_path=str(tmp_path))
    with tv.session() as s:
        attached = s.attach(plugins)

    res = attached.result()
    hits = pa.table(res["hits"]).to_pandas()
    assert int(hits["value"].sum()) == n  # the plugin's own filter still applies


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_session_attach_coscan_keeps_every_file(tmp_path):
    import dftracer.utils as dftu

    n = 200
    files = _write_two_category_traces(tmp_path, n)
    with dftu.Indexer(files=files, index_dir=str(tmp_path)) as ix:
        ix.ensure_indexed()

    plugins = Plugins([_SessionCountsPosix])
    tv = dftu.TraceViewer(files, index_path=str(tmp_path))
    with tv.session() as s:
        all_events = s.collect(tv.group_by().agg("count"))
        attached = s.attach(plugins)

    # The plugin's narrowing must not reach the shared scan: the collect
    # still sees both files.
    total = pa.table(all_events.result()).to_pandas()
    assert int(total["count"].sum()) == 2 * n

    res = attached.result()
    hits = pa.table(res["hits"]).to_pandas()
    assert int(hits["value"].sum()) == n  # plugin's own filter is unaffected


def _reads_and_resolved(src: str) -> "tuple[builtins.set[str], builtins.set[str]]":
    """The declared g_reads[] entries and the columns on_batch actually resolves.

    These must be exactly the same set: a column resolved but not declared gets
    projected away by the host and silently reads back NULL.
    """
    m = re.search(r"g_reads\[\] = \{(.*?)\};", src, re.S)
    reads = builtins.set(re.findall(r'"([^"]+)",', m.group(1))) if m else builtins.set()
    resolved = builtins.set(re.findall(r'dftu_jit_resolve\(df, "([^"]+)"', src))
    return reads, resolved


def test_jit_reads_declares_only_the_fields_the_body_uses():
    @jit.plugin
    class DurOnly:
        tot = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.tot[(0,)] += e.dur

    src = DurOnly._jit_plugin.source
    assert 'static const char* const g_reads[] = {\n    "dur",\n    NULL,\n};' in src
    assert "g_plugin.reads = reads;" in src
    for absent in ("cat", "name", "pid"):
        assert f'"{absent}",' not in src
    reads, resolved = _reads_and_resolved(src)
    assert reads == resolved == {"dur"}


def test_jit_reads_lists_only_the_arg_key_the_body_uses():
    @jit.plugin
    class SizeOnly:
        tot = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.tot[(0,)] += e.arg_i64("size")

    src = SizeOnly._jit_plugin.source
    reads, resolved = _reads_and_resolved(src)
    assert reads == resolved == {"args.size"}


def test_jit_reads_lists_a_field_and_an_arg_key():
    @jit.plugin
    class PidAndSize:
        tot = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.tot[(e.pid,)] += e.arg_i64("size")

    src = PidAndSize._jit_plugin.source
    i = src.index("g_reads[] = {")
    reads_block = src[i : src.index("};", i)]
    assert '"pid",' in reads_block
    assert '"args.size",' in reads_block
    assert "g_plugin.reads = reads;" in src
    reads, resolved = _reads_and_resolved(src)
    assert reads == resolved == {"pid", "args.size"}


def test_jit_raw_body_declares_no_reads():
    @jit.plugin
    class RawNoReads:
        edges = jit.map(key=(jit.i64, jit.str_), value=jit.count())

        @jit.each_event(raw=True)
        def step(self):
            return """
            uint32_t _r = _n_edges++;
            _k0_edges[_r] = (int64_t)e->pid;
            _k1_edges[_r] = e->name;
            _v0_edges[_r] = 1;
            """

    src = RawNoReads._jit_plugin.source
    assert "g_reads[]" not in src
    assert "g_plugin.reads" not in src


def _write_multi_arg_trace(path: str, rows) -> None:
    """rows: (pid, dur, size, rank) with several distinct arg keys per event."""
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i, (pid, dur, size, rank) in enumerate(rows):
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{1000 + i},"dur":{dur},"ph":"X",'
                f'"args":{{"size":{size},"rank":{rank},"mode":"r","tag":"x"}}}}\n'
            )


@pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_projected_plugin_matches_expected_over_many_arg_keys():
    # The batch carries four arg columns (size, rank, mode, tag); the plugin's
    # g_reads lists only "pid" and "args.size", so the projection must not drop
    # the one arg column the body actually needs.
    @jit.plugin
    class SizeSum:
        tot = jit.map(key=(jit.i64,), value=jit.sum())

        @jit.each_event
        def step(self, e):
            self.tot[(e.pid,)] += e.arg_i64("size")

    src = SizeSum._jit_plugin.source
    assert '"pid",' in src
    assert '"args.size",' in src
    for absent in ("args.rank", "args.mode", "args.tag"):
        assert f'"{absent}",' not in src

    rows = [
        (1, 10, 100, 1),
        (1, 20, 200, 2),
        (2, 30, 300, 3),
        (2, 40, 400, 4),
        (1, 50, 500, 5),
    ]
    d = trace_dir(_write_multi_arg_trace, rows)
    plugins = Plugins([SizeSum])
    run = plugins.run(d)

    tbl = pa.table(run.results["tot"])
    by_pid = {k: v for k, v in zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist())}
    expected: dict = {}
    for pid, _dur, size, _rank in rows:
        expected[pid] = expected.get(pid, 0) + size
    assert {k: float(v) for k, v in by_pid.items()} == {k: float(v) for k, v in expected.items()}
