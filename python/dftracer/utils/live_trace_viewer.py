"""``LiveTraceViewer``: a :class:`~dftracer.utils.TraceViewer` over traces that
are still being written. The work is native (``trace::views::LiveView``); this
is its Python adaptor.

Only whole gzip members, CRC checked, are read: a file is read up to its first
unfinished tail or hole, and ``refresh()`` or a new viewer picks up what has
been finished or filled since. Every ``TraceViewer`` capability is inherited.
"""

from __future__ import annotations

import os
from typing import Any, List, Optional, Sequence, Tuple, Union

from . import dftracer_utils_ext as _ext
from .schemas import schema_id
from .trace_viewer import TraceViewer


class LiveTraceViewer(TraceViewer):
    """A :class:`TraceViewer` over gzip trace files that may still be growing.

    ``files`` is a path, a directory scanned recursively for gzip traces, or a
    sequence of paths. ``index_path`` is as for ``TraceViewer``, over the
    snapshots. ``snapshot_dir`` holds a copy of the verified bytes and needs
    room for it; only one viewer at a time may use it. By default it is a
    private temporary directory removed at process exit.

    After a read: ``verified_bytes`` is the bytes of whole members read,
    ``skipped_tail_bytes`` the bytes from each file's stop point to its end,
    and ``holes`` the ``(path, offset, length)`` where a file was cut short.
    """

    __slots__ = ("_live", "_options")

    def __init__(
        self,
        files: Union[str, "os.PathLike[str]", Sequence[Union[str, "os.PathLike[str]"]]],
        index_path: Optional[str] = None,
        runtime: Any = None,
        record_schema: Any = None,
        snapshot_dir: Optional[str] = None,
    ) -> None:
        paths = (
            [os.fspath(files)]
            if isinstance(files, (str, os.PathLike))
            else [os.fspath(f) for f in files]
        )
        self._live = _ext._LiveTraceViewer(paths, index_path, snapshot_dir)
        self._options = (runtime, record_schema)
        self.refresh()

    def refresh(self) -> "LiveTraceViewer":
        """Read the files again in place: rows of members written since, or of
        a filled hole, now appear, and so do new files of a directory. Ops
        applied to this object are dropped. Returns ``self``."""
        runtime, record_schema = self._options
        native = self._live.refresh()
        if record_schema is not None:
            native = native.record_schema(schema_id(record_schema))
        TraceViewer.__init__(self, native)
        self._runtime = runtime
        return self

    @property
    def verified_bytes(self) -> int:
        return self._live.verified_bytes()

    @property
    def skipped_tail_bytes(self) -> int:
        return self._live.skipped_tail_bytes()

    @property
    def holes(self) -> List[Tuple[str, int, int]]:
        return self._live.holes()
