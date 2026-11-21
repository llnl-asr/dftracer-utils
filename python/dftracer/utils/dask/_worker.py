# SPDX-License-Identifier: MIT
"""The Dask worker plugin that sizes each worker's runtime."""

from __future__ import annotations

import logging
import os
import time
from collections import Counter
from typing import (
    TYPE_CHECKING,
    Protocol,
    cast,
)

if TYPE_CHECKING:
    from distributed import Worker


from dask.distributed import WorkerPlugin, get_client

from ..runtime import Runtime
from ._threads import runtime_threads


class _RuntimeHolder(Protocol):
    dftracer_utils_runtime: Runtime


class DFTracerUtilsDaskWorkerPlugin(WorkerPlugin):
    """Configures the process runtime of each Dask worker."""

    def __init__(self, threads: int = 0, io_threads: int = 0) -> None:
        self.threads = threads
        self.io_threads = io_threads

    def setup(self, worker: Worker) -> None:
        holder = cast("_RuntimeHolder", worker)
        holder.dftracer_utils_runtime = Runtime(threads=self.threads, io_threads=self.io_threads)

    def teardown(self, worker: Worker) -> None:
        holder = cast("_RuntimeHolder", worker)
        if hasattr(holder, "dftracer_utils_runtime"):
            # wait=False: don't block on pending tasks during teardown.
            # Dask may be tearing down because of timeout/cancel, and a
            # stuck task would hang the worker process indefinitely.
            try:
                holder.dftracer_utils_runtime.shutdown(wait=False)
            except Exception:
                pass
            del holder.dftracer_utils_runtime


_plugin_registered_schedulers: set[str] = set()


def register_auto_thread_plugin() -> None:
    """Register the DFTracer worker plugin on the active distributed client.

    Sizes each worker's C++ Runtime, compute and I/O threads alike, to the
    worker's own Dask thread count, so several workers on one node do not each
    claim every core.

    Idempotent: re-registering the same plugin on the same scheduler triggers a
    teardown+setup round-trip on every worker, which deadlocks if the previous
    Runtime still has in-flight coroutines. Skips if already registered for the
    scheduler address. A no-op when no distributed client is active.
    """
    try:
        client = get_client()
        sched_addr = getattr(client.scheduler, "address", None) or ""
        if sched_addr in _plugin_registered_schedulers:
            return

        def _addr_to_host(addr: str) -> str:
            return addr.split("://")[-1].rsplit(":", 1)[0]

        nthreads = client.nthreads()
        for _ in range(10):
            nthreads_next = client.nthreads()
            if len(nthreads_next) >= len(nthreads):
                nthreads = nthreads_next
            if len(nthreads) > 0:
                break
            time.sleep(0.5)
        host_counts = Counter(_addr_to_host(a) for a in nthreads.keys())

        logging.getLogger("dftracer.dask_plugin").info(
            "coord register_plugin: host_counts=%s total_workers=%d worker_addr_sample=%s",
            dict(host_counts),
            sum(host_counts.values()),
            list(nthreads.keys())[:8],
        )

        class _AutoThreadPlugin(DFTracerUtilsDaskWorkerPlugin):
            def __init__(self, host_worker_counts: dict[str, int]) -> None:
                super().__init__(threads=0)
                self._host_worker_counts = host_worker_counts

            def setup(self, worker: Worker) -> None:
                affinity = getattr(os, "sched_getaffinity", None)  # Linux only
                total_cpus = len(affinity(0)) if affinity else (os.cpu_count() or 1)
                my_host = worker.address.split("://")[-1].rsplit(":", 1)[0]
                self.threads = runtime_threads(worker, self._host_worker_counts, total_cpus)
                # The I/O pool needs the same treatment: left at 0 it defaults
                # to one thread per core in every worker on the node.
                self.io_threads = self.threads
                logging.getLogger("distributed.worker").info(
                    "DFTracer Runtime: host=%s cpus=%d worker_nthreads=%s "
                    "cpp_threads=%d io_threads=%d",
                    my_host,
                    total_cpus,
                    getattr(worker, "nthreads", None),
                    self.threads,
                    self.io_threads,
                )
                super().setup(worker)

        client.register_plugin(_AutoThreadPlugin(dict(host_counts)))
        _plugin_registered_schedulers.add(sched_addr)
        logging.getLogger("dftracer.dask_plugin").info(
            "Registered DFTracerUtilsDaskWorkerPlugin host_worker_counts=%s total_workers=%d",
            dict(host_counts),
            sum(host_counts.values()),
        )
    except (ValueError, ImportError):
        pass
