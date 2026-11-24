# SPDX-License-Identifier: MIT
"""The write verbs as mixins, so the signatures are written once: ``SinkVerbs``
(``sink_trace``, ``sink_table`` and the plugin ``sink``) for ``LazyFrame`` and
``TraceViewer``, ``SessionSinkVerbs`` for ``Session`` and ``WriteVerbs``
(``write_trace``, ``write_table`` and the plugin ``write``) for ``DataFrame``."""

from __future__ import annotations

import os
from typing import TYPE_CHECKING, Any, Literal, Mapping, Sequence, cast, overload

from ._sink import (
    DEFAULT_MAX_OPEN_FILES,
    DEFAULT_MAX_PARTITIONS,
    DEFAULT_MODE,
    DEFAULT_PART_SIZE,
    SinkSummary,
    TraceWriteOptions,
    run_sink,
)

if TYPE_CHECKING:
    from ._dataframe import DataFrame
    from ._lazyframe import LazyFrame, LazyResult
    from ._sink import PathLike, SinkOption
    from .runtime import Runtime
    from .schemas import JsonValue
    from .trace._viewer import Handle, TraceViewer


def _lazy_result(run: Any) -> LazyResult[SinkSummary]:  # noqa: ANN401
    from ._lazyframe import LazyResult

    return LazyResult([], lambda _frames: run())


