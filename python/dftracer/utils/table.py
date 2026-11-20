# SPDX-License-Identifier: MIT
"""Typed table files: a pandas frame stored as DFTracer-style gzip JSON lines.

Line 1 is a ph 4 record whose args name the format (``dftu.table``, version 1),
the table, the row count, the index levels and every field with its exact
pandas dtype. Every other line is a ph 3 record holding one row in its args.
The file reads with ``zcat`` and ``jq``, and DFAnalyzer's checkpoint tables use
the same format. Encoding and decoding run in C++; this module maps pandas
frames to and from the native frame.
"""

from __future__ import annotations

import glob
import json
import math
import os
import shutil
import tempfile
import types
from typing import TYPE_CHECKING, Any, Protocol, Union, runtime_checkable

from . import dftracer_utils_ext as _ext
from .dataframe import DataFrame

if TYPE_CHECKING:
    import numpy as np
    import pandas as pd
    import pyarrow as pa

    PathLike = Union[str, "os.PathLike[str]"]
    JsonValue = Union[None, bool, int, float, str, list["JsonValue"], dict[str, "JsonValue"]]
    Meta = dict[str, JsonValue]
    Header = dict[str, Any]
    Column = Union["np.ndarray[Any, Any]", "pd.api.extensions.ExtensionArray", "pd.Series"]
    Dtype = Union["np.dtype[Any]", "pd.api.extensions.ExtensionDtype"]

FORMAT = "dftu.table"
FORMAT_VERSION = 1
_PART_GLOB = "*.pfw.gz"
_FLOAT_TEXT = {"NaN": math.nan, "Infinity": math.inf, "-Infinity": -math.inf}
_INT64_MAX = 2**63 - 1


@runtime_checkable
class _Computable(Protocol):
    def compute(self) -> pd.DataFrame: ...


@runtime_checkable
class _Partitioned(Protocol):
    """A Dask ``DataFrame``: partitions that compute to pandas frames."""

    npartitions: int

    def get_partition(self, n: int) -> _Computable: ...


def _libs() -> tuple[types.ModuleType, types.ModuleType, types.ModuleType]:
    try:
        import numpy as np
        import pandas as pd
        import pyarrow as pa
    except ImportError as exc:  # pragma: no cover - the extras are optional
        raise ImportError("dftracer.utils tables need pandas, numpy and pyarrow") from exc
    return np, pd, pa


# -- encoding ---------------------------------------------------------------


def _scalar(value: object) -> object:
    """The JSON value of one element of a set or list cell. A NaN or infinity
    element would read back as the string it is written as, so it is
    refused."""
    np, pd, _ = _libs()
    if value is None or value is pd.NA or value is pd.NaT:
        return None
    if isinstance(value, np.generic):
        value = value.item()
    if isinstance(value, float):
        if not math.isfinite(value):
            raise TypeError("NaN and infinity cannot be stored in a set or list cell")
    if isinstance(value, (str, bool, int, float)):
        return value
    raise TypeError(f"{type(value).__name__} cannot be stored in a table cell")


def _object_cells(name: str, values: pd.Series) -> tuple[str | None, list[str | None]]:
    """The cells of an object column as text, and its container kind or None.

    A str cell is the string itself, a set or list cell is JSON text of its
    elements. A column of sets and lists mixed, of nested containers, or of
    any other object is refused.
    """
    kinds = set()
    for v in values:
        if isinstance(v, (set, frozenset)):
            kinds.add("set")
        elif isinstance(v, (list, tuple)):
            kinds.add("list")
    if len(kinds) > 1:
        raise ValueError(f"column {name!r} mixes sets and lists")
    elem = kinds.pop() if kinds else None
    _, pd, _ = _libs()
    cells: list[str | None] = []
    for v in values:
        if v is None or v is pd.NA or (isinstance(v, float) and math.isnan(v)):
            cells.append(None)
        elif elem is None:
            if not isinstance(v, str):
                raise TypeError(
                    f"column {name!r} holds a {type(v).__name__}; an object column may hold only str, set or list cells"
                )
            cells.append(v)
        else:
            if not isinstance(v, (set, frozenset, list, tuple)):
                raise TypeError(f"column {name!r} mixes containers with {type(v).__name__} cells")
            try:
                cells.append(
                    json.dumps([_scalar(e) for e in v], allow_nan=False, separators=(",", ":"))
                )
            except TypeError as exc:
                raise TypeError(f"column {name!r}: {exc}") from None
    return elem, cells


