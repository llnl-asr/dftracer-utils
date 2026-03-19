#!/usr/bin/env python3
"""Test cases for Runtime Python bindings."""

import pytest

import dftracer.utils as dft_utils


class TestRuntimeCreation:
    def test_default_threads(self):
        rt = dft_utils.Runtime()
        assert rt.threads > 0

    def test_custom_threads(self):
        rt = dft_utils.Runtime(threads=4)
        assert rt.threads == 4
        rt.shutdown()

    def test_context_manager(self):
        with dft_utils.Runtime(threads=2) as rt:
            assert rt.threads == 2

    def test_shutdown_idempotent(self):
        rt = dft_utils.Runtime(threads=2)
        rt.shutdown()
        rt.shutdown()  # should not raise

    def test_get_progress(self):
        rt = dft_utils.Runtime(threads=2)
        p = rt.get_progress()
        assert isinstance(p, dict)
        assert "total" in p
        assert "completed" in p
        rt.shutdown()

    def test_is_responsive(self):
        rt = dft_utils.Runtime(threads=2)
        assert rt.is_responsive() is True
        rt.shutdown()

    def test_get_default_runtime(self):
        rt = dft_utils.get_default_runtime()
        assert isinstance(rt, dft_utils.Runtime)

    def test_set_default_runtime(self):
        original = dft_utils.get_default_runtime()
        rt = dft_utils.Runtime(threads=4)
        dft_utils.set_default_runtime(rt)
        default = dft_utils.get_default_runtime()
        assert default.threads == 4
        dft_utils.set_default_runtime(original)


class TestRuntimeProgress:
    """Progress tracking tests."""

    def test_progress_keys(self):
        rt = dft_utils.Runtime(threads=2)
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

    def test_progress_starts_at_zero(self):
        rt = dft_utils.Runtime(threads=2)
        p = rt.get_progress()
        assert p["total"] == 0
        assert p["completed"] == 0
        assert p["failed"] == 0
        assert p["tasks"] == []
        assert p["errors"] == []
        rt.shutdown()

    def test_progress_workers_present(self):
        rt = dft_utils.Runtime(threads=2)
        p = rt.get_progress()
        assert isinstance(p["workers"], list)
        assert len(p["workers"]) == 2
        for w in p["workers"]:
            assert "id" in w
            assert "idle" in w
            assert "task" in w
            assert "queue_depth" in w
        rt.shutdown()

    def test_progress_after_read_lines(self):
        from .common import Environment

        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            rt = dft_utils.Runtime(threads=2)
            reader = dft_utils.TraceReader(gz_file, runtime=rt)
            reader.read_lines()
            rt.shutdown()
            p = rt.get_progress()
            assert p["total"] >= 1
            assert p["completed"] >= 1

    def test_progress_after_iter_lines(self):
        from .common import Environment

        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            rt = dft_utils.Runtime(threads=2)
            reader = dft_utils.TraceReader(gz_file, runtime=rt)
            list(reader.iter_lines())
            rt.shutdown()
            p = rt.get_progress()
            assert p["total"] >= 1
            assert p["completed"] >= 1

    def test_progress_task_details(self):
        """Completed tasks appear in the tasks list with timing."""
        from .common import Environment

        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            rt = dft_utils.Runtime(threads=2)
            reader = dft_utils.TraceReader(gz_file, runtime=rt)
            reader.read_lines()
            rt.shutdown()
            p = rt.get_progress()
            assert len(p["tasks"]) >= 1
            task = p["tasks"][0]
            assert "name" in task
            assert "state" in task
            assert task["state"] == "completed"
            assert "execution_duration_ms" in task
            assert task["execution_duration_ms"] >= 0
            assert "queued_duration_ms" in task
            assert task["queued_duration_ms"] >= 0

    def test_progress_after_multiple_reads(self):
        from .common import Environment

        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            rt = dft_utils.Runtime(threads=2)
            reader = dft_utils.TraceReader(gz_file, runtime=rt)
            reader.read_lines()
            reader.read_lines()
            reader.read_raw()
            rt.shutdown()
            p = rt.get_progress()
            assert p["total"] >= 3
            assert p["completed"] >= 3
            assert len(p["tasks"]) >= 3

    def test_progress_no_failures_on_success(self):
        from .common import Environment

        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            rt = dft_utils.Runtime(threads=2)
            reader = dft_utils.TraceReader(gz_file, runtime=rt)
            reader.read_lines()
            rt.shutdown()
            p = rt.get_progress()
            assert p["failed"] == 0
            assert p["errors"] == []


if __name__ == "__main__":
    pytest.main([__file__])