class SinkVerbs:
    """Write a plan; the class gives ``_sink_target``: the plan, the extra
    ``run_sink`` arguments and the runtime."""

    __slots__ = ()

    if TYPE_CHECKING:

        def _sink_target(self) -> tuple[LazyFrame, dict[str, Any], Runtime | None]: ...

    @overload
    def sink_trace(
        self,
        path: PathLike,
        *,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = ...,
        mode: Literal["error", "overwrite"] = ...,
        part_size: int = ...,
        partition_by: Sequence[str] | None = ...,
        max_open_files: int = ...,
        max_partitions: int = ...,
        lazy: Literal[False] = ...,
    ) -> SinkSummary: ...
    @overload
    def sink_trace(
        self,
        path: PathLike,
        *,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = ...,
        mode: Literal["error", "overwrite"] = ...,
        part_size: int = ...,
        partition_by: Sequence[str] | None = ...,
        max_open_files: int = ...,
        max_partitions: int = ...,
        lazy: Literal[True],
    ) -> LazyResult[SinkSummary]: ...

    def sink_trace(
        self,
        path: PathLike,
        *,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = None,
        mode: Literal["error", "overwrite"] = DEFAULT_MODE,
        part_size: int = DEFAULT_PART_SIZE,
        partition_by: Sequence[str] | None = None,
        max_open_files: int = DEFAULT_MAX_OPEN_FILES,
        max_partitions: int = DEFAULT_MAX_PARTITIONS,
        lazy: bool = False,
    ) -> SinkSummary | LazyResult[SinkSummary]:
        """Write the rows as a DFTracer trace and return a :class:`SinkSummary`;
        nothing is collected into memory. The plan must keep the event envelope
        (``name``, ``cat``, ``pid``, ``tid``, ``ts``, ``ph``, with ``dur`` and
        ``args.<key>`` columns); a missing column raises a :class:`ValueError`
        that names it.

        A path ending in ``.pfw.gz`` or ``.pfw`` is one file, any other path a
        folder of ``part-NNNNN.pfw.gz`` files (plain output is one ``.pfw`` file).
        The output is staged beside ``path`` and renamed into place when it is
        complete, so a failed write leaves nothing at ``path``. ``mode``
        ``"error"`` refuses an existing ``path``, ``"overwrite"`` replaces it.
        The index of the files is built once under the output root unless
        ``options`` says otherwise. ``part_size`` is the rows of one part of a
        folder. ``partition_by`` names the columns whose values pick the Hive
        ``key=value/`` folder of each row: names and values are percent-encoded,
        a null is ``__HIVE_DEFAULT_PARTITION__``, and the key columns stay in
        each part. It needs a folder path, and more than ``max_partitions`` (1024)
        partitions raise.
        ``max_open_files`` is the most part files open at once.
        ``lazy=True`` returns a :class:`LazyResult` for :func:`collect_all` that
        writes when it runs.
        """
        plan, extra, runtime = self._sink_target()

        def run() -> SinkSummary:
            return run_sink(
                plan,
                path,
                kind="events",
                options=options,
                mode=mode,
                part_size=part_size,
                partition_by=partition_by,
                max_open_files=max_open_files,
                max_partitions=max_partitions,
                **extra,
                runtime=runtime,
            )

        if lazy:
            return _lazy_result(run)
        return run()

    @overload
    def sink_table(
        self,
        path: PathLike,
        *,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = ...,
        name: str | None = ...,
        meta: Mapping[str, JsonValue] | None = ...,
        mode: Literal["error", "overwrite"] = ...,
        part_size: int = ...,
        partition_by: Sequence[str] | None = ...,
        max_open_files: int = ...,
        max_partitions: int = ...,
        lazy: Literal[False] = ...,
    ) -> SinkSummary: ...
    @overload
    def sink_table(
        self,
        path: PathLike,
        *,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = ...,
        name: str | None = ...,
        meta: Mapping[str, JsonValue] | None = ...,
        mode: Literal["error", "overwrite"] = ...,
        part_size: int = ...,
        partition_by: Sequence[str] | None = ...,
        max_open_files: int = ...,
        max_partitions: int = ...,
        lazy: Literal[True],
    ) -> LazyResult[SinkSummary]: ...

    def sink_table(
        self,
        path: PathLike,
        *,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = None,
        name: str | None = None,
        meta: Mapping[str, JsonValue] | None = None,
        mode: Literal["error", "overwrite"] = DEFAULT_MODE,
        part_size: int = DEFAULT_PART_SIZE,
        partition_by: Sequence[str] | None = None,
        max_open_files: int = DEFAULT_MAX_OPEN_FILES,
        max_partitions: int = DEFAULT_MAX_PARTITIONS,
        lazy: bool = False,
    ) -> SinkSummary | LazyResult[SinkSummary]:
        """Write the rows as a ``dftu.table`` file (a header line and one typed
        record per line) and return a :class:`SinkSummary`; any plan can be
        written. A streamed plan does not know whether a column holds a null, so
        the header types its integers and booleans as the nullable pandas
        dtypes. ``name`` is the table name (the output's file or folder name by
        default) and ``meta`` its metadata.

        A path ending in ``.pfw.gz`` or ``.pfw`` is one file, any other path a
        folder of ``part-NNNNN.pfw.gz`` files (plain output is one ``.pfw`` file).
        The output is staged beside ``path`` and renamed into place when it is
        complete, so a failed write leaves nothing at ``path``. ``mode``
        ``"error"`` refuses an existing ``path``, ``"overwrite"`` replaces it.
        The index of the files is built once under the output root unless
        ``options`` says otherwise. ``part_size`` is the rows of one part of a
        folder. ``partition_by`` names the columns whose values pick the Hive
        ``key=value/`` folder of each row: names and values are percent-encoded,
        a null is ``__HIVE_DEFAULT_PARTITION__``, and the key columns stay in
        each part. It needs a folder path, and more than ``max_partitions`` (1024)
        partitions raise.
        ``max_open_files`` is the most part files open at once.
        ``lazy=True`` returns a :class:`LazyResult` for :func:`collect_all` that
        writes when it runs.
        """
        plan, extra, runtime = self._sink_target()

        def run() -> SinkSummary:
            return run_sink(
                plan,
                path,
                kind="records",
                options=options,
                name=name,
                meta=meta,
                mode=mode,
                part_size=part_size,
                partition_by=partition_by,
                max_open_files=max_open_files,
                max_partitions=max_partitions,
                **extra,
                runtime=runtime,
            )

        if lazy:
            return _lazy_result(run)
        return run()

    @overload
    def sink(
        self,
        path: PathLike,
        *,
        format: str,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = ...,
        name: str | None = ...,
        meta: Mapping[str, JsonValue] | None = ...,
        mode: Literal["error", "overwrite"] = ...,
        part_size: int = ...,
        partition_by: Sequence[str] | None = ...,
        max_open_files: int = ...,
        lazy: Literal[False] = ...,
    ) -> SinkSummary: ...
    @overload
    def sink(
        self,
        path: PathLike,
        *,
        format: str,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = ...,
        name: str | None = ...,
        meta: Mapping[str, JsonValue] | None = ...,
        mode: Literal["error", "overwrite"] = ...,
        part_size: int = ...,
        partition_by: Sequence[str] | None = ...,
        max_open_files: int = ...,
        lazy: Literal[True],
    ) -> LazyResult[SinkSummary]: ...

    def sink(
        self,
        path: PathLike,
        *,
        format: str,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = None,
        name: str | None = None,
        meta: Mapping[str, JsonValue] | None = None,
        mode: Literal["error", "overwrite"] = DEFAULT_MODE,
        part_size: int = DEFAULT_PART_SIZE,
        partition_by: Sequence[str] | None = None,
        max_open_files: int = DEFAULT_MAX_OPEN_FILES,
        lazy: bool = False,
    ) -> SinkSummary | LazyResult[SinkSummary]:
        """Write the rows with the plugin sink ``format`` and return a
        :class:`SinkSummary`. ``format`` is required: the DFTracer formats have
        :meth:`sink_trace` and :meth:`sink_table`. ``options`` are the plugin's
        text options and ``path`` its ``path`` option; ``name``, ``meta``,
        ``mode``, ``part_size`` and ``max_open_files`` reach the plugin when
        given. ``lazy=True`` returns a :class:`LazyResult` for
        :func:`collect_all` that writes when it runs.
        """
        plan, extra, runtime = self._sink_target()

        def run() -> SinkSummary:
            return run_sink(
                plan,
                path,
                format=format,
                options=options,
                name=name,
                meta=meta,
                mode=mode,
                part_size=part_size,
                partition_by=partition_by,
                max_open_files=max_open_files,
                **extra,
                runtime=runtime,
            )

        if lazy:
            return _lazy_result(run)
        return run()


