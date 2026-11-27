# SPDX-License-Identifier: MIT
"""Writing plans and frames as DFTracer files: ``sink_trace`` and
``sink_table`` on ``LazyFrame`` and ``TraceViewer``, ``write_trace`` and
``write_table`` on ``DataFrame``, and the generic plugin ``sink`` and ``write``.

A path ending in ``.pfw.gz`` or ``.pfw`` is one file; any other path is a
folder of ``part-NNNNN.pfw.gz`` files (plain output is one ``.pfw`` file). A
trace verb writes DFTracer events and needs the event envelope; a table verb
writes typed records: a ``dftu.table`` header line and one ph 3 line per row. The native writer stages
the output next to the target, renames it into place and builds one index for
the files.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, fields
from typing import TYPE_CHECKING, Any, Mapping, Optional, Sequence, Union

from . import dftracer_utils_ext as _ext
from ._enums import DType, coerce_bytes

if TYPE_CHECKING:
    from ._lazyframe import LazyFrame
    from .runtime import Runtime
    from .schemas import JsonValue

    PathLike = Union[str, "os.PathLike[str]"]
    SinkOption = Union[str, int, float, bool]


_MODES = ("error", "overwrite")
_KINDS = (None, "events", "records")

# The dtype a streamed column gets in a records header. The plan does not say
# whether a column holds a null, so the nullable dtype keeps a null apart from
# a NaN and never refuses one.
_LISTS = (DType.LIST, DType.LARGE_LIST)

_RECORD_DTYPES = {
    DType.BOOL: "boolean",
    DType.STRING: "object",
    DType.LARGE_STRING: "object",
    **{
        DType[text.upper()]: text
        for text in (
            "Int8",
            "Int16",
            "Int32",
            "Int64",
            "UInt8",
            "UInt16",
            "UInt32",
            "UInt64",
            "Float32",
            "Float64",
        )
    },
}
_SIGNED = (DType.INT8, DType.INT16, DType.INT32, DType.INT64)
_UNSIGNED = (DType.UINT8, DType.UINT16, DType.UINT32, DType.UINT64)
_WIDTH = {
    DType.INT8: 8,
    DType.UINT8: 8,
    DType.INT16: 16,
    DType.UINT16: 16,
    DType.INT32: 32,
    DType.UINT32: 32,
    DType.INT64: 64,
    DType.UINT64: 64,
}
_CATEGORY_TYPES = (
    DType.STRING,
    DType.LARGE_STRING,
    *_SIGNED,
    *_UNSIGNED,
    DType.FLOAT32,
    DType.FLOAT64,
)
_EVENT_TEXT = ("name", "cat")
_EVENT_INT = ("pid", "tid", "ts", "ph")


class _Default:
    """The default of a sink argument that is told apart from the same value
    given by the caller: a plugin sink gets every argument that was given."""

    __slots__ = ("value",)

    def __init__(self, value: Any) -> None:  # noqa: ANN401
        self.value = value

    def __repr__(self) -> str:
        return repr(self.value)


DEFAULT_MODE: Any = _Default("error")
DEFAULT_PART_SIZE: Any = _Default(0)
DEFAULT_MAX_OPEN_FILES: Any = _Default(64)
DEFAULT_MAX_PARTITIONS: Any = _Default(1024)


def _given(value: Any) -> tuple[Any, bool]:  # noqa: ANN401
    return (value.value, False) if isinstance(value, _Default) else (value, True)


@dataclass(frozen=True)
class SinkSummary:
    """What a sink wrote: ``rows`` rows under ``root`` (the file or the folder),
    as ``kind`` (``"events"``, ``"records"`` or the plugin format's name), in
    ``part_count`` files."""

    root: str
    rows: int
    kind: str
    part_count: int


@dataclass(frozen=True)
class TraceWriteOptions:
    """The options of the DFTracer format.

    ``compress`` ``None`` follows the path: ``.pfw.gz`` and any folder write
    gzip (a folder cannot be plain), ``.pfw`` writes plain text. A value that contradicts the extension
    raises. ``level`` is the gzip level. ``member_size`` is the uncompressed
    bytes of one gzip member (an int or a unit string such as ``"4MiB"``); ``0``
    is the writer's automatic size, 4 MiB. ``build_index`` ``None`` builds the
    index of gzip output; ``True`` for output that cannot be indexed raises.
    """

    compress: Optional[bool] = None  # noqa: UP045
    level: int = 6
    member_size: Union[int, str] = 0  # noqa: UP007
    build_index: Optional[bool] = None  # noqa: UP045


def _options(options: object) -> TraceWriteOptions:
    if options is None:
        return TraceWriteOptions()
    if isinstance(options, TraceWriteOptions):
        return options
    if isinstance(options, Mapping):
        known = {f.name for f in fields(TraceWriteOptions)}
        unknown = sorted(str(k) for k in options if k not in known)
        if unknown:
            raise ValueError(
                f"unknown sink option(s) {unknown}; the DFTracer format takes {sorted(known)}"
            )
        return TraceWriteOptions(**options)
    raise TypeError(
        f"options must be a TraceWriteOptions, a mapping or None, not {type(options).__name__}"
    )


def output_stem(path: str) -> str:
    """The table name of an output: its file or folder name without ``.pfw.gz`` or ``.pfw``."""
    leaf = os.path.basename(path.rstrip("/")) or path
    for suffix in (".pfw.gz", ".pfw"):
        if leaf.endswith(suffix) and len(leaf) > len(suffix):
            return leaf[: -len(suffix)]
    return leaf


def _partition_keys(partition_by: object) -> list[str]:
    if partition_by is None:
        return []
    if isinstance(partition_by, str) or not isinstance(partition_by, Sequence):
        raise TypeError("partition_by must be a sequence of column names")
    keys = list(partition_by)
    if not keys:
        raise ValueError("partition_by must name at least one column")
    for key in keys:
        if not isinstance(key, str):
            raise TypeError(f"partition_by names must be str, not {type(key).__name__}")
    if len(set(keys)) != len(keys):
        raise ValueError("partition_by names a column twice")
    return keys


def _is_file(path: str) -> bool:
    leaf = path.rstrip("/")
    return leaf.endswith((".pfw.gz", ".pfw"))


_EVENT_HASHES = ("hhash", "fhash", "shash")


def _events_problem(schema: Mapping[str, DType]) -> str | None:
    """Why ``schema`` is not the event envelope, or ``None`` when it is."""
    missing = [name for name in _EVENT_TEXT + _EVENT_INT if name not in schema]
    if missing:
        return f"the plan has no column {', '.join(repr(m) for m in missing)}"
    for name in _EVENT_TEXT:
        if not schema[name].is_text:
            return f"column {name!r} is {schema[name].name.lower()}, not text"
    for name in _EVENT_INT + (("dur",) if "dur" in schema else ()):
        if not schema[name].is_integer:
            return f"column {name!r} is {schema[name].name.lower()}, not an integer"
    for name, dtype in schema.items():
        if name in _EVENT_TEXT or name in _EVENT_INT or name == "dur":
            continue
        if name in _EVENT_HASHES:
            if not dtype.is_text:
                return f"column {name!r} is {dtype.name.lower()}, not text"
            continue
        if not name.startswith("args.") or len(name) == len("args."):
            return f"column {name!r} is not part of an event (name, cat, pid, tid, ts, ph, dur, hhash, fhash, shash, args.<key>)"
        if dtype not in _RECORD_DTYPES:
            return f"column {name!r} has a type an event cannot carry"
    return None


def _schema_header(
    schema: Mapping[str, DType],
    name: str,
    meta: Mapping[str, Any],
    carried: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """The records header of a plan, from its column types, except a column
    whose field ``carried`` holds and still fits."""
    kept = {str(f["name"]): f for f in (carried or {}).get("fields", ())}
    out: list[dict[str, object]] = []
    for column, dtype in schema.items():
        field = kept.get(column)
        if field is not None and _carried_fits(field, dtype):
            out.append(dict(field))
            continue
        if dtype in _LISTS:
            out.append({"name": column, "dtype": "object", "elem": "list"})
            continue
        text = _RECORD_DTYPES.get(dtype)
        if text is None:
            raise TypeError(
                f"column {column!r} has the type {dtype.name.lower()}, which a table cannot store"
            )
        out.append({"name": column, "dtype": text})
    return {
        "format": "dftu.table",
        "format_version": _ext.TABLE_FORMAT_VERSION,
        "table": name,
        "index": [],
        "index_names": [],
        "fields": out,
        "meta": dict(meta),
    }


def _carried_fits(field: Mapping[str, Any], dtype: DType) -> bool:
    """Whether a column of engine type ``dtype`` can carry the table field's
    dtype, as the native writer will check it: the same kind and no wider."""
    full = str(field.get("dtype", "")).lower()
    text = full.split("[", 1)[0]
    if text == "category":
        return dtype in _CATEGORY_TYPES
    if text == "object" and field.get("elem") == "list" and dtype in _LISTS:
        return True
    if text in ("object", "str", "string", "large_string"):
        return dtype.is_text
    if text in ("bool", "boolean"):
        return dtype == DType.BOOL
    if text == "float32" or full.startswith("float["):
        return dtype == DType.FLOAT32
    if text in ("float", "float64", "double"):
        return dtype in (DType.FLOAT32, DType.FLOAT64)
    for stem, kinds in (("uint", _UNSIGNED), ("int", _SIGNED)):
        digits = text[len(stem) :]
        if text.startswith(stem) and digits.isdigit():
            return dtype in kinds and _WIDTH[dtype] <= int(digits)
    return False


def _carried_header(
    header: Mapping[str, Any] | None,
    schema: Mapping[str, DType],
    table: str,
    name: str | None,
    meta: Mapping[str, Any] | None,
) -> Mapping[str, Any]:
    """The records header of a plan: made from its column types, keeping from
    the carried table header the details (dtype, categories, ordered, elem,
    arrow_type) of each column whose engine type still fits them, in the plan's
    column order. The carried index names stay only when every index column
    survives."""
    made = _schema_header(schema, table, meta or {}, header)
    if header is None:
        return made
    for key, value in header.items():
        if key not in ("nrows", "fields", "index", "index_names", "table", "meta"):
            made[key] = value
    if name is None and header.get("table"):
        made["table"] = header["table"]
    if meta is None and "meta" in header:
        made["meta"] = header["meta"]
    index = list(header.get("index") or [])
    if index and all(column in schema for column in index):
        made["index"] = index
        made["index_names"] = list(header.get("index_names") or [])
    return made


def _raw_columns(header: Mapping[str, Any]) -> list[str]:
    """The fields whose cells hold JSON text: set and list columns, and
    categories of mixed labels."""
    out = []
    for field in header["fields"]:
        if field.get("elem"):
            out.append(str(field["name"]))
        elif field.get("dtype") == "category":
            labels = field.get("categories") or []
            # Float labels may hold the text of an infinite float, and the
            # cells stay floats.
            if str(field.get("categories_dtype", "")).lower().startswith("float"):
                continue
            if not (
                all(isinstance(c, str) for c in labels)
                or all(isinstance(c, int) and not isinstance(c, bool) for c in labels)
                or all(isinstance(c, (int, float)) and not isinstance(c, bool) for c in labels)
            ):
                out.append(str(field["name"]))
    return out


def _args_text(header: Mapping[str, Any]) -> str:
    """The args object the native writer takes: the header without its row
    count, which the writer places itself."""
    args = {k: v for k, v in header.items() if k != "nrows"}
    return json.dumps(args, allow_nan=False, separators=(",", ":"))


def _meta_text(meta: object) -> str:
    if not isinstance(meta, Mapping):
        raise TypeError(f"meta must be a mapping, not {type(meta).__name__}")
    try:
        return json.dumps(dict(meta), allow_nan=False)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"meta must be JSON-serializable: {exc}") from None


def _option_text(value: SinkOption) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def _plugin_sink(
    plan: LazyFrame,
    path: str,
    format: str,
    options: object,
    shared: Mapping[str, SinkOption],
    runtime: Runtime | None,
) -> SinkSummary:
    if options is None:
        given: Mapping[str, object] = {}
    elif isinstance(options, Mapping):
        given = options
    else:
        raise TypeError(
            f"the options of the {format!r} sink must be a mapping of str to str, int, float or "
            f"bool, not {type(options).__name__}"
        )
    text: dict[str, str] = {"path": path}
    for key, value in given.items():
        if not isinstance(key, str):
            raise TypeError(f"sink option names must be str, not {type(key).__name__}")
        if key == "path":
            raise ValueError("the path is the first argument of sink, not an option")
        if not isinstance(value, (str, int, float)):
            raise TypeError(
                f"sink option {key!r} must be a str, int, float or bool, not {type(value).__name__}"
            )
        text[key] = _option_text(value)
    for key, value in shared.items():
        if key in text:
            raise ValueError(f"{key!r} is a sink argument and also a sink option; pass it once")
        text[key] = _option_text(value)
    rows, _batches = _ext.LazyFrame._sink_plugin(plan, format, text, 0, runtime)
    return SinkSummary(root=path, rows=int(rows), kind=format, part_count=1)


def run_sink(
    plan: LazyFrame,
    path: PathLike,
    *,
    kind: str | None = None,
    format: str | None = None,
    options: object = None,
    name: str | None = None,
    meta: Mapping[str, JsonValue] | None = None,
    mode: str = DEFAULT_MODE,
    part_size: int = DEFAULT_PART_SIZE,
    partition_by: object = None,
    max_open_files: int = DEFAULT_MAX_OPEN_FILES,
    max_partitions: int = DEFAULT_MAX_PARTITIONS,
    header: Mapping[str, Any] | None = None,
    known_rows: int = -1,
    raw_columns: tuple[str, ...] = (),
    schema: Mapping[str, DType] | None = None,
    runtime: Runtime | None = None,
) -> SinkSummary:
    """Run ``plan`` into ``path``. ``kind`` is ``"events"`` or ``"records"`` for
    the DFTracer format and ``None`` for a plugin ``format``. ``header`` is a
    records header to use when it describes the plan's columns, ``known_rows``
    the row count of an eager frame. ``schema`` is the plan's columns when the
    plan itself cannot name them before it runs (a trace viewer's ``args.*``)."""
    mode, mode_given = _given(mode)
    part_size, part_size_given = _given(part_size)
    max_open_files, max_open_files_given = _given(max_open_files)
    max_partitions, max_partitions_given = _given(max_partitions)
    target = os.fspath(path)
    if not isinstance(target, str):
        raise TypeError("the sink path must be a str or a path-like object of str")
    if not target:
        raise ValueError("the sink path is empty")
    if mode not in _MODES:
        raise ValueError(f"mode must be one of {_MODES}, not {mode!r}")
    if kind not in _KINDS:
        raise ValueError(f"kind must be 'events' or 'records', not {kind!r}")
    if kind is not None and format is not None:
        raise ValueError("give a DFTracer kind or a plugin format, not both")
    keys = _partition_keys(partition_by)
    if (
        not isinstance(max_open_files, int)
        or isinstance(max_open_files, bool)
        or max_open_files < 1
    ):
        raise ValueError(f"max_open_files must be a positive int, not {max_open_files!r}")
    if (
        not isinstance(max_partitions, int)
        or isinstance(max_partitions, bool)
        or max_partitions < 1
    ):
        raise ValueError(f"max_partitions must be a positive int, not {max_partitions!r}")
    if max_partitions_given and format is not None:
        raise ValueError("a plugin sink does not take max_partitions")
    if not isinstance(part_size, int) or isinstance(part_size, bool) or part_size < 0:
        raise ValueError(f"part_size must be a non-negative int, not {part_size!r}")
    if format is not None:
        if keys:
            raise ValueError("a plugin sink does not take partition_by")
        if not isinstance(format, str):
            raise TypeError(f"format must be a str or None, not {type(format).__name__}")
        shared: dict[str, SinkOption] = {}
        if name is not None:
            shared["name"] = name
        if meta is not None:
            shared["meta"] = _meta_text(meta)
        if mode_given:
            shared["mode"] = mode
        if part_size_given:
            shared["part_size"] = part_size
        if max_open_files_given:
            shared["max_open_files"] = max_open_files
        return _plugin_sink(plan, target, format, options, shared, runtime)
    if kind is None:
        raise ValueError("give a DFTracer kind or a plugin format")

    opts = _options(options)
    is_file = _is_file(target)
    if is_file and part_size:
        raise ValueError(f"part_size needs a folder, but {target!r} names one file")
    if is_file and keys:
        raise ValueError(f"partition_by needs a folder, but {target!r} names one file")
    if not isinstance(opts.level, int) or isinstance(opts.level, bool) or not 1 <= opts.level <= 12:
        raise ValueError(f"level must be an int from 1 to 12, not {opts.level!r}")
    if opts.compress is not None and not isinstance(opts.compress, bool):
        raise TypeError("compress must be True, False or None")
    if opts.build_index is not None and not isinstance(opts.build_index, bool):
        raise TypeError("build_index must be True, False or None")
    member_size = coerce_bytes(opts.member_size, "member_size")
    if member_size < 0:
        raise ValueError("member_size must not be negative")
    if is_file:
        natural = target.rstrip("/").endswith(".gz")
        if opts.compress is not None and opts.compress != natural:
            raise ValueError(
                f"compress={opts.compress} contradicts the extension of {target!r}; "
                f"use {'.pfw.gz' if opts.compress else '.pfw'} or leave compress unset"
            )
        compress = natural
    else:
        if opts.compress is False:
            raise ValueError(
                f"{target!r} is a folder of parts, which is gzip; "
                "write a single .pfw file for plain output"
            )
        compress = True
    build_index = compress if opts.build_index is None else opts.build_index
    if opts.build_index and not compress:
        raise ValueError("a plain .pfw output cannot be indexed; use .pfw.gz or build_index=False")

    if kind == "records":
        if meta is not None:
            _meta_text(meta)
        if name is not None and (not isinstance(name, str) or not name):
            raise ValueError("name must be a non-empty str")
    if mode == "error" and os.path.lexists(target):
        raise FileExistsError(f"{target} already exists; pass mode='overwrite' to replace it")

    schema = dict(schema) if schema is not None else plan.schema
    unknown = not schema
    missing_keys = [k for k in keys if schema and k not in schema]
    if missing_keys:
        raise ValueError(f"partition_by names {missing_keys}, which are not columns of the plan")
    problem = (
        "the plan's columns are not known before it runs" if unknown else _events_problem(schema)
    )
    if kind == "events" and problem is not None:
        raise ValueError(f"a trace needs the event envelope: {problem}")

    header_args: str | None = None
    if kind == "records":
        table = name if name is not None else output_stem(target)
        used: Mapping[str, Any]
        if unknown:
            used = {k: v for k, v in _schema_header({}, table, meta or {}).items() if k != "fields"}
        else:
            used = _carried_header(header, schema, table, name, meta)
            raw_columns = tuple(dict.fromkeys((*raw_columns, *_raw_columns(used))))
        header_args = _args_text(used)
    summary = _ext._sink_output(
        plan,
        target,
        kind,
        mode,
        compress,
        opts.level,
        member_size,
        part_size,
        keys,
        max_open_files,
        max_partitions,
        build_index,
        header_args,
        list(raw_columns),
        known_rows if kind == "records" else -1,
        runtime,
    )
    root, count, files = summary
    return SinkSummary(root=root, rows=int(count), kind=kind, part_count=len(files))


__all__ = ["SinkSummary", "TraceWriteOptions", "run_sink"]
