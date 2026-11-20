# SPDX-License-Identifier: MIT
"""``LiveTraceViewer``: tracks gzip traces that are still being written. The work
is native (``trace::views::LiveView``); this is its Python adaptor.
"""

from __future__ import annotations

import os
import time
from typing import TYPE_CHECKING, Iterator, NamedTuple, Sequence

from .. import dftracer_utils_ext as _ext
from ..schemas import RecordSchema, schema_id
from ._viewer import TraceViewer

if TYPE_CHECKING:
    from ..runtime import Runtime


class LiveFileStatus(NamedTuple):
    """One file as the last :meth:`LiveTraceViewer.snapshot` found it.

    ``state`` is ``"whole"`` when every byte is in a whole gzip member, ``"tail"``
    when the file ends in a member that is short or bad, and ``"hole"`` when bytes
    that are not a member come before a whole member. ``hole_bytes`` is the size
    of that gap.
    """

    path: str
    state: str
    verified_bytes: int
    skipped_bytes: int
    hole_bytes: int


class LiveTraceViewer:
    """Tracks gzip trace files that may still be growing.

    ``files`` is a path, a directory scanned recursively for gzip traces, or a
    sequence of paths. ``index_dir`` is as for ``TraceViewer``. The files are
    not copied. This object holds no query ops: :meth:`snapshot` returns a normal
    ``TraceViewer``, which keeps its rows after later snapshots.
    """

    __slots__ = ("_live", "_runtime", "_record_schema")

    def __init__(
        self,
        files: str | os.PathLike[str] | Sequence[str | os.PathLike[str]],
        index_dir: str | None = None,
        runtime: Runtime | None = None,
        record_schema: str | type[RecordSchema] | None = None,
    ) -> None:
        paths = (
            [os.fspath(files)]
            if isinstance(files, (str, os.PathLike))
            else [os.fspath(f) for f in files]
        )
        self._live = _ext._LiveTraceViewer(paths, index_dir)
        self._runtime = runtime
        self._record_schema = record_schema

    def snapshot(self) -> TraceViewer:
        """Add the members written since the last call to the index, once each,
        and return a viewer of every whole member. Each file stops at its first
        short or bad member."""
        return self._viewer(self._live.snapshot())

    def new_rows(self) -> TraceViewer | None:
        """As :meth:`snapshot`, and return a viewer of only the rows that no
        earlier call of this method returned, or ``None`` when there are none. A
        file that a new file replaced starts over. A file whose last whole member
        ends inside a line is held back until a later member ends that line."""
        native = self._live.new_rows()
        return None if native is None else self._viewer(native)

    def follow(self, interval: float = 1.0) -> Iterator[TraceViewer]:
        """Yield a viewer of the new rows each time there are some, waiting
        ``interval`` seconds between looks when there are none. It does not end:
        stop iterating to stop following. No background thread runs."""
        if interval <= 0:
            raise ValueError("interval must be positive")
        while True:
            viewer = self.new_rows()
            if viewer is None:
                time.sleep(interval)
            else:
                yield viewer

    def _viewer(self, native: _ext.TraceViewer) -> TraceViewer:
        if self._record_schema is not None:
            native = native.record_schema(schema_id(self._record_schema))
        viewer = TraceViewer(native)
        viewer._runtime = self._runtime
        return viewer

    def status(self) -> list[LiveFileStatus]:
        """One entry per file, from the last :meth:`snapshot`."""
        return [LiveFileStatus(*entry) for entry in self._live.status()]
