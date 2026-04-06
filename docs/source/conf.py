# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

import os
import inspect
import importlib
import subprocess
import sys
import types
from pathlib import Path
from types import TracebackType
from typing import Iterator

# Auto-generate Mermaid class diagrams from Doxygen XML before building
_docs_dir = Path(__file__).parent.parent  # docs/
_script = _docs_dir / "scripts" / "generate_class_diagrams.py"
_xml_dir = _docs_dir / "doxygen" / "xml"
_gen_dir = _docs_dir / "source" / "_generated"
if _script.exists() and _xml_dir.exists():
    print("Generating Mermaid class diagrams from Doxygen XML...")
    subprocess.run(
        [
            sys.executable,
            str(_script),
            "--xml-dir",
            str(_xml_dir),
            "--output-dir",
            str(_gen_dir),
        ],
        check=False,
    )

# Auto-generate C++ API reference pages from Doxygen XML
_api_script = _docs_dir / "scripts" / "generate_api_index.py"
_api_out = _docs_dir / "source" / "cpp_api" / "api"
if _api_script.exists() and _xml_dir.exists():
    print("Generating C++ API reference pages from Doxygen XML...")
    subprocess.run(
        [
            sys.executable,
            str(_api_script),
            "--xml-dir",
            str(_xml_dir),
            "--output-dir",
            str(_api_out),
        ],
        check=False,
    )

ON_READTHEDOCS = os.environ.get("READTHEDOCS", "").lower() == "true"
PYTHON_SOURCE_DIR = _docs_dir.parent / "python"
autodoc_mock_imports = []


