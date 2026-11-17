# SPDX-License-Identifier: MIT
import atexit as _atexit
from importlib.metadata import PackageNotFoundError as _PackageNotFoundError
from importlib.metadata import version as _version
from typing import Optional as _Optional

from . import schemas  # noqa: F401
from .columnar import (  # noqa: F401
    Agg,
    Expr,
    F,
    GroupBy,
    Udaf,
    Udf,
    call,
    col,
    columnar,
    count,
    eval_many,
    lit,
    udaf,
    udf,
    where,
)
from .dataframe import DataFrame  # noqa: F401
from .dftracer_utils_ext import (  # noqa: F401
    CheckpointIndexer,  # noqa: F401
    DFTUtilsAggregationError,
    DFTUtilsCompressionError,
    DFTUtilsDuqlError,
    DFTUtilsError,
    DFTUtilsIndexerError,
    DFTUtilsIOError,
    DFTUtilsNotFoundError,
    DFTUtilsParseError,
    DFTUtilsPipelineError,
    DFTUtilsReaderError,
    DFTUtilsValueError,
    _close_task_callbacks,
    consolidate_shard_set,
    get_log_level,
    set_log_color,
    set_log_level,
    write_shard_set,
)
from .dftracer_utils_ext import (
    get_default_runtime as _get_default_native_runtime,
)
from .dftracer_utils_ext import (
    peek_default_runtime as _peek_default_native_runtime,
)
from .enums import AggOp, DType, GroupKey, Phase  # noqa: F401
from .indexer import (  # noqa: F401
    AggregationConfig,
    BloomConfig,
    Indexer,
    IndexStatus,
)
from .lazyframe import (  # noqa: F401
    BoundColumn,
    LazyFrame,
    LazyResult,
    LazyScalar,
    SinkSummary,
    collect_all,
    lazy,
    scan_file,
)
from .live_trace_viewer import LiveFileStatus, LiveTraceViewer  # noqa: F401
from .runtime import Runtime, TaskHandle  # noqa: F401
from .series import Series  # noqa: F401
from .store import Store  # noqa: F401
from .time_unit import TimeUnit  # noqa: F401
from .trace_viewer import Containment, TraceViewer  # noqa: F401

_default_wrapper: _Optional["Runtime"] = None


def get_default_runtime() -> "Runtime":
    """Return the process runtime (made on first use)."""
    global _default_wrapper
    if _default_wrapper is None:
        native = _get_default_native_runtime()
        _default_wrapper = Runtime._from_native(native)
    return _default_wrapper


def peek_default_runtime() -> _Optional["Runtime"]:
    """Return the process runtime, or None before its first use.

    Never makes one, so a caller that only looks does not start a
    full-machine-sized pool.
    """
    global _default_wrapper
    if _default_wrapper is not None:
        return _default_wrapper
    native = _peek_default_native_runtime()
    if native is None:
        return None
    _default_wrapper = Runtime._from_native(native)
    return _default_wrapper


@_atexit.register
def _shutdown_default_runtime() -> None:
    # Wait for this handle's tasks and stop its Python threads while the
    # interpreter is still alive. The native exit handler stops the runtime.
    global _default_wrapper
    wrapper = _default_wrapper
    if wrapper is None:
        return
    _default_wrapper = None
    try:
        wrapper.shutdown(wait=True)
    except Exception:
        pass


# Registered after the shutdown above, so it runs first: done callbacks of
# native tasks that have not started are dropped before the interpreter ends.
_atexit.register(_close_task_callbacks)


try:
    __version__ = _version("dftracer-utils")
except _PackageNotFoundError:
    __version__ = "0.0.0"


__all__ = [
    "AggregationConfig",
    "BloomConfig",
    "CheckpointIndexer",
    "Expr",
    "col",
    "lit",
    "columnar",
    "F",
    "where",
    "Udaf",
    "Udf",
    "udaf",
    "udf",
    "call",
    "Agg",
    "GroupBy",
    "count",
    "eval_many",
    "Indexer",
    "IndexStatus",
    "AggOp",
    "DType",
    "GroupKey",
    "Phase",
    "TimeUnit",
    "Store",
    "TraceViewer",
    "LiveFileStatus",
    "LiveTraceViewer",
    "Containment",
    "DataFrame",
    "LazyFrame",
    "LazyResult",
    "LazyScalar",
    "BoundColumn",
    "SinkSummary",
    "collect_all",
    "lazy",
    "scan_file",
    "Series",
    "Runtime",
    "TaskHandle",
    "get_default_runtime",
    "peek_default_runtime",
    "get_log_level",
    "set_log_color",
    "set_log_level",
    "write_shard_set",
    "consolidate_shard_set",
    "DFTUtilsError",
    "DFTUtilsAggregationError",
    "DFTUtilsCompressionError",
    "DFTUtilsDuqlError",
    "DFTUtilsIndexerError",
    "DFTUtilsIOError",
    "DFTUtilsNotFoundError",
    "DFTUtilsParseError",
    "DFTUtilsPipelineError",
    "DFTUtilsReaderError",
    "DFTUtilsValueError",
]
