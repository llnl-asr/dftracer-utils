# SPDX-License-Identifier: MIT
"""A summary authored in Python and compiled by the JIT into a native index
extension: it prunes chunks, gives the rows a scan without an index gives, and
runs no Python at query time."""

import gc
import gzip
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

pytest.importorskip("dftracer.utils.dftracer_utils_ext")

import dftracer.utils as dft  # noqa: E402
from dftracer.utils import jit  # noqa: E402

from .jit_common import HAS_CXX  # noqa: E402

pytestmark = pytest.mark.skipif(not HAS_CXX, reason="no C++ compiler available for the jit backend")

MEMBERS = 12
PER_MEMBER = 10


def write_trace(path) -> str:
    """`size` rises with the event number, so each gzip member holds its own range."""
    out = bytearray()
    for first in range(0, MEMBERS * PER_MEMBER, PER_MEMBER):
        lines = []
        for i in range(first, first + PER_MEMBER):
            event = {
                "id": i,
                "name": "read",
                "cat": "POSIX",
                "pid": 1,
                "tid": 2,
                "ph": 1,
                "ts": 1000 + i,
                "dur": 5,
                "args": {"size": i * 10},
            }
            lines.append(json.dumps(event, separators=(",", ":")))
        out += gzip.compress(("\n".join(lines) + "\n").encode())
    target = path / "jit.pfw.gz"
    target.write_bytes(bytes(out))
    return str(target)


@jit.summary(name="acme.size_range", paths=["args.size"])
class SizeRange:
    n: int = 0
    lo: float = 0.0
    hi: float = 0.0

    def add(self, value):
        if self.n == 0 or value < self.lo:
            self.lo = value
        if self.n == 0 or value > self.hi:
            self.hi = value
        self.n += 1

    def merge(self, other):
        if other.n > 0:
            if self.n == 0 or other.lo < self.lo:
                self.lo = other.lo
            if self.n == 0 or other.hi > self.hi:
                self.hi = other.hi
            self.n += other.n

    def may_contain(self, cond):
        if self.n == 0:
            return False
        if cond.op == jit.EQ:
            return self.lo <= cond.value and cond.value <= self.hi
        if cond.op == jit.LT:
            return self.lo < cond.value
        if cond.op == jit.LE:
            return self.lo <= cond.value
        if cond.op == jit.GT:
            return self.hi > cond.value
        if cond.op == jit.GE:
            return self.hi >= cond.value
        return True


@pytest.fixture()
def built(tmp_path):
    return jit.build(SizeRange)


def test_the_summary_prunes_chunks_and_a_query_gives_the_rows_of_no_index(tmp_path, built):
    trace = write_trace(tmp_path)
    with dft.load_plugin(built):
        ix = dft.Indexer(
            trace, index_dir=str(tmp_path / "idx"), summaries={"size": ["acme.size_range"]}
        )
        status = ix.ensure()
        assert status.needs_work == []

        (entry,) = ix.explain("size >= 1000")
        assert entry["chunks"] == MEMBERS
        assert 0 < len(entry["read"]) < MEMBERS
        by = {(s["path"], s["summary"]): s for s in entry["summaries"]}
        plugin = by[("size", "acme.size_range")]
        assert plugin["chunks"] == MEMBERS
        assert plugin["skipped"] == MEMBERS - len(entry["read"])
        assert "summary size acme.size_range: skipped" in ix.explain_text("size >= 1000")

        (eq,) = ix.explain("size == 555")
        assert eq["read"] == [5]

        indexed = (
            dft.TraceViewer(trace, index_dir=str(tmp_path / "idx")).duql("size >= 1000").collect()
        )
        plain = (
            dft.TraceViewer(trace, index_dir=str(tmp_path / "none")).duql("size >= 1000").collect()
        )
        assert sorted(indexed.to_dict()["ts"]) == sorted(plain.to_dict()["ts"])
        assert sorted(indexed.to_dict()["ts"]) == [
            1000 + i for i in range(100, MEMBERS * PER_MEMBER)
        ]


