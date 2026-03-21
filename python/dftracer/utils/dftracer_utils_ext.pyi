"""Type stubs for dftracer_utils_ext module."""

from types import TracebackType
from typing import Any, Dict, Iterator, List, Optional, Tuple, Type, Union

# ========== INDEXER ==========

class IndexerCheckpoint:
    """Information about a checkpoint in the index."""

    checkpoint_idx: int
    uc_offset: int
    uc_size: int
    c_offset: int
    c_size: int
    bits: int
    num_lines: int

class Indexer:
    """Indexer for creating and managing gzip file indices."""

    def __init__(
        self,
        gz_path: str,
        idx_path: Optional[str] = None,
        checkpoint_size: int = 1048576,
        force_rebuild: bool = False,
        build_bloom: bool = False,
        build_manifest: bool = False,
        index_threshold: int = 8388608,
        runtime: Optional["Runtime"] = None,
    ) -> None:
        """Create an indexer for a gzip file.

        Args:
            gz_path: Path to the gzip trace file.
            idx_path: Path to the index file. If None, uses gz_path + ".idx".
            checkpoint_size: Checkpoint size in bytes for index building.
            force_rebuild: If True, rebuild the index even if it exists.
            build_bloom: If True, build bloom filter data in the index.
            build_manifest: If True, build manifest data in the index.
            index_threshold: Skip indexing for files smaller than this (bytes).
            runtime: Runtime instance for thread pool control.
                If None, uses the default global Runtime.
        """
        ...

    def build(self) -> None:
        """Build the index."""
        ...

    def need_rebuild(self) -> bool:
        """Check if index needs rebuilding."""
        ...

    def exists(self) -> bool:
        """Check if the index file exists."""
        ...

    def get_max_bytes(self) -> int:
        """Get maximum byte position."""
        ...

    def get_num_lines(self) -> int:
        """Get number of lines."""
        ...

    def get_checkpoints(self) -> List[IndexerCheckpoint]:
        """Get all checkpoints."""
        ...

    def find_checkpoint(self, target_offset: int) -> Optional[IndexerCheckpoint]:
        """Find checkpoint for target offset."""
        ...

    @property
    def gz_path(self) -> str:
        """Get gzip path."""
        ...

    @property
    def idx_path(self) -> str:
        """Get index path."""
        ...

    @property
    def checkpoint_size(self) -> int:
        """Get checkpoint size."""
        ...

    @property
    def has_bloom(self) -> bool:
        """Whether bloom filter data exists in the index sidecar."""
        ...

    @property
    def has_manifest(self) -> bool:
        """Whether manifest data exists in the index sidecar."""
        ...

    def __enter__(self) -> "Indexer":
        """Enter the runtime context for the with statement."""
        ...

    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> None:
        """Exit the runtime context for the with statement."""
        ...

# ========== JSON ==========

# Type aliases for JSON values
_JSONPrimitive = Union[str, int, float, bool, None]

