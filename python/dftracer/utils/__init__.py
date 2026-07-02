import atexit
from importlib.metadata import PackageNotFoundError, version
from typing import Optional

from .arrow import read_arrow, write_arrow  # noqa: F401
from .dftracer_utils_ext import (  # noqa: F401
    CheckpointIndexer,  # noqa: F401
    DFTUtilsAggregationError,
    DFTUtilsCompressionError,
    DFTUtilsError,
    DFTUtilsIndexerError,
    DFTUtilsIOError,
    DFTUtilsNotFoundError,
    DFTUtilsParseError,
    DFTUtilsPipelineError,
    DFTUtilsQueryError,
    DFTUtilsReaderError,
    DFTUtilsValueError,
    IndexerCheckpoint,  # noqa: F401
    JsonDictValue,  # noqa: F401
    get_log_level,
    set_log_color,
    set_log_level,
)
from .dftracer_utils_ext import (
    get_default_runtime as _get_default_native_runtime,
)
from .dftracer_utils_ext import (
    set_default_runtime as _set_default_native_runtime,
)
from .indexer import (  # noqa: F401
    AggregationConfig,
    Indexer,
    IndexStatus,
)
from .query import Expr, Field  # noqa: F401
from .runtime import Runtime, TaskHandle  # noqa: F401
from .trace_reader import TraceReader  # noqa: F401

_default_wrapper: Optional["Runtime"] = None


def get_default_runtime() -> "Runtime":
    """Return the module-level default Runtime (lazy-created)."""
    global _default_wrapper
    if _default_wrapper is None:
        native = _get_default_native_runtime()
        _default_wrapper = Runtime._from_native(native)
    return _default_wrapper


def set_default_runtime(runtime: Optional["Runtime"]) -> None:
    """Replace the module-level default Runtime (pass None to clear)."""
    global _default_wrapper
    if runtime is None:
        _set_default_native_runtime(None)
        _default_wrapper = None
    else:
        _set_default_native_runtime(runtime._native)
        _default_wrapper = runtime


@atexit.register
def _shutdown_default_runtime() -> None:
    # Join executor threads while the interpreter is still alive to avoid
    # teardown hangs. Only act on a lazily-created default; never force one.
    global _default_wrapper
    wrapper = _default_wrapper
    if wrapper is None:
        return
    _default_wrapper = None
    try:
        wrapper.shutdown(wait=True)
    except Exception:
        pass


try:
    __version__ = version("dftracer-utils")
except PackageNotFoundError:
    __version__ = "0.0.0"


__all__ = [
    "AggregationConfig",
    "CheckpointIndexer",
    "Expr",
    "Field",
    "Indexer",
    "IndexerCheckpoint",
    "IndexStatus",
    "JsonDictValue",
    "TraceReader",
    "Runtime",
    "TaskHandle",
    "get_default_runtime",
    "get_log_level",
    "read_arrow",
    "set_default_runtime",
    "set_log_color",
    "set_log_level",
    "write_arrow",
]
