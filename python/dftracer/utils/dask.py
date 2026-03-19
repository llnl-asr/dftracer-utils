"""Dask distributed integration for dftracer-utils."""

try:
    from dask.distributed import WorkerPlugin
except ImportError:
    WorkerPlugin = None

from dftracer.utils import Runtime, get_default_runtime, set_default_runtime

if WorkerPlugin is not None:

    class DFTracerUtilsDaskWorkerPlugin(WorkerPlugin):
        """Creates a persistent Runtime per Dask worker.

        Usage:
            client = Client("scheduler:8786")
            client.register_plugin(
                DFTracerUtilsDaskWorkerPlugin(threads=48)
            )
        """

        def __init__(self, threads=0):
            self.threads = threads

        def setup(self, worker):
            worker._dftracer_prev_runtime = get_default_runtime()
            rt = Runtime(threads=self.threads)
            worker.dftracer_utils_runtime = rt
            set_default_runtime(rt)

        def teardown(self, worker):
            if hasattr(worker, "_dftracer_prev_runtime"):
                set_default_runtime(worker._dftracer_prev_runtime)
                del worker._dftracer_prev_runtime
            if hasattr(worker, "dftracer_utils_runtime"):
                worker.dftracer_utils_runtime.shutdown()
                del worker.dftracer_utils_runtime