class JSON:
    """Lazy JSON object that parses on demand using yyjson.

    This implementation provides lazy nested navigation for memory efficiency:
    - Nested objects/arrays return JSON wrappers (lazy, no conversion)
    - Primitives (str, int, float, bool, None) are converted immediately

    Example:
        json_obj = JSON('{"args": {"hhash": "abc"}, "pid": 42}')
        args = json_obj["args"]  # Returns JSON wrapper (lazy, ~48 bytes)
        hhash = args["hhash"]     # Returns str (converted)
        pid = json_obj["pid"]     # Returns int (converted)
    """

    def __init__(self, json_str: str) -> None:
        """Create a JSON object from a JSON string.

        The JSON string is stored but not parsed until first access.
        """
        ...

    def __contains__(self, key: str) -> bool:
        """Check if key exists in JSON object."""
        ...

    def __getitem__(self, key: str) -> Union[_JSONPrimitive, "JSON"]:
        """Get value by key, raises KeyError if not found.

        Returns:
            - JSON wrapper for nested objects/arrays (lazy evaluation)
            - Primitive Python types for values (str, int, float, bool, None)

        Example:
            obj["nested_object"]  # Returns JSON (lazy wrapper)
            obj["string_field"]    # Returns str
            obj["number_field"]    # Returns int or float
        """
        ...

    def get(
        self,
        key: str,
        default: Union[_JSONPrimitive, "JSON"] = None,
    ) -> Union[_JSONPrimitive, "JSON"]:
        """Get value by key with optional default.

        Returns:
            - JSON wrapper for nested objects/arrays (lazy evaluation)
            - Primitive Python types for values
            - default if key not found
        """
        ...

    def keys(self) -> List[str]:
        """Get all keys from JSON object (only for object types)."""
        ...

    def values(self) -> List[Union[_JSONPrimitive, "JSON"]]:
        """Get all values from JSON object (only for object types).

        Returns:
            - List of values, where nested objects/arrays are JSON wrappers (lazy)
            - Primitives are converted to Python types
        """
        ...

    def items(self) -> List[Tuple[str, Union[_JSONPrimitive, "JSON"]]]:
        """Get all key-value pairs from JSON object (only for object types).

        Returns:
            - List of (key, value) tuples
            - Nested objects/arrays are JSON wrappers (lazy)
            - Primitives are converted to Python types
        """
        ...

    def __len__(self) -> int:
        """Return the number of key-value pairs in the JSON object.

        Returns 0 if the root is not an object.
        """
        ...

    def __bool__(self) -> bool:
        """Return True if the JSON object is non-empty, False otherwise.

        Returns:
            - True if object has at least one key-value pair
            - False if object is empty or root is not an object
        """
        ...

    def unwrap(self) -> Union[Dict[str, Any], List[Any], _JSONPrimitive]:
        """Unwrap the lazy JSON object into native Python dict/list.

        Unlike lazy access via obj[key], this method fully converts the entire
        JSON structure to native Python objects:
        - JSON objects -> Python dicts
        - JSON arrays -> Python lists
        - Primitives -> Python types (str, int, float, bool, None)

        Returns:
            Fully converted Python object (dict, list, or primitive)
        """
        ...

    def copy(self) -> "JSON":
        """Return a shallow copy of the JSON object.

        For subtree wrappers: Creates a new wrapper pointing to the same subtree
        For top-level objects: Creates a new JSON object from the same data

        Returns:
            New JSON object
        """
        ...

    def __str__(self) -> str:
        """Return the JSON string representation.

        For top-level objects: returns original JSON string
        For subtrees: serializes the subtree to JSON
        """
        ...

    def __repr__(self) -> str:
        """Return string representation of the object."""
        ...

# ========== TASK HANDLE ==========

class TaskHandle:
    """Handle to a submitted C++ coroutine task."""

    def get(self) -> Any:
        """Block until task completes and return result. Raises on error."""
        ...

    def wait(self) -> None:
        """Block until task completes. Raises on error."""
        ...

    def done(self) -> bool:
        """Return True if task has completed."""
        ...

    @property
    def name(self) -> str:
        """Task name."""
        ...

    @property
    def task_id(self) -> int:
        """Task identifier."""
        ...

# ========== RUNTIME (C++ native) ==========

class Runtime:
    """Lightweight coroutine runtime wrapping Executor + Watchdog.

    Note: For user-facing API, use dftracer.utils.Runtime (Python wrapper)
    which adds submit(), Python callable support, and error handling.
    """

    def __init__(self, threads: int = 0) -> None: ...
    def shutdown(self) -> None: ...
    def wait_all(self) -> None: ...
    def get_progress(self) -> Dict[str, Any]: ...
    def is_responsive(self) -> bool: ...
    def set_timeout(self, global_ms: int = 0) -> None: ...
    def set_default_task_timeout(self, ms: int = 0) -> None: ...
    @property
    def threads(self) -> int: ...
    def __enter__(self) -> "Runtime": ...
    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> None: ...

def get_default_runtime() -> Runtime: ...
def set_default_runtime(runtime: Optional[Runtime]) -> None: ...

# ========== TRACE READER ==========

