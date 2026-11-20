# SPDX-License-Identifier: MIT
"""Strings read from a file or from JSON are views of the bytes the host lends:
they are interned only where an id is needed (a key or an element of state), so
a scan of many distinct lines does not grow the intern table."""

import gzip
import os
import shutil

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

from .jit_common import assert_located, compile_error, load_module, plugin_stats

pa = pytest.importorskip("pyarrow")

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)

_CFG = """
from dataclasses import dataclass
from typing import Set
from dftracer.utils import jit

@dataclass(frozen=True)
class Cfg:
    path: str = ""
    needle: str = "line7"
"""


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("view_trace")
    with gzip.open(os.path.join(d, "t.pfw.gz"), "wt", encoding="utf-8") as fh:
        fh.write(
            '{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"ph":1,"args":{}}\n'
        )
    return str(d)


def _total(results, name):
    return sum(pa.table(results[name]).to_pydict()["value"])


def test_a_big_file_that_is_only_counted_and_compared_interns_nothing(trace, tmp_path):
    big = tmp_path / "big.txt"
    count = 200_000
    big.write_text("".join(f"line{i}\n" for i in range(count)), encoding="utf-8")
    module = load_module(
        tmp_path,
        "view_count",
        _CFG
        + """
@jit.plugin
class Count:
    config: Cfg
    total: int = 0
    hits: int = 0
    same: int = 0
    longer: int = 0

    @jit.each_event
    def step(self, e):
        with jit.open(self.config.path) as f:
            for line in f:
                self.total += 1
                if line == "line7":
                    self.hits += 1
                if line == self.config.needle:
                    self.same += 1
                if len(line) > 9:
                    self.longer += 1
""",
    )
    plugins = Plugins([module.Count], config={"Count": {"path": str(big)}})
    results = plugins.run(trace).results
    assert _total(results, "total") == count
    assert _total(results, "hits") == 1
    assert _total(results, "same") == 1
    assert _total(results, "longer") == sum(1 for i in range(count) if len(f"line{i}") > 9)
    stats = plugin_stats(jit.compile_class(module.Count))
    # The table holds the literals and the config string; it does not grow with the lines.
    assert stats["interned"] < 50


def test_a_line_stored_in_a_set_is_interned_and_the_result_is_right(trace, tmp_path):
    data = tmp_path / "some.txt"
    data.write_text("b\na\nb\nc\n", encoding="utf-8")
    module = load_module(
        tmp_path,
        "view_set",
        _CFG
        + """
@jit.plugin
class Keep:
    config: Cfg
    names: Set[str] = set()
    by = jit.map(key=(jit.str_,), value=jit.count())

    @jit.each_event
    def step(self, e):
        with jit.open(self.config.path) as f:
            for line in f:
                self.names.add(line)
                self.by[(line,)] += 1
""",
    )
    plugins = Plugins([module.Keep], config={"Keep": {"path": str(data)}})
    results = plugins.run(trace).results
    assert sorted(pa.table(results["names"]).to_pydict()["value"][0].split("\x1e")) == [
        "a",
        "b",
        "c",
    ]
    cols = pa.table(results["by"]).to_pydict()
    assert dict(zip(cols["k0"], cols["value"])) == {"a": 1, "b": 2, "c": 1}
    stats = plugin_stats(jit.compile_class(module.Keep))
    # four lines, each interned once although the set and the map both take it
    assert stats["interned"] == 4


def test_a_view_of_a_json_string_compares_emits_and_parses_without_interning(trace, tmp_path):
    module = load_module(
        tmp_path,
        "view_json",
        """
from dftracer.utils import jit

@jit.plugin
class J:
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1

    @jit.finalize
    def done(self):
        d = jit.json('{"s": "{\\\\"v\\\\": 4}", "t": "hi"}')
        inner = jit.json(d.find("s").as_str())
        tag = d.find("t").as_str()
        if tag == "hi":
            jit.emit("same", len(tag))
        jit.emit("tag", tag)
        jit.emit("v", inner.find("v").as_int())
        jit.emit("none", d.find("zz").as_str("fallback"))
""",
    )
    plugins = Plugins([module.J])
    got = {k: v for k, v in plugins.run(trace).results.items() if isinstance(v, bytes)}
    assert got == {"same": b"2", "tag": b"hi", "v": b"4", "none": b"fallback"}


def test_a_read_chunk_is_a_view_valid_until_the_next_read(trace, tmp_path):
    raw = tmp_path / "raw.bin"
    raw.write_bytes(b"abcdefghij")
    module = load_module(
        tmp_path,
        "view_chunks",
        _CFG
        + """
@jit.plugin
class C:
    config: Cfg
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1

    @jit.finalize
    def done(self):
        with jit.open(self.config.path) as f:
            a = f.read(4)
            jit.emit("a", a)
            b = f.read(4)
            jit.emit("b", b)
            c = f.read(4)
            jit.emit("c", c)
            d = f.read(4)
            jit.emit("d", len(d))
""",
    )
    got = {
        k: v
        for k, v in Plugins([module.C], config={"C": {"path": str(raw)}}).run(trace).results.items()
        if isinstance(v, bytes)
    }
    assert got == {"a": b"abcd", "b": b"efgh", "c": b"ij", "d": b"0"}


_LIFE = (
    _CFG
    + """
@jit.plugin
class L:
    config: Cfg
    names: Set[str] = set()
    n: int = 0

    @jit.each_event
    def step(self, e):
{body}
"""
)


def _life(tmp_path, body):
    indented = "\n".join("        " + ln for ln in body.strip("\n").splitlines())
    return load_module(tmp_path, "view_life", _LIFE.format(body=indented))


def test_a_line_cannot_outlive_its_loop(tmp_path):
    with compile_error("only inside its for loop") as info:
        _life(
            tmp_path,
            """
last = 0
with jit.open(self.config.path) as f:
    for line in f:
        self.n += 1
    self.names.add(line)
""",
        )
    assert info.value.source is not None and "self.names.add(line)" in info.value.source


def test_a_view_cannot_be_copied_into_a_longer_lived_local(tmp_path):
    with compile_error("cannot be copied") as info:
        _life(
            tmp_path,
            """
keep = e.name
with jit.open(self.config.path) as f:
    for line in f:
        keep = line
""",
        )
    assert_located(info.value)


def test_a_chunk_is_stale_after_the_next_read(tmp_path):
    with compile_error("read again"):
        _life(
            tmp_path,
            """
with jit.open(self.config.path) as f:
    a = f.read(4)
    b = f.read(4)
    self.names.add(a)
""",
        )


def test_a_json_string_is_stale_after_its_document_is_parsed_again(tmp_path):
    with compile_error("parsed again"):
        _life(
            tmp_path,
            """
d = jit.json('{"a": "x"}')
s = d.find("a").as_str()
d = jit.json('{"a": "y"}')
self.names.add(s)
""",
        )


def test_a_second_loop_over_a_file_that_a_loop_iterates_is_refused(tmp_path):
    with compile_error("already iterated") as info:
        _life(
            tmp_path,
            """
with jit.open(self.config.path) as f:
    for a in f:
        for b in f:
            self.n += 1
        self.names.add(a)
""",
        )
    assert info.value.source is not None and "for b in f" in info.value.source


def test_a_read_inside_a_loop_over_the_same_file_is_refused(tmp_path):
    with compile_error("not both"):
        _life(
            tmp_path,
            """
with jit.open(self.config.path) as f:
    for a in f:
        c = f.read(4)
        self.names.add(a)
""",
        )
