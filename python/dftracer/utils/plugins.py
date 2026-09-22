"""Build and run compiled DFTracer analysis plugins from Python.

A plugin is a compiled shared library exporting the ``dftracer_plugin`` ABI
symbol. :class:`Plugins` builds a fixed set of them (dlopen, ABI gate,
capability resolution) at construction, then runs the set as folds over a
single fused parallel scan of a trace set.

Example::

    from dftracer.utils.plugins import Plugins

    plugins = Plugins(["process_counts.so", MyPlugin])  # .so path, or a
                                                          # @jit.plugin class
    run = plugins.run("./traces")
    print(run.results["process_counts"])   # the plugin's emitted bytes
    print(run.stats["events_scanned"])
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import TYPE_CHECKING, Dict, List, Optional, Union

from .dataframe import DataFrame
from .dftracer_utils_ext import Plugins as _NativePlugins
from .dftracer_utils_ext import _DataFrame

if TYPE_CHECKING:
    import pyarrow as pa  # ty: ignore[unresolved-import]

    from .lazyframe import LazyFrame
    from .runtime import Runtime

__all__ = ["Plugins", "PluginRun", "unnest"]

# A JSON-serializable value tree (config is handed straight to json.dumps).
JSONValue = Union[str, int, float, bool, None, List["JSONValue"], Dict[str, "JSONValue"]]

# One run() result value per emit kind: bytes, a native frame, a lazy plan, or
# an Arrow table.
_RunResult = Union[bytes, "DataFrame", "LazyFrame", "pa.Table"]


def unnest(
    result: "Union[pa.Table, pa.RecordBatchReader, pa.RecordBatch]",
    column: str,
    keep_empty: bool = False,
) -> "pa.Table":
    """Explode a list-typed ``column`` of a ``run()`` map result into one row per
    element, repeating the other columns; the inverse of the set/list/top-k
    aggregates.

    ``result`` is any ``run()`` map value (an eager Arrow table, a
    ``pyarrow.RecordBatchReader``, or a ``pyarrow.RecordBatch``). ``column`` names
    a ``list<utf8>``, ``list<int64>``, or ``list<struct<...>>`` column; a
    ``list<struct>`` flattens its fields into columns (named by the struct
    fields). Returns a ``pyarrow.Table``.

    An empty or null list drops the row (SQL inner unnest). With
    ``keep_empty=True`` it emits one row whose exploded column(s) are null (SQL
    outer/left unnest). Row order is input order, then element order within a row.
    """
    import pyarrow as pa

    if isinstance(result, pa.RecordBatchReader):
        result = result.read_all()
    return DataFrame.from_arrow(pa.table(result)).unnest(column, keep_empty).to_arrow()


def _plugin_key(plugin: Union[str, type]) -> str:
    """The name a caller's ``config`` mapping keys this plugin by: a compiled
    ``.so``'s stem, or a ``@jit.plugin`` class's name."""
    if isinstance(plugin, str):
        return Path(plugin).stem
    return getattr(plugin, "__name__", str(plugin))


class PluginRun:
    """The named results and scan counters of one :meth:`Plugins.run` call.

    ``results`` is ``{name: value}`` of every plugin's emitted results;
    ``stats`` is the scan counters ``{"events_scanned", "events_matched"}``.
    """

    __slots__ = ("results", "stats")

    def __init__(self, results: "Dict[str, _RunResult]", stats: Dict[str, int]) -> None:
        self.results = results
        self.stats = stats


class Plugins:
    """A fixed, built set of compiled DFTracer plugins.

    Construction builds the set immediately: dlopen, the ABI gate, and
    capability resolution all run in ``__init__``, so a bad path, an ABI
    mismatch, or a name collision raises ImportError there rather than from
    :meth:`run`. The set is immutable once built.
    """

    __slots__ = ("_native",)

    def __init__(
        self,
        plugins: "List[Union[str, type]]",
        config: "Optional[Dict[str, Dict[str, JSONValue]]]" = None,
        runtime: "Optional[Runtime]" = None,
    ) -> None:
        """Build ``plugins`` (each a compiled ``.so`` path or a ``@jit.plugin``
        class, AST-compiled to a cached native ``.so`` first) into one set bound
        to ``runtime`` (or the module default).

        ``config`` is an optional ``{plugin_key: {...}}`` mapping, keyed by
        :func:`_plugin_key` (a ``.so``'s filename stem, or a jit class's name);
        each value is a JSON-serializable dict handed to that plugin's factory.
        Raises ImportError on a load/symbol/ABI/capability failure, or
        DFTUtilsValueError on a bad config.
        """
        config = config or {}
        result_names: Dict[str, str] = {}
        specs = []
        for plugin in plugins:
            if isinstance(plugin, str):
                path = plugin
            else:
                from . import jit

                result_names.update(jit.plugin_result_names(plugin))
                path = jit.compile_class(plugin)
            cfg = config.get(_plugin_key(plugin))
            specs.append((path, json.dumps(cfg) if cfg is not None else None))
        self._native = _NativePlugins(specs, result_names or None, runtime)

    def run(
        self,
        traces: Union[str, List[str]],
        index_dir: Optional[str] = None,
        auto_index: bool = True,
    ) -> PluginRun:
        """Fold every plugin in the set over one fused scan of ``traces`` (a
        file, a directory, or a list of paths).

        With ``auto_index`` the traces are normalized and indexed first.
        Returns a :class:`PluginRun` holding the named results and the scan
        counters.

        A result's type depends on what the plugin emitted:

        - ``emit_frame`` -> our :class:`~dftracer.utils.DataFrame`, zero-copy.
          Call ``.to_arrow()`` / ``.to_pandas()`` for other shapes.
        - ``emit_arrow`` -> a ``pyarrow.Table`` as emitted, nested types
          included.
        - ``emit_lazyframe`` -> a :class:`~dftracer.utils.LazyFrame`.
        - ``emit_result`` bytes -> ``bytes``.
        """
        raw_results, stats = self._native.run(traces, index_dir, auto_index)
        results = {name: self._shape(obj) for name, obj in raw_results.items()}
        return PluginRun(results, stats)

    def _shape(self, obj: object) -> "_RunResult":
        """Return one native result in the shape its emit kind implies.

        The native layer already hands back one Python type per emit kind and
        has already applied the plugin's wire-id -> attribute-name result
        renaming, so this only distinguishes a native frame from bytes/Arrow.
        """
        if isinstance(obj, bytes):
            return obj
        if isinstance(obj, _DataFrame):
            return DataFrame(obj)
        return obj