class TraceReader:
    """Smart trace file reader that auto-selects sequential vs indexed reading."""

    def __init__(
        self,
        file_path: str,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        auto_build_index: bool = False,
        index_threshold: int = 8388608,
        runtime: Optional[Runtime] = None,
    ) -> None:
        """Create a TraceReader.

        Args:
            file_path: Path to the trace file (.pfw.gz or plain text).
            index_dir: Directory to search for ``.idx`` sidecar files.
                Empty string (default) searches next to the trace file.
            checkpoint_size: Checkpoint interval in bytes for index
                building (default 32 MB).
            auto_build_index: If True, automatically build an index
                when none exists and the file exceeds *index_threshold*.
            index_threshold: Minimum file size in bytes before
                auto-indexing is triggered (default 8 MB).
            runtime: Runtime instance for thread pool control.
                If None, uses the default global Runtime.

        Raises:
            RuntimeError: If *file_path* does not exist or cannot be opened.
        """
        ...

    def read_lines(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
    ) -> List[str]:
        """Read lines from the trace file and return as a list.

        Lines are 1-indexed. Pass ``start_line=0, end_line=0`` (the
        defaults) to read all lines. Out-of-range values are clamped
        to the actual file bounds.
        """
        ...

    def iter_lines(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
    ) -> Iterator[str]:
        """Return a streaming iterator over decoded lines.

        The C++ coroutine runs on the Runtime thread pool and pushes
        lines into a bounded queue; Python ``__next__`` pops from it.
        """
        ...

    def iter_raw(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        line_aligned: bool = True,
        multi_line: bool = True,
        buffer_size: int = 4194304,
    ) -> Iterator[bytes]:
        """Return a streaming iterator over raw byte chunks."""
        ...

    def read_raw(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        line_aligned: bool = True,
        multi_line: bool = True,
        buffer_size: int = 4194304,
    ) -> List[bytes]:
        """Read raw byte chunks and return as a list."""
        ...

    def iter_lines_json(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
    ) -> Iterator["JSON"]:
        """Return iterator over parsed JSON objects.

        Skips non-JSON lines (array delimiters like ``[`` and ``]``).
        Each yielded item is a lazy :class:`JSON` object.
        """
        ...

    def read_lines_json(
        self,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
    ) -> List["JSON"]:
        """Read lines and return as list of parsed JSON objects.

        Equivalent to ``list(self.iter_lines_json(...))``.
        """
        ...

    def iter_arrow(
        self,
        batch_size: int = 10000,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
    ) -> Iterator[Any]:
        """Return iterator over Arrow record batches.

        Each batch is an ``_ArrowBatchCapsule`` implementing the Arrow
        PyCapsule protocol (``__arrow_c_array__``).  Wrap with
        :class:`~dftracer.utils.arrow.ArrowBatch` for convenience
        methods, or pass directly to ``pyarrow.record_batch()``.

        Args:
            batch_size (int): Maximum rows per Arrow batch.
            start_line (int): First line (0 = beginning).
            end_line (int): Last line (0 = end of file).
            start_byte (int): First byte offset (0 = beginning).
            end_byte (int): Last byte offset (0 = end of file).
            buffer_size (int): Internal read buffer size in bytes.
        """
        ...

    def read_arrow(
        self,
        batch_size: int = 10000,
        start_line: int = 0,
        end_line: int = 0,
        start_byte: int = 0,
        end_byte: int = 0,
        buffer_size: int = 4194304,
    ) -> Any:
        """Read all events as an ArrowTable.

        Equivalent to collecting all batches from :meth:`iter_arrow`
        into an :class:`~dftracer.utils.arrow.ArrowTable`.

        Args:
            batch_size (int): Maximum rows per Arrow batch.
            start_line (int): First line (0 = beginning).
            end_line (int): Last line (0 = end of file).
            start_byte (int): First byte offset (0 = beginning).
            end_byte (int): Last byte offset (0 = end of file).
            buffer_size (int): Internal read buffer size in bytes.
        """
        ...

    def get_max_bytes(self) -> int:
        """Get the maximum byte position in the decompressed trace.

        Returns the decompressed size for indexed files, file size for
        plain text files, or 0 for compressed files without an index.
        """
        ...

    def get_num_lines(self) -> int:
        """Get the total number of lines in the trace.

        Returns the line count for indexed files, or 0 for files
        without an index (use :attr:`num_lines` property for fallback
        counting).
        """
        ...

    @property
    def file_path(self) -> str:
        """Path to the trace file."""
        ...

    @property
    def index_dir(self) -> str:
        """Directory searched for index sidecar files."""
        ...

    @property
    def has_index(self) -> bool:
        """True if a checkpoint index was found at construction time."""
        ...

    @property
    def num_lines(self) -> int:
        """Total line count (reads all lines to compute if needed)."""
        ...

    def __enter__(self) -> "TraceReader":
        """Enter the runtime context for the with statement."""
        ...

    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> None:
        """Exit the runtime context for the with statement."""
        ...

class StatisticsQueryUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        file_path: str,
        query_type: str = "summary",
        top_n: int = 10,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        file_path: str,
        query_type: str = "summary",
        top_n: int = 10,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class BloomQueryUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        file_path: str,
        predicates: Dict[str, List[str]],
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        file_path: str,
        predicates: Dict[str, List[str]],
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class StatisticsAggregatorUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        file_path: str,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        file_path: str,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class MetadataCollectorUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        file_path: str,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        file_path: str,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class ViewBuilderUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        file_path: str,
        predicates: Optional[Dict[str, List[str]]] = None,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        file_path: str,
        predicates: Optional[Dict[str, List[str]]] = None,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class ViewReaderUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        file_path: str,
        predicates: Optional[Dict[str, List[str]]] = None,
        index_dir: str = "",
    ) -> Any: ...
    def __call__(
        self,
        file_path: str,
        predicates: Optional[Dict[str, List[str]]] = None,
        index_dir: str = "",
    ) -> Any: ...
    def iter_arrow(
        self,
        file_path: str,
        predicates: Optional[Dict[str, List[str]]] = None,
        index_dir: str = "",
        batch_size: int = 10000,
    ) -> Iterator[Any]: ...

class ReorganizationPlannerUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        source_files: List[str],
        groups: Optional[List[Dict[str, str]]] = None,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        source_files: List[str],
        groups: Optional[List[Dict[str, str]]] = None,
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class ReconstructionPlannerUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        reorganized_files: List[str],
        index_dir: str = "",
    ) -> Dict[str, Any]: ...
    def __call__(
        self,
        reorganized_files: List[str],
        index_dir: str = "",
    ) -> Dict[str, Any]: ...

class AggregatorUtility:
    def __init__(self, runtime: Optional["Runtime"] = None) -> None: ...
    def process(
        self,
        directory: str,
        time_interval: float = 5.0,
        group_keys: Optional[List[str]] = None,
        categories: Optional[List[str]] = None,
        names: Optional[List[str]] = None,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        executor_threads: int = 4,
        force_rebuild: bool = False,
        chunk_size_mb: int = 64,
        batch_size_mb: int = 4,
        event_batch_size: int = 10000,
    ) -> Any: ...
    def __call__(
        self,
        directory: str,
        time_interval: float = 5.0,
        group_keys: Optional[List[str]] = None,
        categories: Optional[List[str]] = None,
        names: Optional[List[str]] = None,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        executor_threads: int = 4,
        force_rebuild: bool = False,
        chunk_size_mb: int = 64,
        batch_size_mb: int = 4,
        event_batch_size: int = 10000,
    ) -> Any: ...
    def iter_arrow(
        self,
        directory: str,
        time_interval: float = 5.0,
        group_keys: Optional[List[str]] = None,
        categories: Optional[List[str]] = None,
        names: Optional[List[str]] = None,
        index_dir: str = "",
        checkpoint_size: int = 33554432,
        executor_threads: int = 4,
        force_rebuild: bool = False,
        chunk_size_mb: int = 64,
        batch_size_mb: int = 4,
        event_batch_size: int = 10000,
    ) -> Iterator[Any]: ...