def _install_rtd_extension_stub() -> None:
    """Install a lightweight stub for the native extension on RTD."""

    ext_name = "dftracer.utils.dftracer_utils_ext"
    if ext_name in sys.modules:
        return

    ext = types.ModuleType(ext_name)
    JSONPrimitive = str | int | float | bool | None

    class _BaseNative:
        """RTD stub for native extension classes."""
        pass

    class TaskHandle(_BaseNative):
        """Handle to a submitted task.

        Returned by asynchronous utility calls and by
        :class:`dftracer.utils.Runtime`. The handle can be waited on,
        queried for completion, or used to fetch the task result.
        """

        name = ""
        task_id = 0

        def get(self) -> object | None:
            """Block until the task completes and return its result."""
            return None

        def wait(self) -> None:
            """Block until the task completes."""
            return None

        def done(self) -> bool:
            """Return ``True`` when the task has completed."""
            return True

    class Runtime(_BaseNative):
        """Lightweight task runtime for native and Python work.

        The runtime owns the executor threads used by coroutine-backed
        readers, indexers, and utilities. The higher-level Python
        wrapper in :mod:`dftracer.utils.runtime` builds on this native
        object to support Python callables and richer task tracking.
        """

        threads = 0

        def __init__(self, threads: int = 0) -> None:
            """Create a runtime with an optional worker-thread count."""
            super().__init__(threads)
            self.threads = threads

        def shutdown(self) -> None:
            """Stop the runtime and release worker resources."""
            return None

        def wait_all(self) -> None:
            """Block until all submitted native work completes."""
            return None

        def get_progress(self) -> dict[str, object]:
            """Return runtime progress metadata."""
            return {}

        def is_responsive(self) -> bool:
            """Return whether the watchdog still considers the runtime healthy."""
            return True

        def set_timeout(self, global_ms: int = 0) -> None:
            """Set a global watchdog timeout in milliseconds."""
            return None

        def set_default_task_timeout(self, ms: int = 0) -> None:
            """Set the default per-task timeout in milliseconds."""
            return None

        def __enter__(self) -> "Runtime":
            """Enter the runtime context manager."""
            return self

        def __exit__(
            self,
            exc_type: type[BaseException] | None,
            exc_val: BaseException | None,
            exc_tb: TracebackType | None,
        ) -> None:
            """Exit the runtime context manager."""
            return None

    class IndexerCheckpoint(_BaseNative):
        """Information about a single checkpoint in a ``.dftindex`` store.

        Checkpoints map compressed and uncompressed offsets and carry
        per-chunk metadata used for seeking and chunk-level pruning.
        """

        checkpoint_idx = 0
        uc_offset = 0
        uc_size = 0
        c_offset = 0
        c_size = 0
        bits = 0
        num_lines = 0

    class Indexer(_BaseNative):
        """Build and query a root-local ``.dftindex`` RocksDB store.

        The indexer extracts checkpoints and optional bloom/manifest
        data for a compressed DFTracer trace. Readers and higher-level
        utilities use this store for chunk pruning and random access.
        """

        def __init__(
            self,
            gz_path: str,
            index_path: str | None = None,
            checkpoint_size: int = 1048576,
            force_rebuild: bool = False,
            build_bloom: bool = False,
            build_manifest: bool = False,
            index_threshold: int = 8388608,
            runtime: Runtime | None = None,
        ) -> None:
            """Create an indexer for a compressed DFTracer trace."""
            self.gz_path = gz_path
            self.index_path = index_path or ""
            self.checkpoint_size = checkpoint_size
            self.has_bloom = build_bloom
            self.has_manifest = build_manifest

        def build(self) -> None:
            """Build the index store for the configured trace file."""
            return None

        def need_rebuild(self) -> bool:
            """Return whether the index is missing or stale."""
            return False

        def exists(self) -> bool:
            """Return whether the index store already exists."""
            return False

        def get_max_bytes(self) -> int:
            """Return the maximum decompressed byte position in the trace."""
            return 0

        def get_num_lines(self) -> int:
            """Return the number of lines recorded in the index."""
            return 0

        def get_checkpoints(self) -> list["IndexerCheckpoint"]:
            """Return all checkpoints stored for the trace."""
            return []

        def find_checkpoint(self, target_offset: int) -> "IndexerCheckpoint | None":
            """Return the checkpoint closest to a decompressed offset."""
            return None

        def close(self) -> None:
            """Release this Python wrapper's native indexer handle."""
            return None

        def __enter__(self) -> "Indexer":
            """Enter the indexer context manager."""
            return self

        def __exit__(
            self,
            exc_type: type[BaseException] | None,
            exc_val: BaseException | None,
            exc_tb: TracebackType | None,
        ) -> None:
            """Exit the indexer context manager."""
            return None

    class JSON(_BaseNative):
        """Lazy JSON wrapper backed by yyjson.

        Nested objects are exposed as additional :class:`JSON` wrappers
        so callers can inspect large trace records without eagerly
        converting the entire payload to Python dictionaries.
        """

        def __init__(self, json_str: str) -> None:
            """Create a lazy JSON wrapper from a JSON string."""
            self._json_str = json_str

        def get(
            self,
            key: str,
            default: "JSON | JSONPrimitive" = None,
        ) -> "JSON | JSONPrimitive":
            """Look up a key and return ``default`` when it is absent."""
            return default

        def keys(self) -> list[str]:
            """Return the keys in the current JSON object."""
            return []

        def values(self) -> list["JSON | JSONPrimitive"]:
            """Return the values in the current JSON object."""
            return []

        def items(self) -> list[tuple[str, "JSON | JSONPrimitive"]]:
            """Return key-value pairs in the current JSON object."""
            return []

        def unwrap(self) -> dict[str, object] | list[object] | JSONPrimitive:
            """Convert the lazy wrapper into native Python data."""
            return {}

        def copy(self) -> "JSON":
            """Return a shallow copy of the current lazy JSON wrapper."""
            return self

        def __contains__(self, key: str) -> bool:
            """Return ``True`` when a key exists in the object."""
            return False

        def __getitem__(self, key: str) -> "JSON | JSONPrimitive":
            """Return a field value or nested :class:`JSON` wrapper."""
            raise KeyError(key)

        def __len__(self) -> int:
            """Return the number of items in the current JSON object."""
            return 0

        def __bool__(self) -> bool:
            """Return whether the current JSON value is non-empty."""
            return False

        def __str__(self) -> str:
            """Return a JSON-like string representation."""
            return "{}"

        def __repr__(self) -> str:
            """Return a developer-facing representation."""
            return "JSON('{}')"

    class TraceReader(_BaseNative):
        """Read DFTracer traces with optional index-assisted pruning.

        ``TraceReader`` chooses between sequential and indexed access
        based on the file format and the presence of a shared
        root-local ``.dftindex`` store. It exposes line, raw-byte,
        JSON, and Arrow-based views over the same trace data.
        """

        def __init__(
            self,
            file_path: str,
            index_dir: str = "",
            checkpoint_size: int = 33554432,
            auto_build_index: bool = False,
            index_threshold: int = 8388608,
            runtime: Runtime | None = None,
        ) -> None:
            """Create a trace reader for plain or compressed DFTracer files."""
            self.file_path = file_path
            self.index_dir = index_dir
            self.checkpoint_size = checkpoint_size
            self.auto_build_index = auto_build_index
            self.index_threshold = index_threshold

        def read_lines(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
        ) -> list[str]:
            """Materialize decoded lines into a Python list.

            Supports optional line/byte ranges and query-based filtering.
            """
            return []

        def iter_lines(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
        ) -> Iterator[str]:
            """Stream decoded lines from the trace.

            The returned iterator yields one UTF-8 decoded line at a time.
            """
            return iter(())

        def iter_raw(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            line_aligned: bool = True,
            multi_line: bool = True,
            buffer_size: int = 4194304,
            query: str | None = None,
        ) -> Iterator[bytes]:
            """Stream raw byte chunks from the trace.

            Byte-range reads can be aligned to line boundaries and can
            optionally return multi-line chunks.
            """
            return iter(())

        def read_raw(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            line_aligned: bool = True,
            multi_line: bool = True,
            buffer_size: int = 4194304,
            query: str | None = None,
        ) -> list[bytes]:
            """Materialize raw byte chunks into a Python list."""
            return []

        def iter_lines_json(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
        ) -> Iterator["JSON"]:
            """Stream lazy :class:`JSON` objects for trace events."""
            return iter(())

        def read_lines_json(
            self,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
        ) -> list["JSON"]:
            """Materialize trace events as lazy :class:`JSON` objects."""
            return []

        def iter_arrow(
            self,
            batch_size: int = 10000,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
        ) -> Iterator["ArrowBatch"]:
            """Stream Arrow batches parsed from trace events."""
            return iter(())

        def read_arrow(
            self,
            batch_size: int = 10000,
            start_line: int = 0,
            end_line: int = 0,
            start_byte: int = 0,
            end_byte: int = 0,
            buffer_size: int = 4194304,
            query: str | None = None,
        ) -> "ArrowTable | None":
            """Materialize Arrow batches as a single table-like result."""
            return None

        def get_max_bytes(self) -> int:
            """Return indexed decompressed size when available."""
            return 0

        def get_num_lines(self) -> int:
            """Return indexed line count when available."""
            return 0

        def __enter__(self) -> "TraceReader":
            """Enter the trace-reader context manager."""
            return self

        def __exit__(
            self,
            exc_type: type[BaseException] | None,
            exc_val: BaseException | None,
            exc_tb: TracebackType | None,
        ) -> None:
            """Exit the trace-reader context manager."""
            return None

    class AggregatorUtility(_BaseNative):
        """Aggregate trace events into Arrow-ready time buckets."""

        def __init__(self, runtime: Runtime | None = None) -> None:
            """Create an aggregation utility bound to an optional runtime."""
            self.runtime = runtime

        def process(
            self,
            source_dir: str,
            output_path: str = "",
            time_interval_ms: float = 5000.0,
            query: str = "",
            index_dir: str = "",
            force_rebuild: bool = False,
            custom_metric_fields: list[str] | None = None,
            compute_percentiles: bool = False,
        ) -> "ArrowTable | None":
            """Aggregate trace events into a materialized Arrow-style result."""
            return None

        def iter_arrow(
            self,
            source_dir: str,
            output_path: str = "",
            time_interval_ms: float = 5000.0,
            query: str = "",
            index_dir: str = "",
            force_rebuild: bool = False,
            custom_metric_fields: list[str] | None = None,
            compute_percentiles: bool = False,
        ) -> Iterator["ArrowBatch"]:
            """Stream Arrow batches for aggregated trace metrics."""
            return iter(())

    class ComparatorUtility(_BaseNative):
        """Compare baseline and variant traces."""

        def __init__(self, runtime: Runtime | None = None) -> None:
            """Create a comparator utility bound to an optional runtime."""
            self.runtime = runtime

        def compare(
            self,
            baseline: str,
            variant: str,
            query: str = "",
            time_interval_ms: float = 5000.0,
            threshold: float = 0.0,
            index_dir: str = "",
            force_rebuild: bool = False,
        ) -> "ArrowTable | None":
            """Return comparison results as Arrow-compatible output."""
            return None

        def compare_json(
            self,
            baseline: str,
            variant: str,
            query: str = "",
            time_interval_ms: float = 5000.0,
            threshold: float = 0.0,
            index_dir: str = "",
            force_rebuild: bool = False,
        ) -> str:
            """Return comparison results as JSON."""
            return "{}"

        def compare_table(
            self,
            baseline: str,
            variant: str,
            query: str = "",
            time_interval_ms: float = 5000.0,
            threshold: float = 0.0,
            index_dir: str = "",
            force_rebuild: bool = False,
        ) -> str:
            """Return comparison results as a formatted text table."""
            return ""

    class StatisticsQueryUtility(_BaseNative):
        """Query summary or top-N statistics from a trace."""

        def __init__(self, runtime: Runtime | None = None) -> None:
            """Create a statistics-query utility bound to an optional runtime."""
            self.runtime = runtime

        def process(
            self,
            file_path: str,
            query_type: str = "summary",
            top_n: int = 10,
            index_dir: str = "",
            auto_build_index: bool = False,
            index_threshold: int = 8388608,
        ) -> dict[str, object]:
            """Return scalar statistics derived from the trace."""
            return {}

    class StatisticsAggregatorUtility(_BaseNative):
        """Aggregate core statistics from a trace into a Python dictionary."""

        def __init__(self, runtime: Runtime | None = None) -> None:
            """Create a statistics-aggregator utility bound to an optional runtime."""
            self.runtime = runtime

        def process(
            self,
            file_path: str,
            index_dir: str = "",
            auto_build_index: bool = False,
            index_threshold: int = 8388608,
        ) -> dict[str, object]:
            """Return aggregate trace statistics."""
            return {}

    class MetadataCollectorUtility(_BaseNative):
        """Collect file metadata and index-aware trace metadata."""

        def __init__(self, runtime: Runtime | None = None) -> None:
            """Create a metadata collector bound to an optional runtime."""
            self.runtime = runtime

        def process(
            self,
            file_path: str,
            index_dir: str = "",
            checkpoint_size: int = 33554432,
            force_rebuild: bool = False,
            index_threshold: int = 8388608,
        ) -> dict[str, object]:
            """Return metadata for a DFTracer trace file."""
            return {}

    class ReorganizationPlannerUtility(_BaseNative):
        """Build a semantic reorganization plan for trace files."""

        def __init__(self, runtime: Runtime | None = None) -> None:
            """Create a reorganization planner bound to an optional runtime."""
            self.runtime = runtime

        def process(
            self,
            source_files: list[str],
            groups: list[dict[str, object]],
            index_dir: str = "",
            checkpoint_size: int = 33554432,
            force_rebuild: bool = False,
            index_threshold: int = 8388608,
        ) -> dict[str, object]:
            """Return a reorganization plan for the requested groups."""
            return {}

    class ReconstructionPlannerUtility(_BaseNative):
        """Build a reconstruction plan from reorganized traces."""

        def __init__(self, runtime: Runtime | None = None) -> None:
            """Create a reconstruction planner bound to an optional runtime."""
            self.runtime = runtime

        def process(
            self,
            reorganized_files: list[str],
            provenance_dir: str = "",
        ) -> dict[str, object]:
            """Return a reconstruction plan for reorganized trace files."""
            return {}

    ext.Indexer = Indexer
    ext.IndexerCheckpoint = IndexerCheckpoint
    ext.JSON = JSON
    ext.Runtime = Runtime
    ext.TaskHandle = TaskHandle
    ext.TraceReader = TraceReader
    ext.AggregatorUtility = AggregatorUtility
    ext.ComparatorUtility = ComparatorUtility
    ext.MetadataCollectorUtility = MetadataCollectorUtility
    ext.ReconstructionPlannerUtility = ReconstructionPlannerUtility
    ext.ReorganizationPlannerUtility = ReorganizationPlannerUtility
    ext.StatisticsAggregatorUtility = StatisticsAggregatorUtility
    ext.StatisticsQueryUtility = StatisticsQueryUtility

    def get_default_runtime() -> Runtime:
        """Return the process-wide default runtime."""
        return Runtime()

    def set_default_runtime(runtime: Runtime | None = None) -> None:
        """Replace or clear the process-wide default runtime."""
        return None

    ext.get_default_runtime = get_default_runtime
    ext.set_default_runtime = set_default_runtime
    for name in [
        "AggregatorUtility",
        "ComparatorUtility",
        "Indexer",
        "IndexerCheckpoint",
        "JSON",
        "MetadataCollectorUtility",
        "ReconstructionPlannerUtility",
        "ReorganizationPlannerUtility",
        "Runtime",
        "StatisticsAggregatorUtility",
        "StatisticsQueryUtility",
        "TaskHandle",
        "TraceReader",
    ]:
        getattr(ext, name).__module__ = ext_name
    ext.__all__ = [
        "AggregatorUtility",
        "ComparatorUtility",
        "Indexer",
        "IndexerCheckpoint",
        "JSON",
        "MetadataCollectorUtility",
        "ReconstructionPlannerUtility",
        "ReorganizationPlannerUtility",
        "Runtime",
        "StatisticsAggregatorUtility",
        "StatisticsQueryUtility",
        "TaskHandle",
        "TraceReader",
        "get_default_runtime",
        "set_default_runtime",
    ]
    sys.modules[ext_name] = ext