def test_a_path_that_lists_only_the_summary_keeps_no_builtin_summary(tmp_path, built):
    trace = write_trace(tmp_path)
    with dft.load_plugin(built):
        ix = dft.Indexer(
            trace, index_dir=str(tmp_path / "idx"), summaries={"size": ["acme.size_range"]}
        )
        ix.ensure()
        rows = ix.summaries(trace).to_dict()
        assert "size" not in rows["path"]
        manifest = {e["name"] for e in ix.manifest()[0]["extensions"]}
        assert "acme.size_range" in manifest


def test_the_summary_must_take_the_path(tmp_path, built):
    trace = write_trace(tmp_path)
    with dft.load_plugin(built):
        ix = dft.Indexer(
            trace, index_dir=str(tmp_path / "idx"), summaries={"other": ["acme.size_range"]}
        )
        with pytest.raises(dft.DFTUtilsValueError, match="acme.size_range"):
            ix.status()


def test_the_built_plugin_holds_no_python(built):
    nm = shutil.which("nm")
    if nm is None:
        pytest.skip("nm not available")
    flags = ["-u"] if sys.platform == "darwin" else ["-D", "-u"]
    symbols = subprocess.run([nm, *flags, built], capture_output=True, text=True, check=True).stdout
    assert "Py" not in symbols and "python" not in symbols.lower()


def test_the_summary_prunes_after_its_python_class_is_gone(tmp_path):
    @jit.summary(name="acme.gone_range", paths=["args.size"])
    class GoneRange:
        hi: float = 0.0

        def add(self, value):
            if value > self.hi:
                self.hi = value

        def merge(self, other):
            if other.hi > self.hi:
                self.hi = other.hi

        def may_contain(self, cond):
            if cond.op == jit.GE:
                return self.hi >= cond.value
            return True

    path = jit.build(GoneRange)
    del GoneRange
    gc.collect()
    trace = write_trace(tmp_path)
    with dft.load_plugin(path):
        ix = dft.Indexer(
            trace, index_dir=str(tmp_path / "idx"), summaries={"size": ["acme.gone_range"]}
        )
        ix.ensure()
        (entry,) = ix.explain("size >= 1000")
        assert 0 < len(entry["read"]) < MEMBERS


@pytest.mark.parametrize(
    ("body", "named"),
    [
        ("import os", "outside the summary subset"),
        ("self.nope = 1", "assigns only the state fields"),
        ("while True:\n                pass", "outside the summary subset"),
    ],
)
def test_a_construct_outside_the_subset_is_a_located_error(body, named):
    source = f"""
import dftracer.utils.jit as jit

@jit.summary(name="acme.bad", paths=["args.x"])
class Bad:
    n: int = 0

    def add(self, value):
        {body}

    def merge(self, other):
        pass

    def may_contain(self, cond):
        return True
"""
    namespace: dict = {}
    with tempfile.TemporaryDirectory() as d:
        file = Path(d) / "bad_summary.py"
        file.write_text(source)
        code = compile(source, str(file), "exec")
        with pytest.raises(jit.JitError, match=named) as err:
            exec(code, namespace)
        assert err.value.line is not None


def test_the_name_and_paths_are_checked():
    with pytest.raises(jit.JitError, match="name"):

        @jit.summary(name="nodot", paths=["args.x"])
        class A:
            n: int = 0

            def add(self, value):
                self.n += 1

            def merge(self, other):
                self.n += other.n

            def may_contain(self, cond):
                return True

    with pytest.raises(jit.JitError, match="paths"):

        @jit.summary(name="acme.b", paths=[])
        class B:
            n: int = 0

            def add(self, value):
                self.n += 1

            def merge(self, other):
                self.n += other.n

            def may_contain(self, cond):
                return True


TWO53 = 2**53


