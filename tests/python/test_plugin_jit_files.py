# SPDX-License-Identifier: MIT
"""``with jit.open(path) as f`` and ``for line in f`` / ``f.read(n)`` in a compiled
body, lowered to the file service (dftu.svc.fileio): lines of a plain or a gzip
file, raw chunks, and the close of the file on every way out of the block."""

import gzip
import os
import shutil
import tempfile
from dataclasses import dataclass
from typing import Set

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

from .jit_common import assert_located, load_module, plugin_stats

pa = pytest.importorskip("pyarrow")

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)

_EVENTS = 3


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("file_trace")
    with gzip.open(os.path.join(d, "t.pfw.gz"), "wt", encoding="utf-8") as fh:
        for i in range(_EVENTS):
            fh.write(
                f'{{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":{1000 + i},'
                f'"dur":{10 + i},"ph":1,"args":{{}}}}\n'
            )
    return str(d)


@pytest.fixture()
def files(tmp_path):
    plain = tmp_path / "plain.txt"
    plain.write_text("alpha\nbeta\ngamma\n", encoding="utf-8")
    packed = tmp_path / "packed.txt.gz"
    with gzip.open(packed, "wt", encoding="utf-8") as fh:
        fh.write("one\ntwo\n\nfour\n")
    raw = tmp_path / "raw.bin"
    raw.write_bytes(b"abcdefghij")
    return {"plain": str(plain), "packed": str(packed), "raw": str(raw), "dir": str(tmp_path)}


def _bytes(results):
    return {k: v for k, v in results.items() if isinstance(v, bytes)}


def _total(results, name):
    cols = pa.table(results[name]).to_pydict()
    return sum(cols["value"]) if cols["value"] else 0


def _names(results, name):
    cell = pa.table(results[name]).to_pydict()["value"][0]
    return sorted(cell.split("\x1e"))


@dataclass(frozen=True)
class PathCfg:
    path: str = ""


def _line_reader():
    @jit.plugin
    class Lines:
        config: PathCfg
        names: Set[str] = set()
        total: int = 0

        @jit.each_event
        def step(self, e):
            with jit.open(self.config.path) as f:
                for line in f:
                    self.names.add(line)
                    self.total += 1

    return Lines


def test_lines_of_a_plain_file_go_into_plain_state(trace, files):
    plugins = Plugins([_line_reader()], config={"Lines": {"path": files["plain"]}})
    results = plugins.run(trace).results
    assert _names(results, "names") == ["alpha", "beta", "gamma"]
    # The statement runs once per event: three events, three lines each.
    assert _total(results, "total") == 3 * _EVENTS


def test_lines_of_a_gzip_file_are_inflated(trace, files):
    plugins = Plugins([_line_reader()], config={"Lines": {"path": files["packed"]}})
    results = plugins.run(trace).results
    assert _names(results, "names") == ["", "four", "one", "two"]
    assert _total(results, "total") == 4 * _EVENTS


def test_a_literal_path_and_a_finalize_body(trace, files, tmp_path):
    module = load_module(
        tmp_path,
        "file_literal",
        f"""
        from dftracer.utils import jit

        @jit.plugin
        class Literal:
            n: int = 0

            @jit.each_event
            def step(self, e):
                self.n += 1

            @jit.finalize
            def done(self):
                count = 0
                found = 0
                with jit.open({files["packed"]!r}) as f:
                    for line in f:
                        count += 1
                        if line == "four":
                            found += 1
                jit.emit("count", count)
                jit.emit("found", found)
        """,
    )
    assert _bytes(Plugins([module.Literal]).run(trace).results) == {"count": b"4", "found": b"1"}


def test_reading_raw_chunks(trace, files):
    @jit.plugin
    class Chunks:
        config: PathCfg
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
                jit.emit("d", d)

    got = _bytes(Plugins([Chunks], config={"Chunks": {"path": files["raw"]}}).run(trace).results)
    assert got == {"a": b"abcd", "b": b"efgh", "c": b"ij", "d": b""}


def test_a_missing_file_raises_not_found_with_the_path(trace, files):
    missing = os.path.join(files["dir"], "missing.txt")
    plugins = Plugins([_line_reader()], config={"Lines": {"path": missing}})
    with pytest.raises(jit.NotFound) as info:
        plugins.run(trace)
    err = info.value
    assert missing in err.message
    assert isinstance(err, jit.Error)
    assert err.file == __file__ or err.file is not None
    assert err.line is not None and err.col is not None


def test_a_missing_file_is_caught_by_except_not_found(trace, files):
    @jit.plugin
    class Tolerant:
        config: PathCfg
        found: int = 0
        missing: int = 0

        @jit.each_event
        def step(self, e):
            try:
                with jit.open(self.config.path) as f:
                    for line in f:
                        self.found += 1
            except jit.NotFound:
                self.missing += 1

    gone = os.path.join(files["dir"], "gone.txt")
    results = Plugins([Tolerant], config={"Tolerant": {"path": gone}}).run(trace).results
    assert _total(results, "missing") == _EVENTS
    assert _total(results, "found") == 0
    results = Plugins([Tolerant], config={"Tolerant": {"path": files["plain"]}}).run(trace).results
    assert _total(results, "found") == 3 * _EVENTS
    assert _total(results, "missing") == 0