class SessionSinkVerbs:
    """The sink verbs of a session: each registers the viewer's lazy write."""

    __slots__ = ()

    if TYPE_CHECKING:

        def collect(self, root: Any) -> Handle[Any]: ...  # noqa: ANN401

    def sink_trace(
        self,
        viewer: TraceViewer,
        path: PathLike,
        *,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = None,
        mode: Literal["error", "overwrite"] = DEFAULT_MODE,
        part_size: int = DEFAULT_PART_SIZE,
        partition_by: Sequence[str] | None = None,
        max_open_files: int = DEFAULT_MAX_OPEN_FILES,
        max_partitions: int = DEFAULT_MAX_PARTITIONS,
    ) -> Handle[SinkSummary]:
        """``viewer.sink_trace(path, ...)``, run with the session."""
        return self.collect(
            viewer.sink_trace(
                path,
                options=options,
                mode=mode,
                part_size=part_size,
                partition_by=partition_by,
                max_open_files=max_open_files,
                max_partitions=max_partitions,
                lazy=True,
            )
        )

    def sink_table(
        self,
        viewer: TraceViewer,
        path: PathLike,
        *,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = None,
        name: str | None = None,
        meta: Mapping[str, JsonValue] | None = None,
        mode: Literal["error", "overwrite"] = DEFAULT_MODE,
        part_size: int = DEFAULT_PART_SIZE,
        partition_by: Sequence[str] | None = None,
        max_open_files: int = DEFAULT_MAX_OPEN_FILES,
        max_partitions: int = DEFAULT_MAX_PARTITIONS,
    ) -> Handle[SinkSummary]:
        """``viewer.sink_table(path, ...)``, run with the session."""
        return self.collect(
            viewer.sink_table(
                path,
                options=options,
                name=name,
                meta=meta,
                mode=mode,
                part_size=part_size,
                partition_by=partition_by,
                max_open_files=max_open_files,
                max_partitions=max_partitions,
                lazy=True,
            )
        )

    def sink(
        self,
        viewer: TraceViewer,
        path: PathLike,
        *,
        format: str,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = None,
        name: str | None = None,
        meta: Mapping[str, JsonValue] | None = None,
        mode: Literal["error", "overwrite"] = DEFAULT_MODE,
        part_size: int = DEFAULT_PART_SIZE,
        partition_by: Sequence[str] | None = None,
        max_open_files: int = DEFAULT_MAX_OPEN_FILES,
    ) -> Handle[SinkSummary]:
        """``viewer.sink(path, ...)``, run with the session."""
        return self.collect(
            viewer.sink(
                path,
                format=format,
                options=options,
                name=name,
                meta=meta,
                mode=mode,
                part_size=part_size,
                partition_by=partition_by,
                max_open_files=max_open_files,
                lazy=True,
            )
        )