def write_wide_trace(path) -> str:
    """Members of integers around 2**53 and around 1.7e18, one of nulls, one of
    NaN and small floats; `ts` numbers the rows."""
    members = [
        [TWO53 - 2, TWO53 - 1, TWO53, TWO53],
        [TWO53 + 1, TWO53 + 2, TWO53 + 3, TWO53 + 4],
        [TWO53, TWO53, TWO53, TWO53],
        [1_700_000_000_000_000_001, 1_700_000_000_000_000_002, 1_700_000_000_000_000_003],
        [1_700_000_000_000_000_000] * 3,
        [None, None, None],
        [float("nan"), 2.5, 1.5],
        [5, 6, 7],
    ]
    out = bytearray()
    ts = 0
    for values in members:
        lines = []
        for v in values:
            lines.append(
                json.dumps(
                    {
                        "name": "read",
                        "cat": "POSIX",
                        "pid": 1,
                        "tid": 2,
                        "ph": 1,
                        "ts": ts,
                        "dur": 1,
                        "args": {"x": v},
                    },
                    separators=(",", ":"),
                )
            )
            ts += 1
        out += gzip.compress(("\n".join(lines) + "\n").encode())
    target = path / "wide.pfw.gz"
    target.write_bytes(bytes(out))
    return str(target)


@jit.summary(name="acme.wide_range", paths=["args.x"])
class WideRange:
    n: int = 0
    lo: float = 0.0
    hi: float = 0.0

    def add(self, value):
        if self.n == 0 or value < self.lo:
            self.lo = value
        if self.n == 0 or value > self.hi:
            self.hi = value
        self.n += 1

    def merge(self, other):
        if other.n > 0:
            if self.n == 0 or other.lo < self.lo:
                self.lo = other.lo
            if self.n == 0 or other.hi > self.hi:
                self.hi = other.hi
            self.n += other.n

    def may_contain(self, cond):
        if self.n == 0:
            return False
        if cond.op == jit.EQ:
            return self.lo <= cond.value and cond.value <= self.hi
        if cond.op == jit.LT:
            return self.lo < cond.value
        if cond.op == jit.LE:
            return self.lo <= cond.value
        if cond.op == jit.GT:
            return self.hi > cond.value
        if cond.op == jit.GE:
            return self.hi >= cond.value
        return True


@pytest.mark.parametrize(
    "query",
    [
        f"x < {TWO53 + 1}",
        f"x > {TWO53}",
        f"x > {TWO53 - 1}",
        f"x == {TWO53 + 1}",
        f"x == {TWO53}",
        f"x != {TWO53}",
        "x is null",
        f"x <= {TWO53 + 1}",
        f"x >= {TWO53 + 1}",
        "x < 1700000000000000002",
        "x > 1700000000000000000",
        "x == 1700000000000000001",
        "x >= 1700000000000000003",
        "x <= 1700000000000000000",
        "x < 3",
        "x >= 5",
    ],
)
def test_values_above_two_to_the_53_give_the_rows_of_no_index(tmp_path, query):
    trace = write_wide_trace(tmp_path)
    plain = dft.TraceViewer(trace, index_dir=str(tmp_path / "none")).duql(query).collect()
    with dft.load_plugin(jit.build(WideRange)):
        ix = dft.Indexer(
            trace, index_dir=str(tmp_path / "idx"), summaries={"x": ["acme.wide_range"]}
        )
        ix.ensure()
        indexed = dft.TraceViewer(trace, index_dir=str(tmp_path / "idx")).duql(query).collect()
    assert sorted(indexed.to_dict().get("ts", [])) == sorted(plain.to_dict().get("ts", []))


def test_a_bloom_field_with_a_plugin_only_choice_does_not_rebuild(tmp_path):
    trace = write_wide_trace(tmp_path)
    with dft.load_plugin(jit.build(WideRange)):

        def indexer():
            return dft.Indexer(
                trace,
                index_dir=str(tmp_path / "idx"),
                require_bloom=dft.BloomConfig(fields=["x"]),
                summaries={"x": ["acme.wide_range"]},
            )

        assert indexer().ensure().needs_work == []
        assert indexer().status().needs_work == []
        again = indexer()
        assert again.ensure().needs_work == []
        assert again.status().needs_work == []