def test_the_file_closes_when_the_body_finishes(trace, files):
    plugins = Plugins([_line_reader()], config={"Lines": {"path": files["plain"]}})
    plugins.run(trace)
    stats = plugin_stats(jit.compile_class(_line_reader()))
    assert stats["opened"] == _EVENTS
    assert stats["closed"] == _EVENTS


def test_the_file_closes_when_the_body_raises(trace, files):
    @jit.plugin
    class Raises:
        config: PathCfg
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            with jit.open(self.config.path) as f:
                for line in f:
                    if line == "beta":
                        raise jit.Fatal("found beta")

    plugins = Plugins([Raises], config={"Raises": {"path": files["plain"]}})
    with pytest.raises(jit.Fatal, match="found beta"):
        plugins.run(trace)
    stats = plugin_stats(jit.compile_class(Raises))
    assert stats["opened"] >= 1
    assert stats["opened"] == stats["closed"]


def test_skip_rest_closes_the_file_too(trace, files):
    @jit.plugin
    class Skips:
        config: PathCfg
        n: int = 0

        @jit.each_event
        def step(self, e):
            with jit.open(self.config.path) as f:
                for line in f:
                    self.n += 1
                    raise jit.SkipRest()

    plugins = Plugins([Skips], config={"Skips": {"path": files["plain"]}})
    results = plugins.run(trace).results
    assert _total(results, "n") == 1
    stats = plugin_stats(jit.compile_class(Skips))
    assert stats["opened"] == stats["closed"] == 1


def test_nested_files_close_when_the_inner_body_raises(trace, files):
    @jit.plugin
    class Nested:
        config: PathCfg
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            with jit.open(self.config.path) as outer:
                for a in outer:
                    with jit.open(self.config.path) as inner:
                        for b in inner:
                            raise jit.Fatal("inner")

    plugins = Plugins([Nested], config={"Nested": {"path": files["plain"]}})
    with pytest.raises(jit.Fatal, match="inner"):
        plugins.run(trace)
    stats = plugin_stats(jit.compile_class(Nested))
    assert stats["opened"] == 2 and stats["closed"] == 2


def test_a_catch_inside_the_with_keeps_the_file_open_until_the_block_ends(trace, files):
    @jit.plugin
    class Inside:
        config: PathCfg
        lines: int = 0
        bad: int = 0

        @jit.each_event
        def step(self, e):
            with jit.open(self.config.path) as f:
                for line in f:
                    self.lines += 1
                    try:
                        with jit.open("/nonexistent/dftu-inner.txt") as g:
                            for other in g:
                                self.lines += 100
                    except jit.NotFound:
                        self.bad += 1

    plugins = Plugins([Inside], config={"Inside": {"path": files["plain"]}})
    results = plugins.run(trace).results
    assert _total(results, "lines") == 3 * _EVENTS
    assert _total(results, "bad") == 3 * _EVENTS
    stats = plugin_stats(jit.compile_class(Inside))
    assert stats["opened"] == stats["closed"] == _EVENTS


# ---- compile errors --------------------------------------------------------


def _err(source):
    with tempfile.TemporaryDirectory() as tmp:
        with pytest.raises(jit.JitError) as info:
            load_module(tmp, "bad_file", source)
    assert_located(info.value)
    return info.value


_HEAD = """
from dftracer.utils import jit

@jit.plugin
class Bad:
    n: int = 0

    @jit.each_event
    def step(self, e):
"""


def test_the_builtin_open_is_a_compile_error():
    err = _err(
        _HEAD
        + """
        with open("/tmp/x") as f:
            self.n += 1
"""
    )
    assert "with statement" in err.reason
    assert err.source is not None and "with open" in err.source


def test_the_path_is_a_literal_or_a_config_string():
    err = _err(
        _HEAD
        + """
        with jit.open(e.name) as f:
            self.n += 1
"""
    )
    assert "string literal or a str config field" in err.reason


def test_a_file_read_by_lines_and_by_size_is_a_compile_error():
    err = _err(
        _HEAD
        + """
        with jit.open("/tmp/x") as f:
            for line in f:
                self.n += 1
            c = f.read(4)
"""
    )
    assert "not both" in err.reason


def test_read_takes_a_positive_literal():
    err = _err(
        _HEAD
        + """
        with jit.open("/tmp/x") as f:
            c = f.read(self.n)
"""
    )
    assert "positive integer literal" in err.reason


def test_a_file_cannot_be_used_as_a_value_or_with_other_methods():
    err = _err(
        _HEAD
        + """
        with jit.open("/tmp/x") as f:
            x = f
"""
    )
    assert "is a file" in err.reason
    err = _err(
        _HEAD
        + """
        with jit.open("/tmp/x") as f:
            f.seek(3)
"""
    )
    assert "file method .seek" in err.reason


def test_a_for_loop_over_anything_else_stays_a_compile_error():
    err = _err(
        _HEAD
        + """
        for _ in range(2):
            self.n += 1
"""
    )
    assert "for loop" in err.reason


def test_open_must_be_bound_and_used_in_with():
    err = _err(
        _HEAD
        + """
        f = jit.open("/tmp/x")
"""
    )
    assert "with jit.open" in err.reason
    err = _err(
        _HEAD
        + """
        with jit.open("/tmp/x"):
            self.n += 1
"""
    )
    assert "bind the file" in err.reason
