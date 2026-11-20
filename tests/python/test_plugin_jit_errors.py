# SPDX-License-Identifier: MIT
"""Exceptions in a compiled body: ``raise jit.Fatal / SkipRest / Cancelled`` lower
to the status returns of the ABI with the message copied, and ``try`` / ``except``
over a call that can fail lowers to a status check."""

import gzip
import os
import shutil

import pytest

from dftracer.utils import Runtime, jit
from dftracer.utils.plugins import Plugins

from .jit_common import assert_located, compile_error

pa = pytest.importorskip("pyarrow")

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)

_FILES = 3
_PER_FILE = 40


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("err_trace")
    for f in range(_FILES):
        with gzip.open(os.path.join(d, f"t{f}.pfw.gz"), "wt", encoding="utf-8") as fh:
            for i in range(_PER_FILE):
                n = f * _PER_FILE + i
                fh.write(
                    f'{{"name":"read","cat":"POSIX","pid":{1 + n % 4},"tid":1,"ts":{1000 + n},'
                    f'"dur":{10 + n},"ph":1,"args":{{}}}}\n'
                )
    return str(d)


def _run(cls, trace, threads=1):
    with Runtime(threads=threads) as rt:
        return Plugins([cls], runtime=rt).run(trace).results


def _total(results, name):
    cols = pa.table(results[name]).to_pydict()
    return sum(cols["value"]) if cols["value"] else 0


def test_fatal_reaches_the_caller_with_message_and_location(trace):
    @jit.plugin
    class Stops:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            if e.dur > 100:
                raise jit.Fatal("duration over the limit")  # RAISE_LINE

    with pytest.raises(jit.Fatal) as info:
        _run(Stops, trace)
    err = info.value
    assert err.message == "duration over the limit"
    assert err.file == __file__
    with open(__file__, encoding="utf-8") as fh:
        want = next(i for i, ln in enumerate(fh, 1) if "# RAISE_LINE" in ln)
    assert err.line == want
    assert err.col == 17
    assert f"{__file__}:{want}:17: duration over the limit" == str(err)
    assert not isinstance(err, jit.Error)


def test_cancelled_reaches_the_caller_as_a_base_exception(trace):
    @jit.plugin
    class Gives:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            if e.dur > 100:
                raise jit.Cancelled("the user gave up")

    with pytest.raises(jit.Cancelled) as info:
        _run(Gives, trace)
    assert info.value.message == "the user gave up"
    assert isinstance(info.value, BaseException) and not isinstance(info.value, Exception)


def test_cancelled_without_a_message_uses_its_name(trace):
    @jit.plugin
    class Gives:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            raise jit.Cancelled()

    with pytest.raises(jit.Cancelled) as info:
        _run(Gives, trace)
    assert info.value.message == "Cancelled"


def test_skip_rest_stops_the_batch_and_keeps_the_work_so_far(trace):
    @jit.plugin
    class Skips:
        seen: int = 0

        @jit.each_event
        def step(self, e):
            if e.dur > 40:
                raise jit.SkipRest()
            self.seen += 1

    # Each file is one batch; events of one file arrive in ts order, so a file
    # counts its events up to the first one over 40.
    results = _run(Skips, trace)
    got = _total(results, "seen")
    want = 0
    for f in range(_FILES):
        for i in range(_PER_FILE):
            if 10 + f * _PER_FILE + i > 40:
                break
            want += 1
    assert got == want
    assert 0 < got < _FILES * _PER_FILE


def test_skip_rest_never_reaches_the_caller(trace):
    @jit.plugin
    class Skips:
        seen: int = 0

        @jit.each_event
        def step(self, e):
            self.seen += 1
            raise jit.SkipRest("enough")

    _run(Skips, trace)


def test_raise_in_a_nested_branch_and_in_the_mask_lowering(trace):
    @jit.plugin
    class Nested:
        n: int = 0

        @jit.each_event
        def step(self, e):
            if e.pid == 2:
                if e.dur > 60:
                    raise jit.Fatal("pid 2 too slow")
                self.n += 1

    assert Nested._jit_plugin.lowering == "mask"
    with pytest.raises(jit.Fatal, match="pid 2 too slow"):
        _run(Nested, trace)


def test_a_failed_run_does_not_keep_the_batch_it_aborted(trace):
    @jit.plugin
    class Never:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            if e.dur > 1000000:
                raise jit.Fatal("never")

    assert _total(_run(Never, trace), "n") == _FILES * _PER_FILE


