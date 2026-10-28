#!/usr/bin/env python3
"""Test cases for Runtime Python bindings."""

import pytest

import dftracer.utils as dftu_utils


class TestRuntimeCreation:
    def test_default_threads(self):
        rt = dftu_utils.Runtime()
        assert rt.threads > 0

    def test_custom_threads(self):
        rt = dftu_utils.Runtime(threads=4)
        assert rt.threads == 4
        rt.shutdown()

    def test_context_manager(self):
        with dftu_utils.Runtime(threads=2) as rt:
            assert rt.threads == 2

    def test_shutdown_idempotent(self):
        rt = dftu_utils.Runtime(threads=2)
        rt.shutdown()
        rt.shutdown()  # should not raise

    def test_get_progress(self):
        rt = dftu_utils.Runtime(threads=2)
        p = rt.get_progress()
        assert isinstance(p, dict)
        assert "total" in p
        assert "completed" in p
        rt.shutdown()

    def test_is_responsive(self):
        rt = dftu_utils.Runtime(threads=2)
        assert rt.is_responsive() is True
        rt.shutdown()

    def test_get_default_runtime(self):
        rt = dftu_utils.get_default_runtime()
        assert isinstance(rt, dftu_utils.Runtime)

    def test_every_runtime_is_the_process_runtime(self):
        default = dftu_utils.get_default_runtime()
        dftu_utils.Runtime(threads=4)
        assert default.threads == 4
        dftu_utils.Runtime(threads=3)
        assert default.threads == 3

    def test_leaving_a_with_block_keeps_the_runtime_running(self):
        with dftu_utils.Runtime(threads=2):
            pass
        assert dftu_utils.get_default_runtime().is_responsive() is True


class TestRuntimeProgress:
    """Progress tracking tests."""

    def test_progress_keys(self):
        rt = dftu_utils.Runtime(threads=2)
        p = rt.get_progress()
        for key in (
            "total",
            "completed",
            "running",
            "queued",
            "failed",
            "workers",
            "tasks",
            "errors",
        ):
            assert key in p
        rt.shutdown()

    def test_progress_counts_are_consistent(self):
        # The process runtime is shared, so its counts include earlier work.
        rt = dftu_utils.Runtime(threads=2)
        p = rt.get_progress()
        assert p["completed"] + p["failed"] <= p["total"]
        assert isinstance(p["tasks"], list)
        assert isinstance(p["errors"], list)
        rt.shutdown()

    def test_progress_workers_present(self):
        rt = dftu_utils.Runtime(threads=2)
        p = rt.get_progress()
        assert isinstance(p["workers"], list)
        assert len(p["workers"]) >= 1
        for w in p["workers"]:
            assert "id" in w
            assert "idle" in w
            assert "task" in w
            assert "queue_depth" in w
        rt.shutdown()

    def _indexed(self, env):
        gz = env.create_test_gzip_file()
        with dftu_utils.Indexer(files=[gz], index_dir=env.temp_dir) as ix:
            ix.ensure_indexed()
        return gz

    def test_progress_after_stream(self):
        from .common import Environment

        with Environment(lines=10) as env:
            gz = self._indexed(env)
            rt = dftu_utils.Runtime(threads=2)
            list(dftu_utils.TraceViewer(gz, index_path=env.temp_dir, runtime=rt).stream())
            rt.shutdown()
            p = rt.get_progress()
            assert p["total"] >= 1
            assert p["completed"] >= 1

    def test_progress_task_details(self):
        from .common import Environment

        with Environment(lines=10) as env:
            gz = self._indexed(env)
            rt = dftu_utils.Runtime(threads=2)
            list(dftu_utils.TraceViewer(gz, index_path=env.temp_dir, runtime=rt).stream())
            rt.shutdown()
            p = rt.get_progress()
            assert len(p["tasks"]) >= 1
            task = p["tasks"][0]
            assert "name" in task and "state" in task
            assert task["state"] == "completed"
            assert task["execution_duration_ms"] >= 0
            assert task["queued_duration_ms"] >= 0

    def test_progress_after_multiple_ops(self):
        from .common import Environment

        with Environment(lines=10) as env:
            gz = self._indexed(env)
            rt = dftu_utils.Runtime(threads=2)
            tv = dftu_utils.TraceViewer(gz, index_path=env.temp_dir, runtime=rt)
            list(tv.stream())
            list(tv.stream())
            tv.statistics()
            rt.shutdown()
            p = rt.get_progress()
            assert p["total"] >= 3
            assert p["completed"] >= 3
            assert len(p["tasks"]) >= 3

    def test_progress_no_failures_on_success(self):
        from .common import Environment

        with Environment(lines=10) as env:
            gz = self._indexed(env)
            rt = dftu_utils.Runtime(threads=2)
            list(dftu_utils.TraceViewer(gz, index_path=env.temp_dir, runtime=rt).stream())
            rt.shutdown()
            p = rt.get_progress()
            assert p["failed"] == 0
            assert p["errors"] == []


if __name__ == "__main__":
    pytest.main([__file__])