def _repo_url() -> str:
    """Return the GitHub repository URL used for source links."""
    repo = os.environ.get("READTHEDOCS_GIT_REPOSITORY")
    if repo:
        repo = repo.removesuffix(".git")
        if repo.startswith("git@github.com:"):
            repo = repo.replace("git@github.com:", "https://github.com/", 1)
        elif repo.startswith("https://github.com/"):
            return repo
        if repo.startswith("github.com/"):
            return f"https://{repo}"

    repo = os.environ.get("GITHUB_REPOSITORY")
    if repo:
        return f"https://github.com/{repo}"

    try:
        remote = (
            subprocess.check_output(
                ["git", "remote", "get-url", "origin"],
                cwd=_docs_dir.parent,
                text=True,
            )
            .strip()
            .removesuffix(".git")
        )
        if remote.startswith("git@github.com:"):
            return remote.replace("git@github.com:", "https://github.com/", 1)
        if remote.startswith("https://github.com/"):
            return remote
    except Exception:
        pass

    return "https://github.com/LLNL/dftracer-utils"


def _source_ref() -> str:
    """Return the git ref used for source links."""
    for env_name in ("READTHEDOCS_GIT_COMMIT_HASH", "GITHUB_SHA"):
        value = os.environ.get(env_name)
        if value:
            return value
    try:
        return (
            subprocess.check_output(
                ["git", "rev-parse", "HEAD"],
                cwd=_docs_dir.parent,
                text=True,
            )
            .strip()
        )
    except Exception:
        return "develop"

