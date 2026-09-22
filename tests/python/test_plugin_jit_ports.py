#!/usr/bin/env python3
"""Inter-plugin communication for @jit.plugin authoring.

A jit plugin declares a batch-scoped publish port with jit.publish (its wire id
is package-derived, see JitPackage) and a consume port wired to it with
jit.consume(Producer.port); the host hands the per-batch published value across
via DFTU_SVC_PORTS, running the producer first from the provides/consumes the
JIT derives from the two bodies. These tests cover the generated C
(publish/consume scaffolding and the derived name lists) without a compiler,
and the end-to-end data crossing - in either load order, plus the
absent-producer build error - with one.
"""

import gzip
import re
import shutil

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

pa = pytest.importorskip("pyarrow")

_HAS_CXX = bool(shutil.which("c++") or shutil.which("clang++") or shutil.which("g++"))


def _write_trace(path: str, n: int, pids) -> None:
    with gzip.open(path, "wt", encoding="utf-8") as f:
        for i in range(n):
            pid = pids[i % len(pids)]
            f.write(
                f'{{"name":"read","cat":"POSIX","pid":{pid},"tid":1,'
                f'"ts":{1000 + i},"dur":{10 + i},"ph":"X","args":{{}}}}\n'
            )


def _names(src: str, slot: str):
    """The quoted entries of `_<slot>_names[...]` in generated source."""
    m = re.search(rf"_{slot}_names\[\d+\] = \{{(.*?)\}};", src)
    assert m, f"no _{slot}_names array in source"
    return {tok.strip().strip('"') for tok in m.group(1).split(",") if tok.strip() != "NULL"}


# --- codegen-only checks (no C++ compiler needed) ---------------------------


def test_jit_publish_emits_flush():
    @jit.plugin
    class Producer:
        counts = jit.map(key=(jit.i64,), value=jit.count())
        sig = jit.publish(of=jit.u64)

        @jit.each_event
        def step(self, e):
            self.counts[(e.pid,)] += 1
            self.sig += 1

    src = Producer._jit_plugin.source
    assert Producer.sig.name in src  # the derived wire id, e.g. "<pkg>/...producer.sig"
    assert "_pub_sig += (uint64_t)" in src
    assert "_ports->publish(" in src
    assert "DFTU_SVC_PORTS" in src


def test_jit_consume_emits_read():
    @jit.plugin
    class Producer:
        counts = jit.map(key=(jit.i64,), value=jit.count())
        sig = jit.publish(of=jit.u64)

        @jit.each_event
        def step(self, e):
            self.counts[(e.pid,)] += 1
            self.sig += 1

    @jit.plugin
    class Consumer:
        got = jit.map(key=(jit.i64,), value=jit.count())
        sig = jit.consume(Producer.sig, of=jit.u64)

        @jit.each_event
        def step(self, e):
            if self.sig > 0:
                self.got[(e.pid,)] += 1

    src = Consumer._jit_plugin.source
    assert Producer.sig.name in src
    assert "_sub_sig" in src
    assert "_ports->consume(" in src


def test_jit_f64_publish_uses_double():
    @jit.plugin
    class P:
        m = jit.map(key=(jit.i64,), value=jit.count())
        tot = jit.publish(of=jit.f64)

        @jit.each_event
        def step(self, e):
            self.m[(e.pid,)] += 1
            self.tot += e.dur

    src = P._jit_plugin.source
    assert "double _pub_tot = 0.0;" in src
    assert "_pub_tot += (double)" in src


def test_jit_derives_provides_and_consumes():
    @jit.plugin
    class Upstream:
        counts = jit.map(key=(jit.i64,), value=jit.count())
        feed = jit.publish(of=jit.u64)

        @jit.each_event
        def step(self, e):
            self.counts[(e.pid,)] += 1
            self.feed += 1

    @jit.plugin
    class Both:
        counts = jit.map(key=(jit.i64,), value=jit.count())
        out = jit.publish(of=jit.u64)
        inp = jit.consume(Upstream.feed, of=jit.u64)

        @jit.each_event
        def step(self, e):
            if self.inp > 0:
                self.counts[(e.pid,)] += 1
            self.out += 1

    src = Both._jit_plugin.source
    counts_id = Both._jit_plugin.result_names  # {wire id: attr}; one entry, "counts"
    (counts_wire_id,) = [wid for wid, attr in counts_id.items() if attr == "counts"]
    # Produced: the published port and the accumulator this plugin creates.
    assert _names(src, "provides") == {Both.out.name, counts_wire_id}
    assert _names(src, "consumes") == {Upstream.feed.name}
    assert "g_plugin.provides = provides;" in src
    assert "g_plugin.consumes = consumes;" in src


