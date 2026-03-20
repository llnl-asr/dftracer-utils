#!/usr/bin/env python3
"""Test cases for DFTracerUtilsDaskWorkerPlugin."""

import pytest

try:
    from dask.distributed import WorkerPlugin

    DASK_DISTRIBUTED_AVAILABLE = True
except ImportError:
    DASK_DISTRIBUTED_AVAILABLE = False

import dftracer.utils as dft_utils


@pytest.mark.skipif(not DASK_DISTRIBUTED_AVAILABLE, reason="dask.distributed not available")
class TestDaskWorkerPlugin:
    def test_import(self):
        from dftracer.utils.dask import DFTracerUtilsDaskWorkerPlugin

        assert issubclass(DFTracerUtilsDaskWorkerPlugin, WorkerPlugin)

    def test_init_default_threads(self):
        from dftracer.utils.dask import DFTracerUtilsDaskWorkerPlugin

        plugin = DFTracerUtilsDaskWorkerPlugin()
        assert plugin.threads == 0

    def test_init_custom_threads(self):
        from dftracer.utils.dask import DFTracerUtilsDaskWorkerPlugin

        plugin = DFTracerUtilsDaskWorkerPlugin(threads=8)
        assert plugin.threads == 8

    def test_setup_creates_runtime(self):
        from dftracer.utils.dask import DFTracerUtilsDaskWorkerPlugin

        original = dft_utils.get_default_runtime()
        plugin = DFTracerUtilsDaskWorkerPlugin(threads=2)

        class MockWorker:
            pass

        worker = MockWorker()
        plugin.setup(worker)
        assert hasattr(worker, "dftracer_utils_runtime")
        assert isinstance(worker.dftracer_utils_runtime, dft_utils.Runtime)
        assert worker.dftracer_utils_runtime.threads == 2
        dft_utils.set_default_runtime(original)

    def test_teardown_is_idempotent(self):
        from dftracer.utils.dask import DFTracerUtilsDaskWorkerPlugin

        original = dft_utils.get_default_runtime()
        plugin = DFTracerUtilsDaskWorkerPlugin(threads=2)

        class MockWorker:
            pass

        worker = MockWorker()
        plugin.setup(worker)
        dft_utils.set_default_runtime(original)
        plugin.teardown(worker)
        plugin.teardown(worker)

    def test_setup_sets_default_runtime(self):
        from dftracer.utils.dask import DFTracerUtilsDaskWorkerPlugin

        original = dft_utils.get_default_runtime()
        plugin = DFTracerUtilsDaskWorkerPlugin(threads=4)

        class MockWorker:
            pass

        worker = MockWorker()
        plugin.setup(worker)
        default = dft_utils.get_default_runtime()
        assert default.threads == 4
        dft_utils.set_default_runtime(original)


@pytest.mark.skipif(not DASK_DISTRIBUTED_AVAILABLE, reason="dask.distributed not available")
class TestDaskWorkerPluginIntegration:
    """Integration tests with a real LocalCluster.

    Uses processes=True so each worker is a separate process with its
    own global Runtime -- matching the real distributed deployment.
    """

    def test_plugin_with_local_cluster(self):
        from dask.distributed import Client, LocalCluster

        from dftracer.utils.dask import DFTracerUtilsDaskWorkerPlugin

        from .common import Environment

        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()

            cluster = LocalCluster(n_workers=1, threads_per_worker=1)
            client = Client(cluster)
            try:
                client.register_plugin(DFTracerUtilsDaskWorkerPlugin(threads=2))

                def read_line_count(path):
                    import dftracer.utils as dft

                    reader = dft.TraceReader(path)
                    return sum(1 for _ in reader.iter_lines())

                future = client.submit(read_line_count, gz_file)
                result = future.result()
                assert result == 22
            finally:
                client.close()
                cluster.close()

    def test_plugin_multiple_files(self):
        from dask.distributed import Client, LocalCluster

        from dftracer.utils.dask import DFTracerUtilsDaskWorkerPlugin

        from .common import Environment

        with Environment(lines=10) as env:
            files = [env.create_test_gzip_file() for _ in range(3)]

            cluster = LocalCluster(n_workers=2, threads_per_worker=1)
            client = Client(cluster)
            try:
                client.register_plugin(DFTracerUtilsDaskWorkerPlugin(threads=2))

                def read_line_count(path):
                    import dftracer.utils as dft

                    reader = dft.TraceReader(path)
                    return sum(1 for _ in reader.iter_lines())

                futures = client.map(read_line_count, files)
                results = client.gather(futures)
                assert all(r == 12 for r in results)
            finally:
                client.close()
                cluster.close()


if __name__ == "__main__":
    pytest.main([__file__])
