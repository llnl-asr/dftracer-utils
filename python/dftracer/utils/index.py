# SPDX-License-Identifier: MIT
"""Indexer utilities for building and managing trace indexes."""

import json
import os
from dataclasses import dataclass, field
from types import TracebackType
from typing import (
    TYPE_CHECKING,
    Dict,
    List,
    Mapping,
    Optional,
    Sequence,
    Set,
    Type,
    TypedDict,
    Union,
    cast,
    overload,
)

from ._dataframe import DataFrame as _DataFrame
from ._enums import coerce_bytes as _coerce_bytes
from ._enums import coerce_duration as _coerce_duration
from ._public import set_home as _set_home
from .dftracer_utils_ext import CheckpointIndexer as CheckpointIndexer
from .dftracer_utils_ext import CheckpointIndexer as _NativeCheckpointIndexer
from .dftracer_utils_ext import Index as _NativeIndex
from .dftracer_utils_ext import Indexer as _NativeIndexer
from .dftracer_utils_ext import _hash_value
from .dftracer_utils_ext import consolidate_shard_set as consolidate_shard_set
from .dftracer_utils_ext import write_shard_set as write_shard_set
from .runtime import Runtime as _Runtime
from .runtime import TaskHandle as _TaskHandle
from .schemas import RecordSchema as _RecordSchema
from .schemas import schema_id as _schema_id

if TYPE_CHECKING:
    from .dftracer_utils_ext import IndexStatusDict

__all__ = [
    "AggregationConfig",
    "BloomConfig",
    "CheckpointIndexer",
    "ExplainEntry",
    "ExplainExtension",
    "ExplainSummary",
    "Index",
    "IndexStatus",
    "Indexer",
    "ManifestEntry",
    "ManifestExtension",
    "consolidate_shard_set",
    "hash_value",
    "write_shard_set",
]
_DEFAULT_CHECKPOINT_SIZE = 32 * 1024 * 1024  # 32MB

_FileInfo = Dict[str, Set[int]]


@dataclass
class AggregationConfig:
    """Configuration for the aggregation tier every index build makes.

    The defaults are the standard config: 1 s windows with the percentile
    sketches on. A caller may change any field, but the tier is always built.

    Attributes:
        time_interval_ms: Time bucket size in milliseconds (default 1000).
        group_keys: Extra grouping dimensions (default None).
        compute_percentiles: Collect percentile sketches (default True).
        group_by_file: Keep the file hash in the aggregation key (default True).
            A trace touching millions of files makes that key nearly as fine as
            the events themselves; turning it off collapses the rows and counts
            distinct files with a sketch instead (`file_nunique`).
    """

    time_interval_ms: Union[float, str] = 1000.0
    group_keys: Optional[List[str]] = None
    compute_percentiles: bool = True
    group_by_file: bool = True


@dataclass
class BloomConfig:
    """Configuration for the bloom tier, which lets a filter skip chunks.

    Attributes:
        fields: Args fields to index by name besides name/cat/pid/tid and the
            file, host and command hashes, named as in filters (``"size"``,
            ``"args.size"`` or a nested ``"io.size"``). Each gets a bloom
            filter (equality filters) and min/max (range filters) per chunk.
            An existing index without one of these fields is rebuilt.
        path_budget: Also index each file's other args paths, most frequent
            first, while their evidence fits ``stats_share`` and, when this is
            above 0, at most ``path_budget`` of them: a number gets a
            per-chunk min/max, a string a per-chunk bloom while the chunk
            holds at most ``auto_max_distinct`` of its values (else it keeps
            no bloom there). A path without evidence is still queryable but
            never skips a chunk. 0 = no count limit; an existing index built
            with another budget is rebuilt.
        stats_share: The share of a file's compressed size its automatic
            evidence may use, in (0, 1], at least 8 MiB. An existing index
            built with another share is rebuilt. A record schema's
            ``index.stats_share`` and ``index.path_budget`` override both.
        auto_max_distinct: The per-chunk distinct-value cap for automatic
            string blooms.
        false_positive_rate: Bloom false-positive rate, in (0, 1).
        expected_entries: Expected distinct values per chunk, which sizes
            each bloom filter.
    """

    fields: List[str] = field(default_factory=list)
    path_budget: int = 0
    stats_share: float = 0.05
    auto_max_distinct: int = 256
    false_positive_rate: float = 0.01
    expected_entries: int = 1024


