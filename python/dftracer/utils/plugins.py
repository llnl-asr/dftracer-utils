# SPDX-License-Identifier: MIT
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
import os
from pathlib import Path
from typing import TYPE_CHECKING, Dict, List, NamedTuple, Union, cast

from ._dataframe import DataFrame as _DataFrame
from ._jit_errors import check_port_wiring as _check_port_wiring
from ._jit_errors import translate_plan_error as _translate_plan_error
from ._jit_errors import translate_run_error as _translate_run_error
from ._public import set_home as _set_home
from .dftracer_utils_ext import DataFrame as _NativeDataFrame
from .dftracer_utils_ext import Plugins as _NativePlugins
from .dftracer_utils_ext import _load_plugin, _unload_plugin

if TYPE_CHECKING:
    from types import TracebackType

    import pyarrow as pa

    from ._lazyframe import LazyFrame
    from .dftracer_utils_ext import PluginRecord
    from .runtime import Runtime

__all__ = [
    "JSONValue",
    "PluginHandle",
    "PluginInfo",
    "PluginRun",
    "Plugins",
    "load_plugin",
    "unnest",
]

# A JSON-serializable value tree (config is handed straight to json.dumps).
JSONValue = Union[str, int, float, bool, None, List["JSONValue"], Dict[str, "JSONValue"]]

# One run() result value per emit kind: bytes, a native frame or a lazy plan.
_RunResult = Union[bytes, "_DataFrame", "LazyFrame"]


def unnest(
    result: pa.Table | pa.RecordBatchReader | pa.RecordBatch,
    column: str,
    keep_empty: bool = False,
) -> pa.Table:
    """Explode a list-typed ``column`` of a ``run()`` map result into one row per
    element, repeating the other columns; the inverse of the set/list/top-k
    aggregates.

    ``result`` is a ``pyarrow.Table``, a ``pyarrow.RecordBatchReader`` or a
    ``pyarrow.RecordBatch``. ``column`` names a ``list<utf8>``, ``list<int64>``, or ``list<struct<...>>`` column; a
    ``list<struct>`` flattens its fields into columns (named by the struct
    fields). Returns a ``pyarrow.Table``.

    An empty or null list drops the row (SQL inner unnest). With
    ``keep_empty=True`` it emits one row whose exploded column(s) are null (SQL
    outer/left unnest). Row order is input order, then element order within a row.
    """
    return _DataFrame.from_arrow(result).unnest(column, keep_empty).to_arrow()


def _plugin_key(plugin: str | type) -> str:
    """The name a caller's ``config`` mapping keys this plugin by: a compiled
    ``.so``'s stem, or a ``@jit.plugin`` class's name."""
    if isinstance(plugin, str):
        return Path(plugin).stem
    return getattr(plugin, "__name__", str(plugin))


class PluginInfo(NamedTuple):
    """What one loaded plugin is and what it registered, from
    :meth:`Plugins.describe`: the path it was loaded from, its ABI version,
    whether it declares a ``plan_duql`` and whether it carries a fold, and one
    entry per name it registered, per kind."""

    path: str
    abi_version: int
    has_plan_duql: bool
    has_fold: bool
    #: The event fields it reads.
    provides: List[str]
    #: The event fields it needs from an earlier plugin.
    consumes: List[str]
    #: Expression ops, as ``name(arg, ...) -> ret [kind]``.
    ops: List[str]
    #: Column ops (``UDF(name, arity)``).
    udfs: List[str]
    #: Accumulators (``UDAF(name, kind)``).
    udafs: List[str]
    #: State types registered for the fold.
    states: List[str]
    #: Scan providers.
    providers: List[str]
    #: Plan nodes.
    nodes: List[str]
    #: Index extensions.
    index_extensions: List[str]
    #: File formats.
    file_formats: List[str]
    #: Sinks.
    sinks: List[str]
    #: The batch columns it declared it reads; empty means every column.
    reads: List[str]
    #: Declared config keys, as ``name (kind[, required]) - doc``.
    config_keys: List[str]