class WriteVerbs:
    """Write a frame now."""

    __slots__ = ()

    if TYPE_CHECKING:
        num_rows: int

        def lazy(self) -> LazyFrame: ...

    def write_trace(
        self,
        path: PathLike,
        *,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = None,
        mode: Literal["error", "overwrite"] = DEFAULT_MODE,
        part_size: int = DEFAULT_PART_SIZE,
        partition_by: Sequence[str] | None = None,
        max_open_files: int = DEFAULT_MAX_OPEN_FILES,
        max_partitions: int = DEFAULT_MAX_PARTITIONS,
    ) -> SinkSummary:
        """Write this frame to ``path`` now as a DFTracer trace and return a
        :class:`SinkSummary`. The frame must keep the event envelope (``name``,
        ``cat``, ``pid``, ``tid``, ``ts``, ``ph``, with ``dur`` and
        ``args.<key>`` columns); a missing column raises a :class:`ValueError`
        that names it. :meth:`LazyFrame.sink_trace` describes the path, the
        staging, the index and the other arguments."""

        from ._sink import run_sink

        return run_sink(
            self.lazy(),
            path,
            kind="events",
            known_rows=self.num_rows,
            options=options,
            mode=mode,
            part_size=part_size,
            partition_by=partition_by,
            max_open_files=max_open_files,
            max_partitions=max_partitions,
        )

    def write_table(
        self,
        path: PathLike,
        *,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = None,
        name: str | None = None,
        meta: Mapping[str, JsonValue] | None = None,
        mode: Literal["error", "overwrite"] = DEFAULT_MODE,
        part_size: int = DEFAULT_PART_SIZE,
        partition_by: Sequence[str] | None = None,
        max_open_files: int = DEFAULT_MAX_OPEN_FILES,
        max_partitions: int = DEFAULT_MAX_PARTITIONS,
    ) -> SinkSummary:
        """Write this frame to ``path`` now as a ``dftu.table`` file (a header
        line and one typed record per line) and return a :class:`SinkSummary`.
        :meth:`LazyFrame.sink_table` describes the path, the staging, the index
        and the other arguments. The header holds the frame's row count and
        column dtypes: a column that holds a null is written with a nullable
        dtype, a dictionary column as ``category``, a string column as
        ``object``, and a frame that a table read returned keeps its table
        header, so the file reads back to the same pandas frame. ``name`` and
        ``meta`` replace the ones the frame carries."""

        from ._sink import output_stem, run_sink
        from ._table import _write_header

        header, raw = _write_header(
            cast("DataFrame", self),
            name,
            output_stem(os.fspath(path)),
            dict(meta) if meta is not None else None,
        )
        return run_sink(
            self.lazy(),
            path,
            kind="records",
            header=header,
            known_rows=self.num_rows,
            raw_columns=tuple(raw),
            options=options,
            name=name,
            meta=meta,
            mode=mode,
            part_size=part_size,
            partition_by=partition_by,
            max_open_files=max_open_files,
            max_partitions=max_partitions,
        )

    def write(
        self,
        path: PathLike,
        *,
        format: str,
        options: TraceWriteOptions | Mapping[str, SinkOption] | None = None,
        name: str | None = None,
        meta: Mapping[str, JsonValue] | None = None,
        mode: Literal["error", "overwrite"] = DEFAULT_MODE,
        part_size: int = DEFAULT_PART_SIZE,
        partition_by: Sequence[str] | None = None,
        max_open_files: int = DEFAULT_MAX_OPEN_FILES,
    ) -> SinkSummary:
        """Write this frame to ``path`` now with the plugin sink ``format`` and
        return a :class:`SinkSummary`. ``format`` is required: the DFTracer
        formats have :meth:`write_trace` and :meth:`write_table`.
        :meth:`LazyFrame.sink` describes the arguments."""

        from ._sink import run_sink

        return run_sink(
            self.lazy(),
            path,
            format=format,
            options=options,
            name=name,
            meta=meta,
            mode=mode,
            part_size=part_size,
            partition_by=partition_by,
            max_open_files=max_open_files,
        )
