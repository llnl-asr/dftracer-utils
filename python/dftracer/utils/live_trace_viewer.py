# SPDX-License-Identifier: MIT
"""``LiveTraceViewer``: tracks gzip traces that are still being written. The work
is native (``trace::views::LiveView``); this is its Python adaptor.
"""

from __future__ import annotations

import os
from typing import Any, List, NamedTuple, Optional, Sequence, Union

from . import dftracer_utils_ext as _ext
from .schemas import schema_id
from .trace_viewer import TraceViewer


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
    sequence of paths. ``index_path`` is as for ``TraceViewer``. The files are
    not copied. This object holds no query ops: :meth:`snapshot` returns a normal
    ``TraceViewer``, which keeps its rows after later snapshots.
    """

    __slots__ = ("_live", "_runtime", "_record_schema")

    def __init__(
        self,
        files: Union[str, "os.PathLike[str]", Sequence[Union[str, "os.PathLike[str]"]]],
        index_path: Optional[str] = None,
        runtime: Any = None,
        record_schema: Any = None,
    ) -> None:
        paths = (
            [os.fspath(files)]
            if isinstance(files, (str, os.PathLike))
            else [os.fspath(f) for f in files]
        )
        self._live = _ext._LiveTraceViewer(paths, index_path)
        self._runtime = runtime
        self._record_schema = record_schema

    def snapshot(self) -> TraceViewer:
        """Add the members written since the last call to the index, once each,
        and return a viewer of every whole member. Each file stops at its first
        short or bad member."""
        native = self._live.snapshot()
        if self._record_schema is not None:
            native = native.record_schema(schema_id(self._record_schema))
        viewer = TraceViewer(native)
        viewer._runtime = self._runtime
        return viewer

    def status(self) -> List[LiveFileStatus]:
        """One entry per file, from the last :meth:`snapshot`."""
        return [LiveFileStatus(*entry) for entry in self._live.status()]
