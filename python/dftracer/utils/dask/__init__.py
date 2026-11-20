# SPDX-License-Identifier: MIT
"""Dask distributed integration for dftracer-utils.

Nothing here imports ``dask`` until a name is used: each public name loads its
submodule on first access, so ``import dftracer.utils.dask`` costs a few
milliseconds and the optional dependency stays optional.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from .._lazy import lazy_names

if TYPE_CHECKING:
    from .frame import DaskFrame as DaskFrame
    from .index import DistributedIndexResult as DistributedIndexResult
    from .index import assign_files_by_pid as assign_files_by_pid
    from .index import distributed_index as distributed_index
    from .progress import ProgressAggregator as ProgressAggregator
    from .viewer import DaskAggregatedTraceViewer as DaskAggregatedTraceViewer
    from .viewer import DaskTraceViewer as DaskTraceViewer
    from .viewer import QueryPage as QueryPage
    from .viewer import distributed_write_trace as distributed_write_trace
    from .worker import DFTracerUtilsDaskWorkerPlugin as DFTracerUtilsDaskWorkerPlugin
    from .worker import register_auto_thread_plugin as register_auto_thread_plugin

__all__ = [
    "DaskFrame",
    "DaskTraceViewer",
    "DaskAggregatedTraceViewer",
    "DFTracerUtilsDaskWorkerPlugin",
    "DistributedIndexResult",
    "ProgressAggregator",
    "QueryPage",
    "register_auto_thread_plugin",
    "assign_files_by_pid",
    "distributed_index",
    "distributed_write_trace",
]

_SUBMODULE = {
    "DaskFrame": "frame",
    "DistributedIndexResult": "index",
    "assign_files_by_pid": "index",
    "distributed_index": "index",
    "ProgressAggregator": "progress",
    "DaskAggregatedTraceViewer": "viewer",
    "DaskTraceViewer": "viewer",
    "QueryPage": "viewer",
    "distributed_write_trace": "viewer",
    "DFTracerUtilsDaskWorkerPlugin": "worker",
    "register_auto_thread_plugin": "worker",
}

__getattr__, __dir__ = lazy_names(__name__, globals(), _SUBMODULE)
