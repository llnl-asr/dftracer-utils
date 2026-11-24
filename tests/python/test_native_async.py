# SPDX-License-Identifier: MIT
"""Tests for the native async handle returned by build_async and collect_async."""

import asyncio
import gc
import os
import subprocess
import sys
import threading
import time

import pytest

from dftracer.utils import DataFrame, col
from dftracer.utils import dftracer_utils_ext as _ext
from dftracer.utils.index import Indexer, IndexStatus
from dftracer.utils.runtime import Runtime, TaskHandle

from .common import Environment

pytest.importorskip("pyarrow")

_HAS_LAZY = hasattr(_ext, "LazyFrame")
lazy_only = pytest.mark.skipif(not _HAS_LAZY, reason="extension built without Arrow support")


def _big_frame(rows: int) -> DataFrame:
    return DataFrame.from_dict({"a": list(range(rows, 0, -1)), "b": list(range(rows))})


def _os_thread_count() -> int:
    """Threads of this process, native ones included; threading.active_count()
    sees only Python threads."""
    if os.path.exists("/proc/self/status"):
        with open("/proc/self/status") as status:
            for line in status:
                if line.startswith("Threads:"):
                    return int(line.split()[1])
    out = subprocess.run(
        ["ps", "-M", "-p", str(os.getpid())], capture_output=True, text=True, check=False
    )
    if out.returncode != 0:
        pytest.skip("cannot count process threads")
    return len(out.stdout.strip().splitlines()) - 1


def _slow_lazy():
    """A resident plan of many sorts, long enough for a cancel to land."""
    lf = _big_frame(1_000_000).lazy()
    for _ in range(40):
        lf = lf.sort_by("a").sort_by("b")
    return lf


def _wait_done(handle, timeout: float = 60.0) -> None:
    deadline = time.monotonic() + timeout
    while not handle.done():
        assert time.monotonic() < deadline, "task did not finish"
        time.sleep(0.001)


class TestBuildAsync:
    def test_returns_handle_and_matches_build(self):
        with Environment() as env:
            gz = env.create_test_gzip_file()
            with Indexer(files=[gz], index_dir=os.path.join(env.temp_dir, "idx_async")) as ix:
                handle = ix.build_async()
                assert isinstance(handle, TaskHandle)
                async_status = handle.get()
                assert isinstance(async_status, IndexStatus)
                assert ix.status().needs_work == []
            with Indexer(files=[gz], index_dir=os.path.join(env.temp_dir, "idx_sync")) as ix:
                sync_status = ix.build()
                assert ix.status().ready == [gz]
            assert async_status.ready == sync_status.ready == [gz]
            assert async_status.needs_work == sync_status.needs_work == []

    def test_force_build_async_rebuilds_and_returns_status(self):
        with Environment() as env:
            gz = env.create_test_gzip_file()
            with Indexer(files=[gz], index_dir=os.path.join(env.temp_dir, "idx_force")) as ix:
                ix.ensure()
                status = ix.build_async(force=True).get()
                assert isinstance(status, IndexStatus)
                assert status.ready == [gz]

    def test_get_twice_is_safe(self):
        with Environment() as env:
            gz = env.create_test_gzip_file()
            with Indexer(files=[gz]) as ix:
                handle = ix.build_async()
                assert isinstance(handle.get(), IndexStatus)
                assert isinstance(handle.get(), IndexStatus)
                handle.wait()
                assert handle.done() is True

    def test_error_matches_sync_build(self):
        ix = Indexer(files=["/nonexistent/x.pfw.gz"])
        with pytest.raises(Exception) as sync_info:
            ix.build()
        handle = ix.build_async()
        with pytest.raises(type(sync_info.value)) as async_info:
            handle.get()
        assert str(async_info.value) == str(sync_info.value)
        with pytest.raises(type(sync_info.value)):
            handle.get()

    def test_dropping_handle_before_completion_is_safe(self):
        with Environment(lines=20000) as env:
            gz = env.create_test_gzip_file()
            ix = Indexer(files=[gz])
            ix.build_async()
            gc.collect()
            ix.build()
            assert ix.status().needs_work == []

    def test_polled_done_turns_true(self):
        with Environment() as env:
            gz = env.create_test_gzip_file()
            handle = Indexer(files=[gz]).build_async()
            _wait_done(handle)
            assert handle.done() is True
            assert isinstance(handle.get(), IndexStatus)


class TestCheckpointBuildAsync:
    def test_native_handle_matches_build(self):
        with Environment() as env:
            gz = env.create_test_gzip_file()
            native = _ext.CheckpointIndexer(gz)
            handle = native.build_async()
            assert handle.get() is None
            assert native.exists()

    def test_native_error_matches_sync_build(self):
        with Environment() as env:
            gz = env.create_test_gzip_file()
            native = _ext.CheckpointIndexer(gz)
            os.remove(gz)
            with pytest.raises(Exception) as sync_info:
                native.build()
            with pytest.raises(type(sync_info.value)) as async_info:
                native.build_async().get()
            assert str(async_info.value) == str(sync_info.value)

    def test_runtime_submit_wraps_native_handle(self):
        with Environment() as env:
            gz = env.create_test_gzip_file()
            native = _ext.CheckpointIndexer(gz).build_async()
            with Runtime(threads=2, python_threads=2) as rt:
                handle = rt.submit(native)
                assert isinstance(handle, TaskHandle)
                assert handle.get() is None
                assert handle.done() is True


