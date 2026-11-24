# SPDX-License-Identifier: MIT
"""Dask distributed integration for dftracer-utils.

Nothing here imports ``dask`` until a name is used: each public name loads its
submodule on first access, so ``import dftracer.utils.dask`` costs a few
milliseconds and the optional dependency stays optional.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from .._lazy import lazy_names as _lazy_names

if TYPE_CHECKING:
    from ._frame import DaskFrame as DaskFrame
    from ._index import DistributedIndexResult as DistributedIndexResult
    from ._index import assign_files_by_pid as assign_files_by_pid
    from ._index import distributed_index as distributed_index
    from ._progress import ProgressAggregator as ProgressAggregator
    from ._viewer import DaskAggregatedTraceViewer as DaskAggregatedTraceViewer
    from ._viewer import DaskTraceViewer as DaskTraceViewer
    from ._viewer import QueryPage as QueryPage
    from ._viewer import distributed_write_trace as distributed_write_trace
    from ._worker import DFTracerUtilsDaskWorkerPlugin as DFTracerUtilsDaskWorkerPlugin
    from ._worker import register_auto_thread_plugin as register_auto_thread_plugin

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
    "DaskFrame": "_frame",
    "DistributedIndexResult": "_index",
    "assign_files_by_pid": "_index",
    "distributed_index": "_index",
    "ProgressAggregator": "_progress",
    "DaskAggregatedTraceViewer": "_viewer",
    "DaskTraceViewer": "_viewer",
    "QueryPage": "_viewer",
    "distributed_write_trace": "_viewer",
    "DFTracerUtilsDaskWorkerPlugin": "_worker",
    "register_auto_thread_plugin": "_worker",
}

__getattr__, __dir__ = _lazy_names(__name__, globals(), _SUBMODULE)