def test_jit_derives_no_consumes_without_a_consume_port():
    @jit.plugin
    class Solo:
        counts = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.counts[(e.pid,)] += 1

    src = Solo._jit_plugin.source
    (counts_wire_id,) = Solo._jit_plugin.result_names.keys()
    assert "g_plugin.consumes = NULL;" in src
    assert _names(src, "provides") == {counts_wire_id}


def test_map_and_port_wire_ids_are_package_qualified_and_lowercase():
    @jit.plugin
    class Wide:
        edges = jit.map(key=(jit.i64,), value=jit.count())
        sig = jit.publish(of=jit.u64)

        @jit.each_event
        def step(self, e):
            self.edges[(e.pid,)] += 1
            self.sig += 1

    (edges_wire_id,) = Wide._jit_plugin.result_names.keys()
    assert edges_wire_id == edges_wire_id.lower()
    assert edges_wire_id.endswith(".wide.edges")
    assert "/" in edges_wire_id
    assert Wide.sig.name.endswith(".wide.sig")


def test_identity_matches_the_worked_example_and_is_stable_across_a_file_move():
    # Identity comes from __module__/__qualname__ (the import path), never
    # __file__ - so moving the file that defines the class to a new location
    # (same import path) produces the same identity.
    class A:
        pass

    A.__module__ = "acme.stats.io"
    A.__qualname__ = "Wide"

    class B:
        pass

    B.__module__ = "acme.stats.io"
    B.__qualname__ = "Wide"

    default_pkg = jit.JitPackage()
    id_a = default_pkg.identity(A, "edges")
    id_b = default_pkg.identity(B, "edges")
    assert id_a == id_b == "acme/stats.io.wide.edges"


def test_two_classes_in_one_module_with_the_same_attr_do_not_collide():
    @jit.plugin
    class Wide:
        edges = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.edges[(e.pid,)] += 1

    @jit.plugin
    class Narrow:
        edges = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.edges[(e.pid,)] += 1

    (wide_id,) = Wide._jit_plugin.result_names.keys()
    (narrow_id,) = Narrow._jit_plugin.result_names.keys()
    assert wide_id != narrow_id
    assert Wide._jit_plugin.result_names[wide_id] == "edges"
    assert Narrow._jit_plugin.result_names[narrow_id] == "edges"


def test_jit_package_matching_the_default_namespace_changes_no_identity():
    class Sample:
        pass

    top_level = __name__.split(".", 1)[0]
    default_id = jit.JitPackage().identity(Sample, "edges")
    explicit_id = jit.JitPackage(top_level).identity(Sample, "edges")
    # An explicit namespace equal to the module's own top-level package
    # produces the identical identity a bare jit.plugin/jit.map would.
    assert default_id == explicit_id


def test_jit_package_explicit_namespace_renames_only_the_leading_segment():
    pkg = jit.JitPackage("acme")

    @pkg.plugin
    class Renamed:
        edges = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.edges[(e.pid,)] += 1

    (renamed_id,) = Renamed._jit_plugin.result_names.keys()
    assert renamed_id.startswith("acme/")
    assert renamed_id.endswith(".renamed.edges")


def _undecorated_edges_class():
    class ScriptPlugin:
        edges = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.edges[(e.pid,)] += 1

    return ScriptPlugin


def test_main_module_refuses_a_shippable_build_without_a_package():
    # A script/notebook has no import path, so decorating from __main__ (here
    # simulated by overriding __module__) with the bare decorator leaves the
    # plugin usable, but not shippable.
    ScriptPlugin = _undecorated_edges_class()
    ScriptPlugin.__module__ = "__main__"
    ScriptPlugin = jit.plugin(ScriptPlugin)
    assert not ScriptPlugin._jit_plugin.shippable
    with pytest.raises(jit.JitError, match="__main__"):
        jit.build(ScriptPlugin)


def test_main_module_with_a_jit_package_is_shippable():
    ScriptPlugin = _undecorated_edges_class()
    ScriptPlugin.__module__ = "__main__"
    pkg = jit.JitPackage("acme")
    ScriptPlugin = pkg.plugin(ScriptPlugin)
    assert ScriptPlugin._jit_plugin.shippable


def test_jit_port_rejects_reserved_namespace():
    with pytest.raises(jit.JitError, match="reserved dftu"):
        jit.consume("dftu.cap.events")
    with pytest.raises(jit.JitError, match="reserved dftu"):
        jit.JitPackage("dftu")