class PluginRun:
    """The named results and scan counters of one :meth:`Plugins.run` call.

    ``results`` is ``{name: value}`` of every plugin's emitted results;
    ``stats`` is the scan counters ``{"events_scanned", "events_matched"}``.
    """

    __slots__ = ("results", "stats")

    def __init__(self, results: dict[str, _RunResult], stats: dict[str, int]) -> None:
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
        plugins: list[str | type],
        config: dict[str, dict[str, JSONValue]] | None = None,
        runtime: Runtime | None = None,
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
        result_names: dict[str, str] = {}
        specs = []
        for plugin in plugins:
            cfg = config.get(_plugin_key(plugin))
            if isinstance(plugin, str):
                path = plugin
            else:
                from .jit._maps import plugin_config
                from .jit._package import compile_class, plugin_result_names

                cfg = plugin_config(plugin, cfg)
                result_names.update(plugin_result_names(plugin))
                path = compile_class(plugin)
            specs.append((path, json.dumps(cfg) if cfg is not None else None))
        _check_port_wiring(plugins)
        try:
            self._native = _NativePlugins(specs, result_names or None, runtime)
        except (ImportError, ValueError) as exc:
            translated = _translate_plan_error(plugins, exc)
            if translated is None:
                raise
            raise translated from exc

    def describe(self) -> list[PluginInfo]:
        """What each plugin in the set is, in the order it was given: its path,
        its ABI version, whether it declares a ``plan_duql`` and whether it
        carries a fold, and the names it registered per kind (ops, udfs,
        udafs, states, providers, nodes, index extensions, file formats,
        sinks), plus its declared reads and config keys.

        Static: nothing is scanned and no plugin callback runs.
        """
        raw: list[PluginRecord] = self._native.describe()
        return [
            PluginInfo(
                path=info["path"],
                abi_version=info["abi_version"],
                has_plan_duql=info["has_plan_duql"],
                has_fold=info["has_fold"],
                provides=info["provides"],
                consumes=info["consumes"],
                ops=info["ops"],
                udfs=info["udfs"],
                udafs=info["udafs"],
                states=info["states"],
                providers=info["providers"],
                nodes=info["nodes"],
                index_extensions=info["index_extensions"],
                file_formats=info["file_formats"],
                sinks=info["sinks"],
                reads=info["reads"],
                config_keys=info["config_keys"],
            )
            for info in raw
        ]

    def run(
        self,
        traces: str | list[str],
        index_dir: str | None = None,
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
        - ``emit_lazyframe`` -> a :class:`~dftracer.utils.LazyFrame`.
        - ``emit_result`` bytes -> ``bytes``.

        A compiled plugin that fails the run with ``raise jit.Fatal(...)`` or
        ``raise jit.Cancelled()``, or whose call fails (``jit.Error`` and its
        subclasses), surfaces here as that ``jit`` exception with the message
        and the location of the raise or the call.
        """
        try:
            raw_results, stats = self._native.run(traces, index_dir, auto_index)
        except Exception as exc:
            failure = _translate_run_error(exc)
            if failure is None:
                raise
            raise failure from exc
        results = {name: self._shape(obj) for name, obj in raw_results.items()}
        return PluginRun(results, stats)

    def _shape(self, obj: object) -> _RunResult:
        """Return one native result in the shape its emit kind implies.

        The native layer already hands back one Python type per emit kind and
        has already applied the plugin's wire-id -> attribute-name result
        renaming, so this only distinguishes a native frame from bytes.
        """
        if isinstance(obj, bytes):
            return obj
        if isinstance(obj, _NativeDataFrame):
            return _DataFrame(obj)
        return cast("_RunResult", obj)


class PluginHandle:
    """A plugin loaded into the process by :func:`load_plugin`.

    The plugin's file formats and the rest it registers stay available to every
    reader in the process (:func:`scan_file`, :class:`TraceViewer`,
    :class:`Indexer`) until :meth:`close` or the end of the ``with`` block.
    """

    __slots__ = ("_path", "_closed")

    def __init__(self, path: str) -> None:
        self._path = path
        self._closed = False

    @property
    def path(self) -> str:
        """The path the plugin was loaded from."""
        return self._path

    @property
    def closed(self) -> bool:
        return self._closed

    def close(self) -> None:
        """Unload the plugin when no other handle of the same path holds it.

        Raises ``DFTUtilsValueError``, and keeps the plugin loaded, while a
        :class:`LazyFrame` made by :func:`scan_file` through one of its file
        formats is alive: drop it first. A second call does nothing. A plugin
        named by ``DFTRACER_UTILS_PLUGINS`` stays loaded for the process.
        """
        if self._closed:
            return
        _unload_plugin(self._path)
        self._closed = True

    def __enter__(self) -> PluginHandle:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None:
        self.close()


def load_plugin(path: str | os.PathLike[str]) -> PluginHandle:
    """Load the compiled plugin at ``path`` into the process and return its
    handle, which works as a context manager::

        with dft.plugins.load_plugin("csv_reader.so"):
            frame = dft.scan_file("data.csv").collect()

    Loading a path that is already loaded, by the handle of an earlier call or
    by ``DFTRACER_UTILS_PLUGINS``, loads nothing again. Raises a
    ``DFTUtilsError`` that names the path when it does not load.
    """
    text = os.fspath(path)
    _load_plugin(text)
    return PluginHandle(text)


_set_home(__name__, globals(), __all__)