def _category_labels(name: str, categories: pd.Index) -> tuple[list[object], str]:
    """The JSON labels of a category column and their kind: ``"str"``, ``"int"``
    or ``"float"``. Labels the format cannot carry are refused."""
    np, pd, _ = _libs()
    dtype = categories.dtype
    if len(categories) == 0:
        return [], "str"
    if pd.api.types.is_bool_dtype(dtype):
        raise TypeError(f"column {name!r}: boolean categories cannot be stored in a table")
    if pd.api.types.is_integer_dtype(dtype):
        labels = [int(c) for c in categories]
        if max(labels) > _INT64_MAX:
            raise ValueError(f"column {name!r}: category labels above 2**63 - 1 cannot be stored")
        return list(labels), "int"
    if pd.api.types.is_float_dtype(dtype):
        floats = [float(c) for c in categories]
        if not all(math.isfinite(f) for f in floats):
            raise ValueError(f"column {name!r}: infinite category labels cannot be stored")
        return list(floats), "float"
    if all(isinstance(c, str) for c in categories):
        return list(categories), "str"
    raise TypeError(f"column {name!r}: categories must be all str or all numbers")


def _arrow_numeric(series: pd.Series) -> pa.Array:
    np, pd, pa = _libs()
    dtype = series.dtype
    if isinstance(dtype, np.dtype):
        return pa.array(series.to_numpy(), from_pandas=False)
    if isinstance(dtype, pd.ArrowDtype):
        arr = pa.array(series.array)
        return arr.combine_chunks() if isinstance(arr, pa.ChunkedArray) else arr
    numpy_dtype = getattr(dtype, "numpy_dtype", None)
    if numpy_dtype is not None:
        fill = False if numpy_dtype == np.dtype(bool) else 0
        data = series.array.to_numpy(dtype=numpy_dtype, na_value=fill)
        return pa.array(data, mask=series.isna().to_numpy(), from_pandas=False)
    raise TypeError(f"dtype {dtype} cannot be stored in a table")


def _check_dtype(name: str, dtype: object) -> None:
    np, pd, pa = _libs()
    if isinstance(dtype, np.dtype):
        if dtype.kind in "iub" or (dtype.kind == "f" and dtype.itemsize in (4, 8)):
            return
    elif isinstance(dtype, pd.ArrowDtype):
        t = dtype.pyarrow_dtype
        if (
            pa.types.is_integer(t)
            or pa.types.is_boolean(t)
            or pa.types.is_string(t)
            or pa.types.is_large_string(t)
        ) or (pa.types.is_floating(t) and not pa.types.is_float16(t)):
            return
    elif isinstance(dtype, (pd.CategoricalDtype, pd.StringDtype)):
        return
    elif getattr(dtype, "numpy_dtype", None) is not None and getattr(dtype, "kind", "") in "iufb":
        return
    raise TypeError(f"column {name!r} has dtype {dtype}, which a table cannot store")


def _arrow_alias(name: str, dtype: pd.ArrowDtype) -> str:
    """The text that rebuilds an ArrowDtype's pyarrow type with
    ``pa.type_for_alias``, which the reader needs because ``string[pyarrow]``
    names both ArrowDtype(pa.string()) and StringDtype("pyarrow")."""
    _, _, pa = _libs()
    text = str(dtype.pyarrow_dtype)
    try:
        rebuilt = pa.type_for_alias(text)
    except ValueError:
        rebuilt = None
    if rebuilt != dtype.pyarrow_dtype:
        raise TypeError(f"column {name!r}: the Arrow type {text} cannot be stored in a table")
    return text


def _dtype_text(dtype: object) -> str:
    """The dtype string of a header field. A StringDtype that has NA as its
    missing value names its storage, as ``string[pyarrow]`` or
    ``string[python]``."""
    _, pd, _ = _libs()
    if isinstance(dtype, pd.StringDtype) and dtype.na_value is pd.NA:
        return f"string[{dtype.storage}]"
    return str(dtype)