@lazy_only
class TestCollectAsync:
    def test_matches_collect(self):
        lf = _big_frame(1000).lazy().filter(col("a") > 10).sort_by("a")
        handle = lf.collect_async()
        assert isinstance(handle, TaskHandle)
        assert handle.get().to_arrow().equals(lf.collect().to_arrow())

    def test_batch_size_argument(self):
        lf = _big_frame(100).lazy()
        assert lf.collect_async(batch_size=7).get().to_arrow().equals(lf.collect().to_arrow())

    def test_get_twice_is_safe(self):
        handle = _big_frame(100).lazy().collect_async()
        first = handle.get()
        second = handle.get()
        assert first.to_arrow().equals(second.to_arrow())

    def test_error_matches_sync_collect(self):
        lf = DataFrame.from_dict({"s": ["foo", "bar"]}).lazy().with_columns(o=col("s").rindex("o"))
        with pytest.raises(Exception, match="not found in every row") as sync_info:
            lf.collect()
        with pytest.raises(type(sync_info.value)) as async_info:
            lf.collect_async().get()
        assert str(async_info.value) == str(sync_info.value)

    def test_returns_before_the_work_ends(self):
        lf = _big_frame(2_000_000).lazy().sort_by("a").sort_by("b")
        handle = lf.collect_async()
        assert handle.done() is False
        _wait_done(handle)
        assert handle.get().num_rows == 2_000_000

    def test_dropping_handle_before_completion_is_safe(self):
        lf = _big_frame(500_000).lazy().sort_by("a")
        lf.collect_async()
        del lf
        gc.collect()
        assert _big_frame(10).lazy().collect_async().get().num_rows == 10

    def test_gil_is_released_while_waiting(self):
        lf = _big_frame(3_000_000).lazy().sort_by("a").sort_by("b")
        counter = [0]
        stop = threading.Event()

        def spin():
            while not stop.is_set():
                counter[0] += 1

        thread = threading.Thread(target=spin)
        thread.start()
        try:
            while counter[0] == 0:
                time.sleep(0.001)
            handle = lf.collect_async()
            before = counter[0]
            handle.get()
            after = counter[0]
        finally:
            stop.set()
            thread.join()
        assert after > before


class TestCancel:
    def test_cancel_build_async_gives_cancelled_error(self):
        with Environment(lines=300_000) as env:
            gz = env.create_test_gzip_file()
            handle = Indexer(files=[gz]).build_async()
            handle.cancel()
            with pytest.raises(Exception, match="cancel"):
                handle.get()

    @lazy_only
    def test_cancel_collect_async_gives_cancelled_error(self):
        handle = _slow_lazy().collect_async()
        handle.cancel()
        with pytest.raises(Exception, match="cancel"):
            handle.get()

    @lazy_only
    def test_cancel_after_the_end_keeps_the_result(self):
        handle = _big_frame(10).lazy().collect_async()
        handle.get()
        handle.cancel()
        assert handle.get().num_rows == 10