REPO_URL = _repo_url()
SOURCE_REF = _source_ref()


def _pyi_target_for_extension(fullname: str) -> tuple[Path, list[str]] | None:
    """Map extension-exported objects to their public type-stub file."""
    top = fullname.split(".", 1)[0]
    utility_map = {
        "AggregatorUtility": "python/dftracer/utils/utilities/_aggregator.pyi",
        "ComparatorUtility": "python/dftracer/utils/utilities/_comparator.pyi",
        "MetadataCollectorUtility": (
            "python/dftracer/utils/utilities/_metadata_collector.pyi"
        ),
        "StatisticsQueryUtility": (
            "python/dftracer/utils/utilities/_statistics_query.pyi"
        ),
        "StatisticsAggregatorUtility": (
            "python/dftracer/utils/utilities/_statistics_aggregator.pyi"
        ),
        "ReorganizationPlannerUtility": (
            "python/dftracer/utils/utilities/_reorganization_planner.pyi"
        ),
        "ReconstructionPlannerUtility": (
            "python/dftracer/utils/utilities/_reconstruction_planner.pyi"
        ),
    }
    rel_path = utility_map.get(top, "python/dftracer/utils/dftracer_utils_ext.pyi")
    return (_docs_dir.parent / rel_path, fullname.split("."))