def _prepare(
    df: pd.DataFrame | DataFrame, name: str, meta: Meta | None, keep_index: bool = False
) -> tuple[Header, pa.Table, list[str]]:
    np, pd, pa = _libs()
    if not isinstance(df, pd.DataFrame):
        if isinstance(df, DataFrame):
            df = df.to_pandas()
        else:
            raise TypeError(
                f"expected a pandas or dftracer.utils DataFrame, got {type(df).__name__}"
            )
    index = df.index
    default = (
        isinstance(index, pd.RangeIndex)
        and index.name is None
        and index.start == 0
        and index.step == 1
        and index.stop == len(df)
    )
    if default and not keep_index:
        index_fields: list[str] = []
        flat = df
    else:
        for level in index.names:
            if level is not None and not isinstance(level, (str, int)):
                raise TypeError(f"index level name {level!r} cannot be stored in a table")
        index_fields = [
            f"__index_level_{i}__" if n is None else str(n) for i, n in enumerate(index.names)
        ]
        clash = set(index_fields) & {str(c) for c in df.columns}
        if clash or len(set(index_fields)) != len(index_fields):
            raise ValueError(
                f"table {name!r}: index level names {index_fields} collide with column names "
                f"{sorted(clash)} or with each other"
            )
        flat = df.rename_axis(index_fields).reset_index()
    columns = [str(c) for c in flat.columns]
    if len(set(columns)) != len(columns):
        raise ValueError(f"table {name!r}: duplicate field names {columns}")
    if not columns and len(flat) > 0:
        raise ValueError("a frame without columns cannot keep its row count")

    fields: list[dict[str, object]] = []
    arrays: list[Any] = []
    raw_columns: list[str] = []
    for position, col in enumerate(columns):
        series = flat.iloc[:, position]
        dtype = series.dtype
        field: dict[str, object] = {"name": col, "dtype": _dtype_text(dtype)}
        if isinstance(dtype, pd.ArrowDtype):
            field["arrow_type"] = _arrow_alias(col, dtype)
        if isinstance(dtype, pd.CategoricalDtype):
            labels, kind = _category_labels(col, dtype.categories)
            field["categories"] = labels
            field["categories_dtype"] = str(dtype.categories.dtype)
            field["ordered"] = bool(dtype.ordered)
            codes = series.cat.codes.to_numpy()
            mask = codes < 0
            if labels:
                pa_type = {"str": pa.string(), "int": pa.int64(), "float": pa.float64()}[kind]
                np_type = {"str": object, "int": "int64", "float": "float64"}[kind]
                taken = dtype.categories.to_numpy(dtype=np_type)
                values = taken.take(np.where(mask, 0, codes))
                arrays.append(pa.array(values, mask=mask, type=pa_type, from_pandas=False))
            else:
                arrays.append(pa.nulls(len(series), type=pa.string()))
        elif dtype == np.dtype(object):
            elem, cells = _object_cells(col, series)
            if elem:
                field["elem"] = elem
                raw_columns.append(col)
            arrays.append(pa.array(cells, type=pa.string()))
        elif isinstance(dtype, pd.StringDtype):
            values = series.to_numpy(dtype=object)
            arrays.append(
                pa.array(values, mask=series.isna().to_numpy(), type=pa.string(), from_pandas=False)
            )
        else:
            _check_dtype(col, dtype)
            arrays.append(_arrow_numeric(series))
        fields.append(field)
    header: Header = {
        "format": FORMAT,
        "format_version": FORMAT_VERSION,
        "table": name,
        "nrows": len(flat),
        "index": index_fields,
        "index_names": [None if n is None else n for n in index.names] if index_fields else [],
        "fields": fields,
        "meta": meta or {},
    }
    return header, pa.Table.from_arrays(arrays, names=columns), raw_columns


def write_table(
    df: pd.DataFrame | DataFrame | _Partitioned,
    path: PathLike,
    *,
    name: str,
    meta: Meta | None = None,
    level: int = 6,
    threads: int = 0,
) -> None:
    """Write ``df`` to ``path`` as one typed table named ``name``.

    ``df`` is a pandas or dftracer.utils ``DataFrame``. A path ending in
    ``.gz`` is written as a multi-member gzip file, any other path as plain
    text. A default ``RangeIndex`` is not stored; any other index becomes
    leading fields, an unnamed level as ``__index_level_<i>__`` that
    :func:`read_table` restores without a name; a level name that is not a str
    is stored as its text and restored as given. ``meta`` is a JSON-serializable
    dict kept in the header. A NaN in an object column is written as null and
    reads back as ``None``.

    A Dask ``DataFrame`` writes the folder ``path`` with one file per
    partition, ``part-00000.pfw.gz`` and on, each a full table.

    Raises ``TypeError`` for a column the format cannot carry (datetime,
    timedelta, an object column of anything but str, sets or lists) and
    ``ValueError`` for duplicate column names, index level names that collide
    with column names, infinite or above-int64 category labels, or a column
    that mixes sets and lists.
    """
    path = os.fspath(path)
    if isinstance(df, _Partitioned):
        _write_partitions(df, path, name, meta, level, threads)
        return
    _write_one(df, path, name, meta, level, threads, False)


