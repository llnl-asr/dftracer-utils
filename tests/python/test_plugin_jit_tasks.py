# SPDX-License-Identifier: MIT
"""``async def`` in a compiled plugin: a ``@jit.task`` method is a
``dftu_step_fn`` state machine started through the task service from the
``@jit.finalize`` body. Closed await set: ``jit.sleep``, ``jit.all``, ``jit.any``,
a task call and a store get."""

import gzip
import os
import shutil
import tempfile
import time
from pathlib import Path

import pytest

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins

from .jit_common import assert_located, load_module, plugin_stats

pytestmark = pytest.mark.skipif(
    not (shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")),
    reason="no C++ compiler available for the jit backend",
)


@pytest.fixture(scope="module")
def trace(tmp_path_factory):
    d = tmp_path_factory.mktemp("task_trace")
    with gzip.open(os.path.join(d, "t.pfw.gz"), "wt", encoding="utf-8") as fh:
        for i in range(12):
            fh.write(
                f'{{"name":"read","cat":"POSIX","pid":{1 + i % 3},"tid":1,"ts":{1000 + i},'
                f'"dur":{10 + i},"ph":1,"args":{{}}}}\n'
            )
    return str(d)


def _bytes(results):
    return {k: v for k, v in results.items() if isinstance(v, bytes)}


def test_a_task_sleeps_and_completes(trace):
    @jit.plugin
    class Slept:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def work(self, a: int, secs: float) -> int:
            await jit.sleep(secs)
            return a * 2 + 1

        @jit.finalize
        async def done(self) -> None:
            x = await self.work(5, 0.2)
            jit.emit("x", x)

    plugins = Plugins([Slept])
    start = time.monotonic()
    results = plugins.run(trace).results
    elapsed = time.monotonic() - start
    assert _bytes(results) == {"x": b"11"}
    assert elapsed >= 0.2
    stats = plugin_stats(jit.compile_class(Slept))
    assert stats["started"] == 2 and stats["released"] == 2 and stats["completed"] == 2


def test_all_gives_the_results_in_order_and_runs_the_tasks_at_once(trace):
    @jit.plugin
    class Fan:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def work(self, a: int, secs: float) -> int:
            await jit.sleep(secs)
            return a * 10

        @jit.task
        async def half(self, a: int) -> float:
            await jit.sleep(0.01)
            return a / 2

        @jit.finalize
        async def done(self) -> None:
            a, b, c, d = await jit.all(
                self.work(1, 0.5), self.work(2, 0.5), self.work(3, 0.5), self.work(4, 0.5)
            )
            jit.emit("a", a)
            jit.emit("b", b)
            jit.emit("c", c)
            jit.emit("d", d)
            p, q = await jit.all(self.half(3), self.work(5, 0.01))
            jit.emit("p", p)
            jit.emit("q", q)

    # built before the clock starts: the JIT compile time is no part of the run
    plugins = Plugins([Fan])
    start = time.monotonic()
    results = plugins.run(trace).results
    elapsed = time.monotonic() - start
    assert _bytes(results) == {
        "a": b"10",
        "b": b"20",
        "c": b"30",
        "d": b"40",
        "p": b"1.5",
        "q": b"50",
    }
    # Four tasks of 0.5 s one after the other would take two seconds.
    assert elapsed < 1.5


def test_tasks_that_run_at_once_make_their_jobs_at_once(trace):
    # Each task makes three sleep jobs, so the tasks of one jit.all make
    # jobs on many threads at the same time.
    @jit.plugin
    class Busy:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def work(self, a: int) -> int:
            await jit.sleep(0.0)
            await jit.sleep(0.0)
            await jit.sleep(0.0)
            return a

        @jit.finalize
        async def done(self) -> None:
            t = 0
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            t += a + b + c + d
            jit.emit("t", t)

    for _ in range(10):
        assert _bytes(Plugins([Busy]).run(trace).results) == {"t": b"300"}


def test_any_takes_the_first_and_cancels_the_rest_at_their_await(trace):
    @jit.plugin
    class Race:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def slow(self) -> int:
            await jit.sleep(60.0)
            raise jit.Fatal("ran after the cancel")

        @jit.task
        async def fast(self, v: int) -> int:
            await jit.sleep(0.01)
            return v

        @jit.finalize
        async def done(self) -> None:
            w = await jit.any(self.slow(), self.fast(7))
            jit.emit("winner", w)

    plugins = Plugins([Race])
    start = time.monotonic()
    results = plugins.run(trace).results
    assert time.monotonic() - start < 10
    assert _bytes(results) == {"winner": b"7"}
    stats = plugin_stats(jit.compile_class(Race))
    # finalize, slow and fast started; each released once; the cancelled one never completed.
    assert stats["started"] == 3
    assert stats["released"] == 3
    assert stats["completed"] == 2


def test_an_exception_in_a_task_reaches_the_caller_with_its_location(trace, tmp_path):
    module = load_module(
        tmp_path,
        "task_raises",
        """
        from dftracer.utils import jit

        @jit.plugin
        class Boom:
            n: int = 0

            @jit.each_event
            def step(self, e):
                self.n += 1

            @jit.task
            async def explode(self, v: int) -> int:
                await jit.sleep(0.01)
                if v > 2:
                    raise jit.Fatal("task exploded")  # RAISE_MARK
                return v

            @jit.finalize
            async def done(self) -> None:
                x = await self.explode(3)
                jit.emit("x", x)
        """,
    )
    with pytest.raises(jit.Fatal) as info:
        Plugins([module.Boom]).run(trace)
    err = info.value
    assert err.message == "task exploded"
    assert err.file == str(tmp_path / "task_raises.py")
    marked = next(
        i for i, ln in enumerate(Path(err.file).read_text().splitlines(), 1) if "RAISE_MARK" in ln
    )
    assert err.line == marked


def test_a_failing_task_in_all_cancels_its_siblings(trace):
    @jit.plugin
    class Cancels:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def forever(self) -> int:
            await jit.sleep(60.0)
            return 1

        @jit.task
        async def fails(self) -> int:
            await jit.sleep(0.01)
            raise jit.Fatal("one failed")

        @jit.finalize
        async def done(self) -> None:
            a, b = await jit.all(self.forever(), self.fails())
            jit.emit("a", a)

    start = time.monotonic()
    with pytest.raises(jit.Fatal, match="one failed"):
        Plugins([Cancels]).run(trace)
    assert time.monotonic() - start < 10


def test_a_task_failure_is_caught_by_except_in_the_awaiter(trace, tmp_path):
    module = load_module(
        tmp_path,
        "task_catch",
        """
        from dftracer.utils import jit

        @jit.plugin
        class Catch:
            n: int = 0

            @jit.each_event
            def step(self, e):
                self.n += 1

            @jit.task
            async def reads(self, a: int) -> int:
                with jit.open("/nonexistent/dftu-task-missing.txt") as f:
                    for line in f:
                        a += 1
                return a

            @jit.finalize
            async def done(self) -> None:
                try:
                    x = await self.reads(1)
                except jit.NotFound:
                    x = -1
                jit.emit("x", x)
        """,
    )
    assert _bytes(Plugins([module.Catch]).run(trace).results) == {"x": b"-1"}


def test_locals_live_across_an_await(trace):
    @jit.plugin
    class Across:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def mix(self, a: int, scale: float) -> float:
            total = a
            half = scale / 2
            tag = "kept"
            await jit.sleep(0.01)
            total += 3
            if total > 5:
                await jit.sleep(0.01)
                total = total * 2
            else:
                await jit.sleep(0.01)
            half += 0.25
            bonus = 100
            if tag == "kept":
                bonus = 1
            return total + half + bonus

        @jit.finalize
        async def done(self) -> None:
            base = 4
            note = "tag"
            r = await self.mix(base, 1.0)
            r2 = await self.mix(0, 2.0)
            jit.emit("r", r)
            jit.emit("r2", r2)
            jit.emit("base", base)
            jit.emit("note", note)

    got = _bytes(Plugins([Across]).run(trace).results)
    assert float(got["r"]) == (4 + 3) * 2 + 0.5 + 0.25 + 1
    assert float(got["r2"]) == 3 + 1.0 + 0.25 + 1
    assert got["base"] == b"4" and got["note"] == b"tag"


def test_skip_rest_ends_a_task_cleanly_with_its_result_so_far(trace):
    @jit.plugin
    class Skips:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def partial(self) -> int:
            raise jit.SkipRest()

        @jit.finalize
        async def done(self) -> None:
            x = await self.partial()
            jit.emit("x", x)

    assert _bytes(Plugins([Skips]).run(trace).results) == {"x": b"0"}


def test_cancelled_in_a_task_reaches_the_caller(trace):
    @jit.plugin
    class Gives:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def stop(self) -> int:
            await jit.sleep(0.01)
            raise jit.Cancelled("stopped in a task")

        @jit.finalize
        async def done(self) -> None:
            x = await self.stop()
            jit.emit("x", x)

    with pytest.raises(jit.Cancelled, match="stopped in a task"):
        Plugins([Gives]).run(trace)


def test_a_task_can_start_another_task(trace):
    @jit.plugin
    class Chain:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def inner(self, v: int) -> int:
            await jit.sleep(0.01)
            return v + 1

        @jit.task
        async def outer(self, v: int) -> int:
            a, b = await jit.all(self.inner(v), self.inner(v + 10))
            return a + b

        @jit.finalize
        async def done(self) -> None:
            x = await self.outer(1)
            jit.emit("x", x)

    assert _bytes(Plugins([Chain]).run(trace).results) == {"x": b"14"}


def test_a_sync_finalize_emits_and_uses_the_config(trace):
    from dataclasses import dataclass

    @dataclass(frozen=True)
    class Cfg:
        label: str = "tag"
        k: int = 4

    @jit.plugin
    class Sync:
        config: Cfg
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        def done(self):
            jit.emit("label", self.config.label)
            jit.emit("k", self.config.k * 2)

    got = _bytes(Plugins([Sync], config={"Sync": {"label": "x1", "k": 5}}).run(trace).results)
    assert got == {"label": b"x1", "k": b"10"}


def test_a_store_get_is_awaited_in_finalize(trace, tmp_path):
    from dftracer.utils.store import Store as PyStore

    idx = str(tmp_path / "idx")

    @jit.plugin
    class Touch:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    Plugins([Touch]).run(trace, index_dir=idx)

    @jit.plugin
    class Reads:
        runs: jit.Store[int, int]
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.finalize
        async def done(self) -> None:
            a = await self.runs.get(1, 0)
            b = await self.runs.get(2, 5)
            jit.emit("a", a)
            jit.emit("b", b)

    name = Path(jit.compile_class(Reads)).stem
    store = PyStore.open(os.path.join(idx, ".dftindex", "store"))
    ns = store.namespace(f"plugin.{name}.runs", 1, durable=True, shared=True)
    with store.batch(ns, None) as batch:
        batch.put(1, 42)
    store.flush()
    store.close()
    assert _bytes(Plugins([Reads]).run(trace, index_dir=idx).results) == {"a": b"42", "b": b"5"}


def test_a_store_get_after_tasks_that_ran_at_once(trace, tmp_path):
    from dftracer.utils.store import Store as PyStore

    idx = str(tmp_path / "idx")

    @jit.plugin
    class Touch:
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

    Plugins([Touch]).run(trace, index_dir=idx)

    @jit.plugin
    class Mixed:
        runs: jit.Store[int, int]
        n: int = 0

        @jit.each_event
        def step(self, e):
            self.n += 1

        @jit.task
        async def work(self, a: int) -> int:
            await jit.sleep(0.0)
            await jit.sleep(0.0)
            return a

        @jit.finalize
        async def done(self) -> None:
            a, b, c, d = await jit.all(self.work(1), self.work(2), self.work(3), self.work(4))
            v = await self.runs.get(1, 0)
            jit.emit("t", a + b + c + d + v)

    name = Path(jit.compile_class(Mixed)).stem
    store = PyStore.open(os.path.join(idx, ".dftindex", "store"))
    ns = store.namespace(f"plugin.{name}.runs", 1, durable=True, shared=True)
    with store.batch(ns, None) as batch:
        batch.put(1, 32)
    store.flush()
    store.close()
    for _ in range(5):
        assert _bytes(Plugins([Mixed]).run(trace, index_dir=idx).results) == {"t": b"42"}


# ---- compile errors --------------------------------------------------------


def _err(source):
    with tempfile.TemporaryDirectory() as tmp:
        with pytest.raises(jit.JitError) as info:
            load_module(tmp, "bad_flow", source)
    assert_located(info.value)
    return info.value


_HEAD = """
from dftracer.utils import jit

@jit.plugin
class Bad:
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1
"""


def test_await_in_a_condition_is_a_compile_error_with_the_location():
    err = _err(
        _HEAD
        + """
    @jit.finalize
    async def done(self):
        if await jit.sleep(1.0):
            jit.emit("x", 1)
"""
    )
    assert "statement of its own" in err.reason
    assert err.line is not None and err.col is not None
    assert err.source is not None and "await jit.sleep" in err.source


def test_await_inside_a_loop_body_is_a_compile_error():
    err = _err(
        _HEAD
        + """
    @jit.finalize
    async def done(self):
        with jit.open("/tmp/x") as f:
            for line in f:
                await jit.sleep(1.0)
"""
    )
    assert "await inside a" in err.reason


def test_await_of_something_outside_the_closed_set_is_a_compile_error():
    err = _err(
        _HEAD
        + """
    @jit.finalize
    async def done(self):
        x = await jit.gather(1)
"""
    )
    assert (
        "awaitables are jit.sleep" in err.reason
        or "unknown" in err.reason
        or "closed" in err.reason
    )


def test_a_receive_on_a_name_that_is_not_a_declared_port_is_a_compile_error():
    err = _err(
        _HEAD
        + """
    @jit.finalize
    async def done(self):
        x = await self.chan.recv()
"""
    )
    assert "channel send or receive" in err.reason


def test_a_task_call_that_is_not_awaited_is_a_compile_error():
    err = _err(
        _HEAD
        + """
    @jit.task
    async def work(self, a: int) -> int:
        return a

    @jit.finalize
    async def done(self):
        self.work(1)
"""
    )
    assert "must be awaited" in err.reason


def test_a_task_started_from_each_event_is_a_compile_error():
    err = _err(
        """
from dftracer.utils import jit

@jit.plugin
class Bad:
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1
        self.work(1)

    @jit.task
    async def work(self, a: int) -> int:
        return a

    @jit.finalize
    async def done(self):
        x = await self.work(1)
"""
    )
    assert "started from the @jit.finalize body" in err.reason


def test_sleep_inside_all_is_a_compile_error():
    err = _err(
        _HEAD
        + """
    @jit.task
    async def work(self) -> int:
        return 1

    @jit.finalize
    async def done(self):
        a, b = await jit.all(self.work(), jit.sleep(1.0))
"""
    )
    assert "jit.sleep inside jit.all or jit.any" in err.reason


def test_any_needs_one_return_type():
    err = _err(
        _HEAD
        + """
    @jit.task
    async def one(self) -> int:
        return 1

    @jit.task
    async def two(self) -> float:
        return 1.5

    @jit.finalize
    async def done(self):
        x = await jit.any(self.one(), self.two())
"""
    )
    assert "share one return type" in err.reason


def test_a_task_parameter_must_be_annotated():
    err = _err(
        _HEAD
        + """
    @jit.task
    async def work(self, a) -> int:
        return 1

    @jit.finalize
    async def done(self):
        x = await self.work(1)
"""
    )
    assert "annotated `int` or `float`" in err.reason


def test_a_task_must_be_async_and_a_task_needs_a_finalize():
    err = _err(
        _HEAD
        + """
    @jit.task
    def work(self, a: int) -> int:
        return 1

    @jit.finalize
    async def done(self):
        pass
"""
    )
    assert "async def" in err.reason
    err = _err(
        _HEAD
        + """
    @jit.task
    async def work(self, a: int) -> int:
        return 1
"""
    )
    assert "none" in err.reason


def test_a_task_cannot_read_the_store():
    err = _err(
        """
from dftracer.utils import jit

@jit.plugin
class Bad:
    runs: jit.Store[int, int]
    n: int = 0

    @jit.each_event
    def step(self, e):
        self.n += 1

    @jit.task
    async def work(self) -> int:
        v = await self.runs.get(1, 0)
        return v

    @jit.finalize
    async def done(self):
        x = await self.work()
"""
    )
    assert "cannot use the store" in err.reason


def test_a_task_returning_the_wrong_type_is_a_compile_error():
    err = _err(
        _HEAD
        + """
    @jit.task
    async def work(self) -> int:
        return 1.5

    @jit.finalize
    async def done(self):
        x = await self.work()
"""
    )
    assert "returns an int" in err.reason


def test_emit_outside_finalize_and_emit_twice_are_compile_errors():
    err = _err(
        _HEAD
        + """
    @jit.task
    async def work(self) -> int:
        jit.emit("x", 1)
        return 1

    @jit.finalize
    async def done(self):
        x = await self.work()
"""
    )
    assert "statement of a @jit.finalize" in err.reason
    err = _err(
        _HEAD
        + """
    @jit.finalize
    def done(self):
        jit.emit("x", 1)
        jit.emit("x", 2)
"""
    )
    assert "twice" in err.reason


def test_the_markers_cannot_run_in_python():
    with pytest.raises(jit.JitError, match="plugin construct"):
        jit.sleep(1.0)
    with pytest.raises(jit.JitError, match="plugin construct"):
        jit.json("{}")
    with pytest.raises(jit.JitError, match="plugin construct"):
        jit.open("x")
