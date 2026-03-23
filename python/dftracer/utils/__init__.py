from importlib.metadata import PackageNotFoundError, version
from typing import Optional

from .dftracer_utils_ext import (
    JSON,  # noqa: F401
    Indexer,  # noqa: F401
    IndexerCheckpoint,  # noqa: F401
    TraceReader,  # noqa: F401
)
from .dftracer_utils_ext import (
    get_default_runtime as _get_default_native_runtime,
)
from .dftracer_utils_ext import (
    set_default_runtime as _set_default_native_runtime,
)
from .query import Expr, Field  # noqa: F401
from .runtime import Runtime, TaskHandle  # noqa: F401

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


try:
    __version__ = version("dftracer-utils")
except PackageNotFoundError:
    __version__ = "0.0.0"


__all__ = [
    "Expr",
    "Field",
    "Indexer",
    "IndexerCheckpoint",
    "TraceReader",
    "Runtime",
    "TaskHandle",
    "get_default_runtime",
    "set_default_runtime",
]