def _write_one(
    df: pd.DataFrame | DataFrame,
    path: str,
    name: str,
    meta: Meta | None,
    level: int,
    threads: int,
    keep_index: bool,
) -> None:
    header, table, raw_columns = _prepare(df, name, meta, keep_index)
    frame = DataFrame.from_arrow(table)
    _ext._write_table(frame, path, json.dumps(header, allow_nan=False), raw_columns, level, threads)


def _write_partitions(
    df: _Partitioned, folder: str, name: str, meta: Meta | None, level: int, threads: int
) -> None:
    """Writes the parts into a sibling folder and renames it into place at the
    end, so a failed partition leaves nothing behind."""
    if os.path.exists(folder) and (not os.path.isdir(folder) or os.listdir(folder)):
        raise FileExistsError(f"{folder} already exists and is not an empty folder")
    parent = os.path.dirname(os.path.abspath(folder))
    os.makedirs(parent, exist_ok=True)
    staging = tempfile.mkdtemp(prefix=os.path.basename(folder) + ".", suffix=".tmp", dir=parent)
    try:
        for i in range(df.npartitions):
            part = df.get_partition(i).compute()
            # Every part stores its index, so the parts agree on their fields.
            _write_one(
                part,
                os.path.join(staging, f"part-{i:05d}.pfw.gz"),
                name,
                meta,
                level,
                threads,
                True,
            )
        if os.path.isdir(folder):
            os.rmdir(folder)
        os.rename(staging, folder)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise


# -- decoding ---------------------------------------------------------------


def _flat(column: pa.ChunkedArray) -> pa.Array:
    _, _, pa = _libs()
    if column.num_chunks == 1:
        return column.chunk(0)
    return pa.concat_arrays(column.chunks) if column.num_chunks else pa.array([], type=column.type)


def _resolve(dtype: str) -> Dtype:
    _, pd, _ = _libs()
    try:
        return pd.api.types.pandas_dtype(dtype)
    except TypeError:
        if dtype == "str":
            return pd.api.types.pandas_dtype("object")
        raise


def _restore_category(name: str, arr: pa.Array, field: dict[str, Any]) -> pd.Categorical:
    """A category column. Only a null is missing; a value that is not a label is
    an error. A file with no ``categories_dtype`` (older DFAnalyzer) also takes
    the string "NaN" as missing when "NaN" is no label, and a NaN float."""
    np, pd, pa = _libs()
    labels = list(field.get("categories") or [])
    dtype_text = field.get("categories_dtype")
    cat_dtype: Any = _resolve(dtype_text) if dtype_text else None
    legacy = dtype_text is None
    if cat_dtype is not None and getattr(cat_dtype, "kind", "") == "f":
        labels = [_FLOAT_TEXT.get(c, c) if isinstance(c, str) else c for c in labels]
    elif legacy and not all(isinstance(c, str) for c in labels):
        labels = [_FLOAT_TEXT.get(c, c) if isinstance(c, str) else c for c in labels]
    null = arr.is_null().to_numpy(zero_copy_only=False)
    missing = null.copy()
    text = all(isinstance(c, str) for c in labels)
    if pa.types.is_string(arr.type) and text:
        values = arr.to_numpy(zero_copy_only=False)
        codes = pd.Index(labels, dtype=object).get_indexer(values)
        if legacy and "NaN" not in labels:
            missing |= values == "NaN"
        shown = values
    elif pa.types.is_integer(arr.type) and all(
        isinstance(c, int) and not isinstance(c, bool) for c in labels
    ):
        values = arr.fill_null(0).to_numpy(zero_copy_only=False)
        codes = pd.Index(np.array(labels, dtype=np.int64)).get_indexer(values)
        shown = values
    elif pa.types.is_floating(arr.type) and all(
        isinstance(c, (int, float)) and not isinstance(c, bool) for c in labels
    ):
        values = arr.fill_null(0.0).to_numpy(zero_copy_only=False)
        codes = pd.Index(np.array(labels, dtype=np.float64)).get_indexer(values)
        if legacy:
            missing |= np.isnan(values)
        shown = values
    else:
        # Labels of mixed kinds come as the JSON text of each cell.
        lookup = {c: i for i, c in enumerate(labels)}
        decode = cat_dtype is not None and getattr(cat_dtype, "kind", "") == "f"
        decoded = []
        for t in arr.to_pylist():
            v = None if t is None else json.loads(t)
            decoded.append(_FLOAT_TEXT.get(v, v) if decode and isinstance(v, str) else v)
        codes = np.array(
            [lookup.get(v, -1) if v is not None else -1 for v in decoded], dtype=np.int64
        )
        shown = np.array(decoded, dtype=object)
    bad = (codes < 0) & ~missing
    if bad.any():
        raise ValueError(
            f"column {name!r}: {shown[int(np.argmax(bad))]!r} is not one of its categories"
        )
    codes = np.where(missing, -1, codes)
    categories = pd.Index(labels, dtype=cat_dtype) if cat_dtype is not None else pd.Index(labels)
    return pd.Categorical.from_codes(
        codes, categories=categories, ordered=bool(field.get("ordered", False))
    )


