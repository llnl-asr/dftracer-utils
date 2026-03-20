from typing import Optional, Union
from importlib.metadata import version, PackageNotFoundError

from .dftracer_utils_ext import (
    Reader,  # noqa: F401
    Indexer,  # noqa: F401
    IndexerCheckpoint,  # noqa: F401
    JSON,  # noqa: F401
    TraceReader,  # noqa: F401
)
from .dftracer_utils_ext import (
    get_default_runtime as _get_default_native_runtime,
)
from .dftracer_utils_ext import (
    set_default_runtime as _set_default_native_runtime,
)

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


def dft_reader(
    gzip_path_or_indexer: Union[str, Indexer], index_path: Optional[str] = None
):
    """Create a reader

    Args:
        gzip_path_or_indexer: Either a path to gzip file or a Indexer instance
        index_path: Path to index file (ignored if indexer is provided)

    Returns:
        Reader instance
    """
    if isinstance(gzip_path_or_indexer, Indexer):
        return Reader(gzip_path_or_indexer.gz_path, indexer=gzip_path_or_indexer)
    else:
        return Reader(gzip_path_or_indexer, index_path)


__all__ = [
    "Reader",
    "Indexer",
    "IndexerCheckpoint",
    "TraceReader",
    "Runtime",
    "TaskHandle",
    "get_default_runtime",
    "set_default_runtime",
    "dft_reader",
]