def _find_symbol_lines(path: Path, parts: list[str]) -> tuple[int, int] | None:
    """Find source lines for a class/function/method in a Python source or stub file."""
    try:
        tree = ast.parse(path.read_text())
    except Exception:
        return None

    node = tree
    current_body = tree.body
    for part in parts:
        match = None
        for child in current_body:
            if isinstance(child, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                if child.name == part:
                    match = child
                    break
        if match is None:
            return None
        node = match
        current_body = getattr(match, "body", [])

    start = getattr(node, "lineno", None)
    end = getattr(node, "end_lineno", start)
    if start is None:
        return None
    return (start, end or start)


def _github_url(path: Path, lines: tuple[int, int] | None) -> str | None:
    """Build a GitHub blob URL for a repo-relative path and optional lines."""
    try:
        rel = path.resolve().relative_to(_docs_dir.parent.resolve()).as_posix()
    except Exception:
        return None
    url = f"{REPO_URL}/blob/{SOURCE_REF}/{rel}"
    if lines is not None:
        start, end = lines
        url += f"#L{start}"
        if end != start:
            url += f"-L{end}"
    return url


def linkcode_resolve(domain: str, info: dict[str, str]) -> str | None:
    """Resolve Python objects to GitHub source links."""
    if domain != "py":
        return None

    module_name = info.get("module")
    fullname = info.get("fullname")
    if not module_name or not fullname:
        return None

    try:
        module = importlib.import_module(module_name)
    except Exception:
        return None

    obj = module
    for part in fullname.split("."):
        obj = getattr(obj, part, None)
        if obj is None:
            return None

    obj_module = getattr(obj, "__module__", module_name)
    if obj_module == "dftracer.utils.dftracer_utils_ext":
        target = _pyi_target_for_extension(fullname)
        if target is None:
            return None
        path, parts = target
        lines = _find_symbol_lines(path, parts)
        return _github_url(path, lines)

    try:
        source_file = Path(inspect.getsourcefile(obj) or inspect.getfile(obj))
        _, start = inspect.getsourcelines(obj)
        end = start + max(len(inspect.getsource(obj).splitlines()) - 1, 0)
        return _github_url(source_file, (start, end))
    except Exception:
        return None


if ON_READTHEDOCS:
    sys.path.insert(0, str(PYTHON_SOURCE_DIR))
    _install_rtd_extension_stub()
    autodoc_mock_imports = [
        "pyarrow",
        "dask",
        "dask.distributed",
    ]

try:
    import dftracer.utils

    print("✓ dftracer.utils package found and imported successfully.")
except (ImportError, ModuleNotFoundError) as e:
    if not ON_READTHEDOCS and PYTHON_SOURCE_DIR.exists():
        print(f"Warning: installed dftracer.utils package not found: {e}")
        print("Falling back to source package with RTD extension stubs.")
        sys.path.insert(0, str(PYTHON_SOURCE_DIR))
        _install_rtd_extension_stub()
        autodoc_mock_imports = [
            "pyarrow",
            "dask",
            "dask.distributed",
        ]
        import dftracer.utils
    else:
        print(f"Warning: dftracer.utils package not found: {e}")
        print("API documentation will have limited information.")
        print("To generate full API docs, install the package: pip install -e .")

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information


project = "dftracer-utils"
copyright = "%Y, Ray Andrew Sinurat, Hariharan Devarajan"
author = "Ray Andrew Sinurat, Hariharan Devarajan"

# The version info for the project
# Try to get version from the package
try:
    from importlib.metadata import version

    release = version("dftracer-utils")
    version = ".".join(release.split(".")[:2])
except Exception:
    version = "0.1"
    release = "0.1.0"

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = [
    "sphinx.ext.autodoc",
    # "sphinx.ext.autosummary",  # Disabled: manual docs in api/reader.rst and api/indexer.rst
    "sphinx.ext.napoleon",
    "sphinx.ext.viewcode",
    "sphinx.ext.intersphinx",
    "sphinx.ext.todo",
    "sphinx.ext.coverage",
    "sphinx.ext.mathjax",
    # sphinx_autodoc_typehints disabled: it strips types from signatures
    # and loses C extension __text_signature__. Sphinx's built-in autodoc
    # handles both Python type hints and C extension __text_signature__.
    "myst_parser",  # For Markdown support
    "breathe",  # Always enable breathe
    "sphinx.ext.ifconfig",  # For conditional inclusion
    "sphinxcontrib.mermaid",  # Mermaid diagrams
]

# Mermaid configuration
mermaid_version = "11"
mermaid_init_js = "mermaid.initialize({startOnLoad:true, theme:'neutral'});"
mermaid_d3_zoom = True

# Check if Doxygen XML output exists and set up Breathe config
doxygen_xml_path = Path(__file__).parent.parent / "doxygen" / "xml"
if doxygen_xml_path.exists():
    cpp_api_enabled = True
    # Breathe configuration for C++ documentation
    breathe_projects = {"dftracer-utils": str(doxygen_xml_path)}
    breathe_default_project = "dftracer-utils"
else:
    cpp_api_enabled = False
    print("Warning: Doxygen XML output not found. C++ API documentation will be skipped.")
    print(f"Expected path: {doxygen_xml_path}")
    print("Run 'doxygen Doxyfile' in the docs directory to generate C++ documentation.")

# Napoleon settings for Google/NumPy style docstrings
napoleon_google_docstring = True
napoleon_numpy_docstring = True
napoleon_include_init_with_doc = True
napoleon_include_private_with_doc = False
napoleon_include_special_with_doc = True
napoleon_use_admonition_for_examples = False
napoleon_use_admonition_for_notes = False
napoleon_use_admonition_for_references = False
napoleon_use_ivar = False
napoleon_use_param = True
napoleon_use_rtype = True
napoleon_preprocess_types = False
napoleon_type_aliases = None
napoleon_attr_annotations = True

# Add mappings for intersphinx - link to main DFTracer docs and Python docs
intersphinx_mapping = {
    "python": ("https://docs.python.org/3", None),
    "dftracer": ("https://dftracer.readthedocs.io/en/latest/", None),
}

templates_path = ["_templates"]
exclude_patterns = ["api/_autosummary"]

# The suffix(es) of source filenames.
source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

# The master toctree document.
master_doc = "index"

# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = "furo"
html_static_path = ["_static"]
html_css_files = ["custom.css"]

# Search configuration
html_search_language = "en"

# Theme options
html_theme_options = {
    "navigation_with_keys": True,
}

# -- Options for autodoc -----------------------------------------------------
autodoc_default_options = {
    "members": True,
    "member-order": "bysource",
    "undoc-members": True,
    "exclude-members": "__weakref__",
}

# Type annotations in both signature and description
autodoc_typehints = "both"
autodoc_typehints_description_target = "documented"

# -- Options for todo extension ----------------------------------------------
todo_include_todos = True

# -- Options for autosummary -------------------------------------------------
autosummary_generate = False