class ManifestExtension(TypedDict):
    """One extension built for an indexed file (``Indexer.manifest``)."""

    name: str
    version: int
    params_hash: str
    ready: bool
    current: bool


class ManifestEntry(TypedDict):
    """The extensions built for one indexed file (``Indexer.manifest``)."""

    path: str
    index_dir: str
    extensions: List[ManifestExtension]


class ExplainExtension(TypedDict):
    """What one extension rules out of a query (``Indexer.explain``)."""

    name: str
    file_ruled_out: bool
    removed: List[int]


class ExplainSummary(TypedDict):
    """The chunks one summary of one path skips (``Indexer.explain``)."""

    path: str
    summary: str
    skipped: int
    chunks: int


class ExplainEntry(TypedDict):
    """Why a query reads the chunks it reads in one file (``Indexer.explain``)."""

    path: str
    indexed: bool
    may_match: bool
    chunks: int
    read: List[int]
    extensions: List[ExplainExtension]
    summaries: List[ExplainSummary]


@dataclass
class IndexStatus:
    """Status of index resolution.

    Attributes:
        total_files: Total number of files discovered.
        ready: Files that are fully indexed for requested tiers.
        needs_work: Files that need indexing.
        index_dir: Path to the .dftindex store.
        aggregation_interval_us: Time interval (us) of the cached aggregation
            tier, or 0 if none.
        truncated: Ready files whose last gzip member was cut short; their
            recovered tail is not checksum-verified.
    """

    total_files: int
    ready: List[str] = field(default_factory=list)
    needs_work: List[str] = field(default_factory=list)
    index_dir: str = ""
    aggregation_interval_us: int = 0
    truncated: List[str] = field(default_factory=list)

    @classmethod
    def _from_dict(cls, result: "IndexStatusDict") -> "IndexStatus":
        """Build from the native status()/ensure()/build() result dict."""
        return cls(
            total_files=result["total_files"],
            ready=result["ready"],
            needs_work=result["needs_work"],
            index_dir=result.get("index_dir", ""),
            aggregation_interval_us=result.get("aggregation_interval_us", 0),
            truncated=result.get("truncated", []),
        )