def _restore(name: str, arr: pa.Array, field: dict[str, Any]) -> Column:
    np, pd, pa = _libs()
    dtype = str(field["dtype"])
    if dtype == "category":
        return _restore_category(name, arr, field)
    if dtype == "object":
        elem = field.get("elem")
        if not elem:
            return pd.Series(arr.to_numpy(zero_copy_only=False), dtype=object)
        make = list if elem == "list" else set
        out = np.empty(len(arr), dtype=object)
        for i, t in enumerate(arr.to_pylist()):
            if t is None:
                out[i] = None
                continue
            cell = json.loads(t)
            if not isinstance(cell, list):
                raise ValueError(f"column {name!r}: row {i} is not a JSON array")
            out[i] = make(cell)
        return pd.Series(out, dtype=object)
    resolved = _resolve(dtype)
    if "arrow_type" in field:
        try:
            resolved = pd.ArrowDtype(pa.type_for_alias(str(field["arrow_type"])))
        except ValueError:
            raise ValueError(
                f"column {name!r}: unknown Arrow type {field['arrow_type']!r}"
            ) from None
    if isinstance(resolved, np.dtype):
        if resolved.kind == "f":
            return arr.fill_null(math.nan).to_numpy(zero_copy_only=False).astype(resolved)
        return arr.to_numpy(zero_copy_only=False).astype(resolved)
    if isinstance(resolved, pd.ArrowDtype):
        return pd.arrays.ArrowExtensionArray(arr.cast(resolved.pyarrow_dtype))
    if isinstance(resolved, pd.StringDtype):
        return resolved.__from_arrow__(pa.chunked_array([arr]))
    numpy_dtype = getattr(resolved, "numpy_dtype", None)
    if numpy_dtype is not None:
        fill = False if numpy_dtype == np.dtype(bool) else 0
        data = arr.fill_null(fill).to_numpy(zero_copy_only=False).astype(numpy_dtype)
        mask = arr.is_null().to_numpy(zero_copy_only=False)
        return resolved.construct_array_type()(data, mask)
    raise TypeError(f"dtype {dtype} cannot be restored")


def _read_one(path: str, threads: int) -> tuple[pd.DataFrame, Header]:
    _, pd, _ = _libs()
    frame, header_text = _ext._read_table(path, threads)
    header: Header = json.loads(header_text)
    fields: list[dict[str, Any]] = header["fields"]
    names = [str(f["name"]) for f in fields]
    if len(set(names)) != len(names):
        raise ValueError(f"{path}: duplicate field names {names}")
    table = frame.to_arrow()
    data = {
        n: _restore(n, _flat(table.column(i)), f) for i, (n, f) in enumerate(zip(names, fields))
    }
    df = pd.DataFrame(data, columns=names)
    index_fields: list[str] = header.get("index", [])
    if index_fields:
        df = df.set_index(index_fields)
        df.index.names = header.get("index_names") or index_fields
    return df, header