def test_jit_port_rejects_bad_charset():
    with pytest.raises(jit.JitError, match="ASCII"):
        jit.consume("Bad Id!")


def test_jit_port_rejects_bad_width():
    with pytest.raises(jit.JitError, match="must be jit.u64"):
        jit.publish(of=jit.str_)


def test_jit_consume_rejects_a_non_publish_object():
    with pytest.raises(jit.JitError, match="jit.publish attribute"):
        jit.consume(jit.consume("a.b"))


def test_jit_rejects_reading_a_publish_port():
    with pytest.raises(jit.JitError, match="publish port"):

        @jit.plugin
        class Bad:
            m = jit.map(key=(jit.i64,), value=jit.sum())
            p = jit.publish()

            @jit.each_event
            def step(self, e):
                self.m[(e.pid,)] += self.p


def test_jit_rejects_writing_a_consume_port():
    with pytest.raises(jit.JitError, match="consume port"):

        @jit.plugin
        class Bad:
            m = jit.map(key=(jit.i64,), value=jit.sum())
            c = jit.consume("a.b")

            @jit.each_event
            def step(self, e):
                self.c += 1


# --- end-to-end data-crossing checks (need a C++ compiler) ------------------


def _producer():
    @jit.plugin
    class Producer:
        # A results map keeps the producer's own output; the port carries the
        # per-batch event count (>= 1 for any non-empty batch) to a consumer.
        seen = jit.map(key=(jit.i64,), value=jit.count())
        sig = jit.publish(of=jit.u64)

        @jit.each_event
        def step(self, e):
            self.seen[(e.pid,)] += 1
            self.sig += 1

    return Producer


def _consumer(producer):
    @jit.plugin
    class Consumer:
        got = jit.map(key=(jit.i64,), value=jit.count())
        sig = jit.consume(producer.sig, of=jit.u64)

        @jit.each_event
        def step(self, e):
            # Counts an event only when a producer published a value this batch.
            if self.sig > 0:
                self.got[(e.pid,)] += 1

    return Consumer


def _pid_counts(tbl):
    return {k: v for k, v in zip(tbl.column("k0").to_pylist(), tbl.column("value").to_pylist())}


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_ports_data_crosses_producer_to_consumer(tmp_path):
    n = 80
    pids = [1, 2, 3]
    _write_trace(str(tmp_path / "trace.pfw.gz"), n, pids)

    producer = _producer()
    consumer = _consumer(producer)
    plugins = Plugins([producer, consumer])
    results = plugins.run(str(tmp_path)).results

    seen = _pid_counts(pa.table(results["seen"]))
    got = _pid_counts(pa.table(results["got"]))
    # The producer published a nonzero value every batch, so the consumer counted
    # every event: its per-pid distribution matches the producer's own.
    assert got == seen
    assert sum(got.values()) == n


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_ports_cross_in_either_load_order(tmp_path):
    # The consumer is listed first; the publish/consume names the JIT derives
    # decide the fold order, not the Plugins([...]) list order.
    n = 80
    pids = [1, 2, 3]
    _write_trace(str(tmp_path / "trace.pfw.gz"), n, pids)

    producer = _producer()
    consumer = _consumer(producer)
    plugins = Plugins([consumer, producer])
    results = plugins.run(str(tmp_path)).results

    seen = _pid_counts(pa.table(results["seen"]))
    got = _pid_counts(pa.table(results["got"]))
    assert got == seen
    assert sum(got.values()) == n


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_jit_consume_without_a_producer_fails_the_build():
    # A consume port no loaded plugin publishes is a load error, not a whole
    # scan that quietly reads zero.
    producer = _producer()
    consumer = _consumer(producer)
    with pytest.raises(Exception, match=re.escape(producer.sig.name)):
        Plugins([consumer])


@pytest.mark.skipif(not _HAS_CXX, reason="no C++ compiler available for the jit backend")
def test_results_key_stays_the_attr_name_with_and_without_jit_package(tmp_path):
    # The wire id a jit.map sends the host is package-qualified, but a caller
    # still indexes run()'s results by the plain attribute name.
    _write_trace(str(tmp_path / "trace.pfw.gz"), 20, [1])

    @jit.plugin
    class Bare:
        edges = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.edges[(e.pid,)] += 1

    pkg = jit.JitPackage("acme")

    @pkg.plugin
    class Packaged:
        edges = jit.map(key=(jit.i64,), value=jit.count())

        @jit.each_event
        def step(self, e):
            self.edges[(e.pid,)] += 1

    for cls in (Bare, Packaged):
        plugins = Plugins([cls])
        results = plugins.run(str(tmp_path)).results
        assert "edges" in results