class Indexer:
    """High-level indexer for building and managing trace indexes.

    Supports tiered indexing:
    - Tier 1: Checkpoints (for random access)
    - Tier 2: Bloom filters (for fast filtering)
    - Tier 3: Aggregation data, built by every build

    Args:
        files: A path (``str`` or ``os.PathLike``) or a sequence of paths,
            with the same rule as :class:`~dftracer.utils.trace.TraceViewer`: each
            path is a file, a directory (scanned recursively for trace gzip
            files, ``*.gz``) or a glob (``*``, ``?``, ``[...]``, ``**``), and a
            sequence may mix them. The files are those of the paths in order;
            with several paths each file appears once (by device and inode).
            A path that exists is taken as it is, even when its name has a
            wildcard; a glob that matches nothing adds no
            file. A missing path with no wildcard raises
            ``DFTUtilsNotFoundError`` on the first ``status``, ``ensure`` or
            ``build`` (``TraceViewer`` raises it at ``collect()``, as it
            reads nothing before). An empty sequence raises ``ValueError``,
            since there is nothing to index (``TraceViewer`` takes it as a
            view of no rows); an empty directory or a glob with no match is
            an index of no files. A file that is not a trace raises
            ``DFTUtilsIOError`` when ``ensure`` or ``build`` indexes it;
            ``status`` reads no content, so it does not.
        index_dir: Directory for .dftindex stores (default: next to files).
        require_checkpoint: Build checkpoint tier (default True).
        require_bloom: Build the bloom tier: True for the defaults, or a
            BloomConfig naming the args fields to index (default True).
        build_bloom: Build the bloom/stats/dimension tier (default True). Off
            for aggregation-only consumers that never read it.
        aggregation_config: Settings of the aggregation tier, which every build
            builds (default: the standard config).
        parallelism: Number of parallel workers (0 = all cores).
        runtime: Runtime for executor parallelism (default: global runtime).
        extensions: Pruning extensions the bloom tier builds, from
            "zonemap", "bloom", "counts" and "postings" (default all four).
        schema: Record schema of every trace, a registered id or a
            :class:`dftracer.utils.schemas.RecordSchema` class; None detects
            each file's from its first lines. A generic trace is indexed by
            exact JSON path, so a filter on any field can skip chunks.
        memory_budget: Bytes the build may hold at once, as for
            ``TraceViewer.memory_budget`` (an int or a size such as "4GB");
            0 is about a third of available memory. Past it, large files are
            indexed fewer at a time and spill to disk.
        summaries: Per path, the summaries it is indexed with, in place of the
            defaults: ``{"args.fhash": ["bloom"], "size": ["zonemap"]}``. A
            path is named as in filters (``"size"``, ``"args.size"`` or a
            nested ``"io.size"``); a path that is not a fixed field is
            indexed by name. Each list holds "zonemap", "bloom" or "counts",
            once each, each among ``extensions``, or the name of a summary
            compiled by :func:`dftracer.utils.jit.summary` that takes the
            path and is loaded; a path not listed keeps every summary of
            ``extensions``. A change of
            ``summaries`` rebuilds the pruning tier of the indexed files. An
            empty or repeated path or an unknown summary raises
            ``DFTUtilsValueError`` naming it on the first ``status``,
            ``ensure`` or ``build``; a bare ``str`` in place of a list raises
            ``TypeError``.
        format: The reader of every file by name: ``"jsonl"`` (the built-in
            trace reader) or a registered plugin file format. None picks one
            per file by content; two readers that tie are an error naming both.
            An unknown name raises ``DFTUtilsValueError`` listing the known ones.

    Example:
        >>> indexer = Indexer("/path/to/traces")
        >>> indexer.ensure()  # builds checkpoint, bloom; no-op when fresh

        >>> # With explicit file list
        >>> indexer = Indexer(["/path/to/trace1.pfw.gz", "/path/to/trace2.pfw.gz"])
        >>> indexer.ensure()

        >>> # Rebuild even when the index is fresh
        >>> indexer.build(force=True)

        >>> # With a customized aggregation
        >>> indexer = Indexer(
        ...     "/path/to/traces",
        ...     aggregation_config=AggregationConfig(time_interval_ms=5000),
        ... )
        >>> indexer.ensure()  # fused pass with aggregation
    """

    def __init__(
        self,
        files: "Union[str, os.PathLike[str], Sequence[Union[str, os.PathLike[str]]]]",
        index_dir: str = "",
        require_checkpoint: bool = True,
        require_bloom: Union[bool, BloomConfig] = True,
        build_bloom: bool = True,
        aggregation_config: Optional[AggregationConfig] = None,
        checkpoint_size: Union[int, str] = _DEFAULT_CHECKPOINT_SIZE,
        parallelism: int = 0,
        runtime: Optional[_Runtime] = None,
        extensions: Optional[Sequence[str]] = None,
        memory_budget: Union[int, str] = 0,
        schema: "Union[str, Type[_RecordSchema], None]" = None,
        format: Optional[str] = None,
        summaries: Optional[Mapping[str, Sequence[str]]] = None,
    ) -> None:
        checkpoint_size = _coerce_bytes(checkpoint_size, "checkpoint_size")
        if summaries is not None and not isinstance(summaries, Mapping):
            raise TypeError("summaries must be a mapping of path to a list of summary names")

        agg_config = aggregation_config if aggregation_config is not None else AggregationConfig()

        bloom = require_bloom if isinstance(require_bloom, BloomConfig) else BloomConfig()

        # Build native indexer
        native_runtime = runtime._native if runtime else None
        self._native = _NativeIndexer(
            files=files,
            index_dir=index_dir,
            require_checkpoint=require_checkpoint,
            require_bloom=bool(require_bloom),
            build_bloom=build_bloom,
            time_interval_ms=_coerce_duration(agg_config.time_interval_ms, 1e3, "time_interval_ms"),
            group_keys=agg_config.group_keys,
            compute_percentiles=agg_config.compute_percentiles,
            group_by_file=agg_config.group_by_file,
            checkpoint_size=checkpoint_size,
            parallelism=parallelism,
            runtime=native_runtime,
            bloom_fields=bloom.fields,
            false_positive_rate=bloom.false_positive_rate,
            expected_entries=bloom.expected_entries,
            path_budget=bloom.path_budget,
            stats_share=bloom.stats_share,
            auto_max_distinct=bloom.auto_max_distinct,
            extensions=list(extensions) if extensions is not None else None,
            memory_budget=_coerce_bytes(memory_budget, "memory_budget"),
            schema=_schema_id(schema) if schema is not None else None,
            format=format,
            summaries=dict(summaries) if summaries is not None else None,
        )
        self._aggregation_config = agg_config
        self._file_info_cache: Optional[_FileInfo] = None
        self._closed = False

    def __enter__(self) -> "Indexer":
        return self

    def __exit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Optional[TracebackType],
    ) -> None:
        self.close()

    def close(self) -> None:
        """Release resources."""
        self._closed = True

    @property
    def aggregation_config(self) -> AggregationConfig:
        """The settings the aggregation tier is built with."""
        return self._aggregation_config

    def status(self) -> IndexStatus:
        """Check which files are indexed and which need work. Builds nothing.

        Returns:
            IndexStatus with total_files, ready, and needs_work lists.
        """
        return IndexStatus._from_dict(self._native.status())

    def ensure(self) -> IndexStatus:
        """Build what is missing or stale; a no-op when the index is fresh.

        This method builds indexes in parallel using the Runtime executor.
        When aggregation is enabled, it performs a fused pass for efficiency.

        Returns:
            IndexStatus after building.
        """
        return IndexStatus._from_dict(self._native.ensure())

    def build(self, force: bool = False) -> IndexStatus:
        """Build the missing index tiers based on require_* flags.

        Args:
            force: Rebuild every tier even when the index is fresh.

        Returns:
            IndexStatus after building.
        """
        return IndexStatus._from_dict(self._native.build(force=force))

    def build_async(self, force: bool = False) -> _TaskHandle[IndexStatus]:
        """Start :meth:`build` on the native pool and return at once.

        ``get()`` on the handle returns the :class:`IndexStatus` that ``build``
        returns when the build ends, or raises the error ``build`` would, and
        releases the GIL while it waits. Opening the files happens in the task,
        so a bad path fails there too. A dropped handle does not stop the build,
        and a :meth:`build` of the same index that starts meanwhile waits for it.
        """
        native = self._native.build_async(force=force)
        return _TaskHandle(
            native=native,
            name=native.name,
            task_id=native.task_id,
            finish=IndexStatus._from_dict,
        )

    def manifest(self) -> List[ManifestEntry]:
        """The extensions built for each indexed file.

        Returns:
            One dict per file with ``path``, ``index_dir`` and
            ``extensions``: dicts with ``name``, ``version``, ``params_hash``
            (16 hex digits), ``ready`` and ``current``.
        """
        return cast("List[ManifestEntry]", json.loads(self._native.manifest()))

    def explain(self, duql: str) -> List[ExplainEntry]:
        """Why a query reads the chunks it reads, per file. Writes nothing.

        Returns:
            One dict per file with ``path``, ``indexed``, ``may_match``,
            ``chunks``, ``read`` (the chunks a scan reads), ``extensions``:
            dicts with ``name``, ``file_ruled_out`` and ``removed`` (the
            chunks that extension rules out alone), and ``summaries``: dicts
            with ``path``, ``summary``, ``skipped`` and ``chunks``, one for
            each summary of each filtered path that rules out a chunk when it
            judges only that path.

        Raises:
            DFTUtilsValueError: The duql text does not parse.
        """
        return cast("List[ExplainEntry]", json.loads(self._native.explain(duql)))

    def explain_text(self, duql: str) -> str:
        """:meth:`explain` as text: per file the chunks read, then a line
        ``summary <path> <kind>: skipped K of N chunks`` for each summary that
        rules chunks out."""
        return self._native.explain_text(duql)

    def summaries(self, file: "Union[str, os.PathLike[str]]") -> _DataFrame:
        """The summaries stored for an indexed file, as :meth:`Index.summaries`
        returns them.

        Raises:
            DFTUtilsNotFoundError: The index holds no entry for `file`.
        """
        return Index(self.status().index_dir).summaries(file)

    def rebuild_extension(self, name: str) -> IndexStatus:
        """Rewrite one tier extension of every file, leaving the others.

        Args:
            name: "zonemap", "bloom", "counts", "postings", "dft.stats",
                "dft.metadata", or the name of an index extension that a
                loaded :class:`~dftracer.utils.plugins.Plugins` set
                registered.
        """
        return IndexStatus._from_dict(self._native.rebuild_extension(name))

    def drop_extension(self, name: str) -> IndexStatus:
        """Remove one tier extension from every indexed file.

        The next build makes it again only when ``extensions`` names it, or
        for a plugin extension while its plugin set is loaded.

        Args:
            name: As for :meth:`rebuild_extension`.
        """
        return IndexStatus._from_dict(self._native.drop_extension(name))

    def get_checkpoint_indexer(self, file_path: str) -> _NativeCheckpointIndexer:
        """Get a checkpoint indexer for a specific file.

        Returns an indexer for checkpoint-level operations on a single file,
        such as finding checkpoints for random access.

        Args:
            file_path: Path to the trace file (.pfw/.pfw.gz).

        Returns:
            Indexer instance for checkpoint operations (checkpoints, find_checkpoint, etc).
        """
        return self._native.get_checkpoint_indexer(file_path)

    def rowset(self, name: str) -> "_DataFrame":
        """The rows the index build stored for row set `name` of the files'
        source, every indexed file's in order.

        The dftracer source stores ``files`` (``fhash``, ``path``), ``hosts``
        (``hhash``, ``name``), ``strings`` (``shash``, ``value``) and
        ``ranks`` (``pid``, ``rank``).

        Raises:
            DFTUtilsValueError: If an index holds no rows for `name` (a row set the
                build does not evaluate, or an index built before it); read
                it with ``TraceViewer.duql("from <name>")``.

        Example:
            >>> indexer = Indexer("/path/to/traces")
            >>> indexer.ensure()
            >>> files = indexer.rowset("files").to_dict()
            >>> # files = {"fhash": ["abc123", ...], "path": ["/data.h5", ...]}
        """
        return _DataFrame(self._native.rowset(name))

    def query_file_pids(self, path: str) -> Set[int]:
        """Query PIDs observed in a specific file.

        Args:
            path: Path of the trace file, as given to the indexer.

        Returns:
            set of PIDs (int) observed in the file, empty when the index
            holds no entry for the file.
        """
        return self._native.query_file_pids(path)

    def query_all_file_pids(self) -> Dict[str, Set[int]]:
        """Query PIDs for all indexed files.

        Returns a dictionary mapping trace path to the set of PIDs observed
        in that file. This is useful for distributed aggregation to assign
        files to workers by PID affinity.

        Returns:
            dict mapping trace path (str) to set of PIDs (int).
        """
        return self._native.query_all_file_pids()

    def query_file_info(self) -> _FileInfo:
        """Query every indexed file with its PIDs in a single DB open.

        Returns:
            dict mapping trace path (str) to set of PIDs (int).
        """
        if self._file_info_cache is None:
            self._file_info_cache = self._native.query_file_info()
        return self._file_info_cache