def test_each_raise_lowers_to_its_status():
    @jit.plugin
    class Kinds:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1
            if e.dur > 1:
                raise jit.Fatal("a")
            if e.dur > 2:
                raise jit.SkipRest()
            if e.dur > 3:
                raise jit.Cancelled()

    text = Kinds._jit_plugin.source
    assert "DFTU_E_UNKNOWN" in text and "DFTU_E_CANCELLED" in text
    assert "jit.Fatal [" in text and ']: a"' in text


# ---- compile errors --------------------------------------------------------


def test_raise_of_a_python_class_is_a_compile_error_with_the_location():
    with pytest.raises(jit.JitError) as info:

        @jit.plugin
        class Bad:
            n: int = 0

            @jit.each_event
            def step(self, e):
                if e.dur > 1:
                    raise ValueError("nope")

    err = info.value
    assert "ValueError" in err.reason or "raise of" in err.reason
    assert err.file == __file__ and err.line is not None and err.col is not None
    assert err.source is not None and "raise ValueError" in err.source


def test_raise_of_a_jit_error_is_a_compile_error():
    with compile_error("jit.NotFound.*only jit.Fatal"):

        @jit.plugin
        class Bad:
            n: int = 0

            @jit.each_event
            def step(self, e):
                raise jit.NotFound("x")


def test_bare_raise_and_raise_from_are_compile_errors():
    with compile_error("bare `raise`"):

        @jit.plugin
        class Bare:
            n: int = 0

            @jit.each_event
            def step(self, e):
                raise

    with compile_error("from"):

        @jit.plugin
        class From:
            n: int = 0

            @jit.each_event
            def step(self, e):
                raise jit.Fatal("a") from None


def test_a_message_must_be_a_string_literal():
    with compile_error("string literal"):

        @jit.plugin
        class Bad:
            n: int = 0

            @jit.each_event
            def step(self, e):
                raise jit.Fatal(e.name)


# ---- try / except over a call that can fail --------------------------------


@pytest.fixture()
def one_trace(tmp_path):
    d = tmp_path / "trace"
    d.mkdir()
    with gzip.open(d / "t.pfw.gz", "wt", encoding="utf-8") as fh:
        for i in range(12):
            fh.write(
                f'{{"name":"read","cat":"POSIX","pid":{1 + i % 3},"tid":1,"ts":{1000 + i},'
                f'"dur":{10 + i},"ph":1,"args":{{}}}}\n'
            )
    return str(d)


def _store_for(cls, idx, values):
    """Write ``values`` ({pid: value}) into the ``runs`` namespace of ``cls``."""
    from pathlib import Path

    from dftracer.utils.store import Store as PyStore

    name = Path(jit.compile_class(cls)).stem
    store = PyStore.open(os.path.join(idx, ".dftindex", "store"))
    ns = store.namespace(f"plugin.{name}.runs", 1, durable=True, shared=True)
    with store.batch(ns, None) as batch:
        for key, value in values.items():
            batch.put(key, value)
    store.flush()
    store.close()


def _touch(trace, idx):
    @jit.plugin
    class Touch:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    Plugins([Touch]).run(trace, index_dir=idx)


def test_except_catches_a_failed_store_read_and_the_scan_goes_on(one_trace, tmp_path):
    idx = str(tmp_path / "idx")
    _touch(one_trace, idx)

    @jit.plugin
    class Safe:
        runs: jit.Store[int, int]
        ok: int = 0
        bad: int = 0

        @jit.each_event
        def step(self, e):
            try:
                v = self.runs.get(e.pid, 0)  # noqa: F841
                self.ok += v + 1
            except jit.Error:
                self.bad += 1

    # pid 1 holds a float where an int is read (a failed get), pid 2 holds 5.
    _store_for(Safe, idx, {1: 2.5, 2: 5})
    results = Plugins([Safe]).run(one_trace, index_dir=idx).results
    assert _total(results, "bad") == 4
    assert _total(results, "ok") == 4 * (5 + 1) + 4 * (0 + 1)


def test_the_failed_statement_leaves_no_update_half_done(one_trace, tmp_path):
    idx = str(tmp_path / "idx")
    _touch(one_trace, idx)

    @jit.plugin
    class Half:
        runs: jit.Store[int, int]
        seen: int = 0
        bad: int = 0

        @jit.each_event
        def step(self, e):
            try:
                v = self.runs.get(e.pid, 0)  # noqa: F841
                self.seen += 1
            except jit.Error:
                self.bad += 1

    _store_for(Half, idx, {1: 2.5})
    results = Plugins([Half]).run(one_trace, index_dir=idx).results
    assert _total(results, "bad") == 4
    assert _total(results, "seen") == 8