def _schema(header: Header) -> tuple[list[tuple[str, str]], list[str]]:
    """What parts of a folder must share: field names and dtypes, and the index.
    Categories and ``elem`` depend on a part's content, so they may differ."""
    fields = [(str(f["name"]), str(f["dtype"])) for f in header["fields"]]
    return fields, list(header.get("index") or [])


def read_table(path: PathLike, *, threads: int = 0) -> pd.DataFrame:
    """Read a table written by :func:`write_table` into a pandas frame.

    Dtypes, categories, set and list cells and the index come back as the
    header says. NaN stays apart from NA in a nullable float column; a missing
    cell of an object column is ``None``. A folder
    reads every ``*.pfw.gz`` in it in name order and concatenates the parts,
    which must agree on their fields and index.

    Raises ``DFTUtilsParseError`` for another format, an unknown
    ``format_version``, a row that misses a field or holds a value its dtype
    cannot take, or a row count that differs from the header's;
    ``DFTUtilsCompressionError`` for corrupt gzip data; and ``ValueError`` for
    a cell that is no category label, a set or list cell that is no JSON array,
    or the parts of a folder that disagree.
    """
    _, pd, _ = _libs()
    path = os.fspath(path)
    if not os.path.isdir(path):
        return _read_one(path, threads)[0]
    parts = sorted(glob.glob(os.path.join(path, _PART_GLOB)))
    if not parts:
        raise ValueError(f"{path} holds no {_PART_GLOB} table parts")
    frames: list[Any] = []
    first: Header | None = None
    for part in parts:
        df, header = _read_one(part, threads)
        if first is None:
            first = header
        elif _schema(header) != _schema(first):
            raise ValueError(f"{part}: its fields differ from those of {parts[0]}")
        frames.append(df)
    assert first is not None
    return pd.concat(frames, ignore_index=not first["index"]) if len(frames) > 1 else frames[0]


class TableWriter:
    """Writes several frames of one schema as one table file.

    Use it as a context manager: ``write`` adds a frame, and leaving the block
    writes the file, whose header holds the total row count. The format keeps
    the header on line 1 and the native writer fixes the row count when the
    file starts, so the frames are held in memory until ``close``. Leaving the
    block by an exception writes nothing.
    """

    def __init__(
        self,
        path: PathLike,
        *,
        name: str,
        meta: Meta | None = None,
        level: int = 6,
        threads: int = 0,
    ) -> None:
        self._path = os.fspath(path)
        self._name = name
        self._meta = meta
        self._level = level
        self._threads = threads
        self._frames: list[pd.DataFrame] = []
        self._schema: tuple[object, ...] | None = None
        self._closed = False

    def write(self, df: pd.DataFrame | DataFrame) -> None:
        """Add ``df``, whose columns, dtypes and index levels match the first frame's."""
        if self._closed:
            raise ValueError("write to a closed TableWriter")
        _, pd, _ = _libs()
        if isinstance(df, DataFrame):
            df = df.to_pandas()
        if not isinstance(df, pd.DataFrame):
            raise TypeError(
                f"expected a pandas or dftracer.utils DataFrame, got {type(df).__name__}"
            )
        schema = (tuple(df.index.names), tuple(map(str, df.columns)), tuple(df.dtypes))
        if self._schema is None:
            self._schema = schema
        elif schema != self._schema:
            raise ValueError(
                "the frame's columns, dtypes or index levels differ from the first frame's"
            )
        self._frames.append(df)

    def close(self) -> None:
        """Write the file. Raises ``ValueError`` when no frame was written."""
        if self._closed:
            return
        self._closed = True
        if not self._frames:
            raise ValueError("no frame was written")
        _, pd, _ = _libs()
        frames, self._frames = self._frames, []
        first = frames[0]
        keep_index = not (isinstance(first.index, pd.RangeIndex) and first.index.name is None)
        whole = frames[0] if len(frames) == 1 else pd.concat(frames, ignore_index=not keep_index)
        write_table(
            whole,
            self._path,
            name=self._name,
            meta=self._meta,
            level=self._level,
            threads=self._threads,
        )

    def discard(self) -> None:
        """Drop the frames without writing a file."""
        self._closed = True
        self._frames = []

    def __enter__(self) -> TableWriter:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if exc_type is None:
            self.close()
        else:
            self.discard()


__all__ = ["TableWriter", "read_table", "write_table"]