class Index:
    """A read-only handle to the index root of an :class:`Indexer`.

    Args:
        index_dir: The ``.dftindex`` store, as ``IndexStatus.index_dir`` names
            it.
    """

    def __init__(self, index_dir: str) -> None:
        self._native = _NativeIndex(index_dir)

    def summaries(self, file: "Union[str, os.PathLike[str]]") -> _DataFrame:
        """What the index stores about the chunks of one trace file.

        Args:
            file: The trace path, as given to the indexer.

        Returns:
            A DataFrame with one row per stored summary per chunk and the
            columns ``path`` (the indexed path), ``chunk``, ``summary``
            ("zonemap", "bloom", "counts" or "postings"), ``bytes`` (the
            stored size; for postings, 8 bytes per hash) and ``hash_version``
            (the value hash version of a bloom filter or a posting list, null
            for a summary that holds no hash).

        Raises:
            DFTUtilsNotFoundError: The index holds no entry for `file`.
        """
        return _DataFrame(self._native.summaries(os.fspath(file)))


@overload
def hash_value(x: None) -> int: ...
@overload
def hash_value(x: bool) -> int: ...
@overload
def hash_value(x: int) -> int: ...
@overload
def hash_value(x: float) -> int: ...
@overload
def hash_value(x: str) -> int: ...
@overload
def hash_value(x: bytes) -> int: ...
def hash_value(x: Union[None, bool, int, float, str, bytes]) -> int:
    """The canonical typed hash the index builds its filters and postings with.

    Every integer hashes as an int64 (an int above int64 as a uint64), a float
    that is a whole number in int64 range hashes as that integer (``42.0`` and
    ``42`` agree, ``-0.0`` and ``0.0`` agree), every NaN is one value, a bool
    hashes as 0 or 1, a ``str`` hashes as its UTF-8 bytes and ``bytes`` as the
    same bytes, so ``"a"`` and ``b"a"`` agree, and a string never equals a
    number. ``None`` is the null value. It is the value a plugin gets from
    ``dftu_svc_index.hash_value``.

    Raises:
        OverflowError: An int outside int64 and uint64.
        TypeError: Any other type.
    """
    return _hash_value(x)


_set_home(__name__, globals(), __all__)