def _run_script(body: str, timeout: float = 120.0):
    proc = subprocess.run(
        [sys.executable, "-I", "-c", _SCRIPT_HEAD + body],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    assert "Fatal Python error" not in proc.stderr, proc.stderr
    assert proc.returncode == 0, proc.stderr
    return proc


_SCRIPT_HEAD = """
import asyncio, threading, time
from dftracer.utils import DataFrame

def slow():
    lf = DataFrame.from_dict({"a": list(range(1_000_000, 0, -1)), "b": list(range(1_000_000))}).lazy()
    for _ in range(40):
        lf = lf.sort_by("a").sort_by("b")
    return lf
"""


@lazy_only
class TestExitGate:
    def test_exit_with_a_pending_callback(self):
        _run_script(
            """
h = slow().collect_async()
h.add_done_callback(lambda handle: print("fired"))
"""
        )

    def test_exit_while_a_callback_runs(self):
        proc = _run_script(
            """
started = threading.Event()
h = DataFrame.from_dict({"a": [1]}).lazy().collect_async()

def slow_callback(handle):
    started.set()
    time.sleep(1.0)
    print("callback-finished")

h.add_done_callback(slow_callback)
started.wait(30)
"""
        )
        assert "callback-finished" in proc.stdout

    def test_exit_with_a_pending_await(self):
        _run_script(
            """
async def waiter(h):
    return await h

h = slow().collect_async()
loop = asyncio.new_event_loop()
task = loop.create_task(waiter(h))
loop.run_until_complete(asyncio.sleep(0.05))
"""
        )


@lazy_only
class TestDoneCallback:
    def test_fires_once_with_the_handle(self):
        handle = _big_frame(300_000).lazy().sort_by("a").collect_async()
        seen = []
        fired = threading.Event()

        def on_done(h):
            seen.append(h)
            fired.set()

        handle.add_done_callback(on_done)
        assert fired.wait(30)
        handle.get()
        time.sleep(0.05)
        assert seen == [handle]

    def test_fires_at_once_for_a_done_handle(self):
        handle = _big_frame(10).lazy().collect_async()
        handle.get()
        seen = []
        handle.add_done_callback(seen.append)
        assert seen == [handle]

    def test_native_callback_gets_the_native_handle(self):
        with Environment() as env:
            native = _ext.CheckpointIndexer(env.create_test_gzip_file()).build_async()
            seen = []
            fired = threading.Event()
            native.add_done_callback(lambda h: (seen.append(h), fired.set()))
            assert fired.wait(30)
            assert seen == [native]

    @pytest.mark.filterwarnings("ignore::pytest.PytestUnraisableExceptionWarning")
    def test_exception_in_callback_does_not_propagate(self):
        handle = _big_frame(10).lazy().collect_async()
        handle.get()
        handle.add_done_callback(lambda h: 1 / 0)
        assert handle.get().num_rows == 10


@lazy_only
class TestAwait:
    def test_await_matches_collect(self):
        lf = _big_frame(1000).lazy().filter(col("a") > 10).sort_by("a")

        async def run():
            return await lf.collect_async()

        assert asyncio.run(run()).to_arrow().equals(lf.collect().to_arrow())

    def test_gather(self):
        a = _big_frame(1000).lazy().sort_by("a")
        b = _big_frame(500).lazy().sort_by("b")

        async def run():
            return await asyncio.gather(a.collect_async(), b.collect_async())

        first, second = asyncio.run(run())
        assert first.to_arrow().equals(a.collect().to_arrow())
        assert second.to_arrow().equals(b.collect().to_arrow())

    def test_many_awaits_start_no_threads(self):
        # The pool may grow under 200 queued tasks; awaiting them must add no
        # thread of its own beyond that growth.
        frame = _big_frame(500_000).lazy().sort_by("a").sort_by("b")
        frame.collect_async().get()
        before = _os_thread_count()
        plain = [frame.collect_async() for _ in range(200)]
        time.sleep(0.05)
        pool_growth = _os_thread_count() - before
        for handle in plain:
            handle.get()
        before = _os_thread_count()
        peak = [before]

        async def run():
            gathered = asyncio.gather(*[frame.collect_async() for _ in range(200)])
            await asyncio.sleep(0)
            peak[0] = _os_thread_count()
            return await gathered

        assert len(asyncio.run(run())) == 200
        assert peak[0] - before <= max(pool_growth, 0) + 8

    def test_group_by_fork_join_adds_few_threads(self):
        # Every kernel forks through the runtime, so a worker that waited for
        # its fork without running a chunk made the pool add a thread for it.
        rows = 20_000
        frame = DataFrame.from_dict({"k": [i % 7 for i in range(rows)], "v": list(range(rows))})
        lf = frame.lazy().group_by("k", "count", "sum:v").sort_by("k")
        lf.collect_async().get()
        before = _os_thread_count()
        handles = [lf.collect_async() for _ in range(200)]
        peak = _os_thread_count()
        for handle in handles:
            handle.get()
        assert peak - before <= 8

    def test_await_done_handle(self):
        handle = _big_frame(10).lazy().collect_async()
        handle.get()

        async def run():
            return await handle

        assert asyncio.run(run()).num_rows == 10

    def test_await_error_matches_sync(self):
        lf = DataFrame.from_dict({"s": ["foo", "bar"]}).lazy().with_columns(o=col("s").rindex("o"))
        with pytest.raises(Exception, match="not found in every row") as sync_info:
            lf.collect()

        async def run():
            return await lf.collect_async()

        with pytest.raises(type(sync_info.value)) as async_info:
            asyncio.run(run())
        assert str(async_info.value) == str(sync_info.value)

    def test_cancelling_the_awaiter_stops_the_native_task(self):
        handle = _slow_lazy().collect_async()

        async def run():
            task = asyncio.ensure_future(_await(handle))
            await asyncio.sleep(0.01)
            task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await task

        asyncio.run(run())
        _wait_done(handle)
        with pytest.raises(Exception, match="cancel"):
            handle.get()

    def test_loop_closed_before_completion(self):
        handle = _big_frame(2_000_000).lazy().sort_by("a").sort_by("b").collect_async()
        loop = asyncio.new_event_loop()

        async def start():
            return handle.__await__().send(None)

        loop.run_until_complete(start())
        loop.close()
        _wait_done(handle)
        time.sleep(0.05)
        assert handle.get().num_rows == 2_000_000


async def _await(handle):
    return await handle