def test_an_uncaught_failed_store_read_still_fails_the_run(one_trace, tmp_path):
    idx = str(tmp_path / "idx")
    _touch(one_trace, idx)

    @jit.plugin
    class Loud:
        runs: jit.Store[int, int]
        ok: int = 0

        @jit.each_event
        def step(self, e):
            try:
                v = self.runs.get(e.pid, 0)  # noqa: F841
                self.ok += v
            except jit.NotFound:
                self.ok += 1000

    _store_for(Loud, idx, {1: 2.5})
    with pytest.raises(Exception, match="a stored value is not an int"):
        Plugins([Loud]).run(one_trace, index_dir=idx)


def test_fatal_inside_a_try_is_not_caught_by_except_error(one_trace):
    @jit.plugin
    class Through:
        n: int = 0

        @jit.each_event
        def step(self, e):
            try:
                self.n += 1
                if e.dur > 12:
                    raise jit.Fatal("passes through")
            except jit.Error:
                self.n += 1000

    with pytest.raises(jit.Fatal, match="passes through"):
        Plugins([Through]).run(one_trace)


def test_skip_rest_inside_a_try_stops_the_batch(one_trace):
    @jit.plugin
    class Quiet:
        n: int = 0

        @jit.each_event
        def step(self, e):
            try:
                if e.dur > 13:
                    raise jit.SkipRest()
                self.n += 1
            except jit.Error:
                self.n += 1000

    assert _total(Plugins([Quiet]).run(one_trace).results, "n") == 4


def test_nested_handlers_run_in_order(one_trace, tmp_path):
    idx = str(tmp_path / "idx")
    _touch(one_trace, idx)

    @jit.plugin
    class Two:
        runs: jit.Store[int, int]
        io: int = 0
        other: int = 0

        @jit.each_event
        def step(self, e):
            try:
                v = self.runs.get(e.pid, 0)  # noqa: F841
            except jit.NotFound:
                self.io += 1
            except (jit.ParseError, jit.Error):
                self.other += 1

    _store_for(Two, idx, {1: 2.5})
    results = Plugins([Two]).run(one_trace, index_dir=idx).results
    assert _total(results, "other") == 4
    assert _total(results, "io") == 0


def _reject(body):
    with pytest.raises(jit.JitError) as info:
        body()
    assert_located(info.value)
    return info.value


def test_a_bare_except_is_a_compile_error_with_the_location():
    def build():
        @jit.plugin
        class Bad:
            n: int = 0

            @jit.each_event
            def step(self, e):
                try:
                    self.n += 1
                except:  # noqa: E722
                    self.n += 2

    err = _reject(build)
    assert "bare `except`" in err.reason
    assert err.file == __file__ and err.line and err.col
    assert err.source is not None and "except:" in err.source


def test_except_of_a_python_builtin_is_a_compile_error():
    def build():
        @jit.plugin
        class Bad:
            n: int = 0

            @jit.each_event
            def step(self, e):
                try:
                    self.n += 1
                except ValueError:
                    self.n += 2

    err = _reject(build)
    assert "Python class" in err.reason and "jit.Error" in err.reason
    assert err.source is not None and "ValueError" in err.source


@pytest.mark.parametrize(
    "name, why",
    [
        ("Fatal", "ends the run"),
        ("Cancelled", "must stop"),
        ("SkipRest", "not an error"),
    ],
)
def test_catching_a_run_ending_exception_is_a_compile_error(name, why):
    source = f"""
from dftracer.utils import jit

@jit.plugin
class Bad:
    n: int = 0

    @jit.each_event
    def step(self, e):
        try:
            self.n += 1
        except jit.{name}:
            self.n += 2
"""
    import tempfile

    from .jit_common import load_module

    with tempfile.TemporaryDirectory() as tmp:
        with compile_error(why):
            load_module(tmp, f"bad_catch_{name.lower()}", source)


def test_try_needs_an_except_and_takes_no_else_or_finally():
    with compile_error("else.*finally"):

        @jit.plugin
        class Bad:
            n: int = 0

            @jit.each_event
            def step(self, e):
                try:
                    self.n += 1
                except jit.Error:
                    self.n += 2
                finally:
                    self.n += 3


def test_except_as_name_is_a_compile_error():
    with compile_error("as name"):

        @jit.plugin
        class Bad:
            n: int = 0

            @jit.each_event
            def step(self, e):
                try:
                    self.n += 1
                except jit.Error as err:  # noqa: F841
                    self.n += 2


def test_a_store_get_inside_try_must_be_the_whole_assignment():
    with compile_error("whole right side"):

        @jit.plugin
        class Bad:
            runs: jit.Store[int, int]
            n: int = 0

            @jit.each_event
            def step(self, e):
                try:
                    self.n += self.runs.get(e.pid, 0)
                except jit.Error:
                    self.n += 1
