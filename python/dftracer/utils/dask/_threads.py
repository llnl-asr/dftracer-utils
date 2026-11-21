# SPDX-License-Identifier: MIT
"""The thread count of a Dask worker's runtime, which needs no dask import."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from distributed import Worker


def runtime_threads(worker: Worker, host_worker_counts: dict[str, int], total_cpus: int) -> int:
    """C++ Runtime thread count for one Dask worker.

    Dask already divided the node between its workers and the Runtime is shared
    by every task on this worker, so the worker's own thread count is the share
    to match. `host_worker_counts` is a client-side snapshot and only a
    fallback: a worker that started later, or whose address spells the host
    differently, is missing from it, and assuming it is alone on the node
    oversubscribes by however many workers the node really has.
    """
    own = getattr(worker, "nthreads", None) or getattr(
        getattr(worker, "state", None), "nthreads", None
    )
    if own:
        return max(1, min(int(own), total_cpus))
    host = worker.address.split("://")[-1].rsplit(":", 1)[0]
    n_local = host_worker_counts.get(host) or max(host_worker_counts.values(), default=1)
    return max(1, total_cpus // n_local)
