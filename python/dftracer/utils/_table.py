# SPDX-License-Identifier: MIT
"""Typed table files: a pandas frame stored as DFTracer-style gzip JSON lines.

Line 1 is a ph 4 record whose args name the format (``dftu.table``, version 1),
the table, the row count, the index levels and every field with its exact
pandas dtype. Every other line is a ph 3 record holding one row in its args.
The file reads with ``zcat`` and ``jq``, and DFAnalyzer's checkpoint tables use
the same format. Encoding and decoding run in C++; this module maps pandas
frames to and from the native frame and makes the headers that
``DataFrame.write`` and the sinks store.
"""

from __future__ import annotations

import json
import math
import os
import re
import types
from typing import TYPE_CHECKING, Any, Union

from . import dftracer_utils_ext as _ext
from ._dataframe import DataFrame, _bound
from ._sink import _raw_columns

if TYPE_CHECKING:
    import numpy as np
    import pandas as pd
    import pyarrow as pa

    from ._enums import DType

    PathLike = Union[str, "os.PathLike[str]"]
    JsonValue = Union[None, bool, int, float, str, list["JsonValue"], dict[str, "JsonValue"]]
    Meta = dict[str, JsonValue]
    Header = dict[str, Any]
    Column = Union["np.ndarray[Any, Any]", "pd.api.extensions.ExtensionArray", "pd.Series"]
    Dtype = Union["np.dtype[Any]", "pd.api.extensions.ExtensionDtype"]

_FLOAT_TEXT = {"NaN": math.nan, "Infinity": math.inf, "-Infinity": -math.inf}
_INT64_MAX = 2**63 - 1


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
    df: pd.DataFrame, name: str, meta: Meta | None, keep_index: bool = False
) -> tuple[Header, pa.Table, list[str]]:
    np, pd, pa = _libs()
    if not isinstance(df, pd.DataFrame):
        raise TypeError(f"expected a pandas or dftracer.utils DataFrame, got {type(df).__name__}")
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
        "format": "dftu.table",
        "format_version": _ext.TABLE_FORMAT_VERSION,
        "table": name,
        "nrows": len(flat),
        "index": index_fields,
        "index_names": [None if n is None else n for n in index.names] if index_fields else [],
        "fields": fields,
        "meta": meta or {},
    }
    return header, pa.Table.from_arrays(arrays, names=columns), raw_columns


def _is_text(t: pa.DataType) -> bool:
    _, _, pa = _libs()
    return bool(
        pa.types.is_string(t)
        or pa.types.is_large_string(t)
        or getattr(pa.types, "is_string_view", lambda _: False)(t)
    )


def _prepare_native(df: DataFrame, name: str, meta: Meta | None) -> Header:
    """The header of a dftracer.utils frame, from its Arrow schema. A column keeps
    its width; one that holds a null takes the nullable dtype, so a null stays
    apart from NaN. Strings are ``object``, a dictionary column of strings a
    ``category``, and the frame's row index names become the header's index."""
    _, _, pa = _libs()
    table = df.to_arrow()
    names = [f.name for f in table.schema]
    if len(set(names)) != len(names):
        raise ValueError(f"table {name!r}: duplicate field names {names}")
    fields: list[dict[str, object]] = []
    for position, arrow_field in enumerate(table.schema):
        col = arrow_field.name
        column = table.column(position)
        t = arrow_field.type
        nulls = column.null_count > 0
        field: dict[str, object] = {"name": col}
        if pa.types.is_dictionary(t) and _is_text(t.value_type):
            chunks = column.unify_dictionaries().chunks
            labels = chunks[0].dictionary.to_pylist() if chunks else []
            field.update(
                dtype="category", categories=labels, categories_dtype="object", ordered=False
            )
        elif pa.types.is_signed_integer(t) or pa.types.is_unsigned_integer(t):
            signed = pa.types.is_signed_integer(t)
            stem = ("Int" if signed else "UInt") if nulls else ("int" if signed else "uint")
            field["dtype"] = f"{stem}{t.bit_width}"
        elif pa.types.is_floating(t) and t.bit_width in (32, 64):
            field["dtype"] = f"Float{t.bit_width}" if nulls else f"float{t.bit_width}"
        elif pa.types.is_boolean(t):
            field["dtype"] = "boolean" if nulls else "bool"
        elif _is_text(t):
            field["dtype"] = "object"
        else:
            raise TypeError(f"column {col!r} has the Arrow type {t}, which a table cannot store")
        fields.append(field)
    index_fields = list(df.index_names())
    return {
        "format": "dftu.table",
        "format_version": _ext.TABLE_FORMAT_VERSION,
        "table": name,
        "nrows": df.num_rows,
        "index": index_fields,
        "index_names": [
            None if re.fullmatch(r"__index_level_\d+__", n) else n for n in index_fields
        ],
        "fields": fields,
        "meta": meta or {},
    }


def _frame_from_pandas(
    df: pd.DataFrame, name: str, meta: Meta | None = None, keep_index: bool = False
) -> DataFrame:
    """A native frame carrying the exact table header of a pandas frame, so
    ``frame.write_table(path)`` stores every dtype, category, Arrow type and set or
    list cell of ``df`` and ``frame.to_pandas()`` gives ``df`` back. A pandas
    default ``RangeIndex`` is not stored unless ``keep_index``; any other index
    becomes leading fields. Raises ``TypeError`` for a column the format cannot
    carry and ``ValueError`` for duplicate column names, colliding index
    names, infinite or above-int64 category labels, or a column that mixes
    sets and lists."""
    header, table, _ = _prepare(df, name, meta, keep_index)
    return _with_header(DataFrame.from_arrow(table), header)


def _write_header(
    df: DataFrame, name: str | None, stem: str, meta: Meta | None
) -> tuple[Header, list[str]]:
    """The header ``df.write`` stores, and its raw columns: the header the frame
    carries while it still describes the frame's columns and Arrow types, else
    one made from the frame's schema."""
    carried = getattr(df, "_table_header", None)
    if carried is not None:
        own = _result_header(df, carried)
        if own is not None:
            table = df.to_arrow()
            fields: list[dict[str, Any]] = own["fields"]
            columns = [_flat(table.column(i)) for i in range(len(fields))]
            if all(_fits(str(f["name"]), a, f) for f, a in zip(fields, columns)):
                own = {**own, "table": name or own.get("table") or stem}
                if meta is not None:
                    own["meta"] = meta
                return own, _raw_columns(own)
    header = _prepare_native(df, name or stem, meta)
    return header, []


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
    if _is_text(arr.type) and text:
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


def _resolved(name: str, field: dict[str, Any]) -> Dtype:
    """The pandas dtype a non-category, non-object field restores to."""
    _, pd, pa = _libs()
    resolved = _resolve(str(field["dtype"]))
    if "arrow_type" in field:
        try:
            resolved = pd.ArrowDtype(pa.type_for_alias(str(field["arrow_type"])))
        except ValueError:
            raise ValueError(
                f"column {name!r}: unknown Arrow type {field['arrow_type']!r}"
            ) from None
    return resolved


def _fits(name: str, arr: pa.Array, field: dict[str, Any]) -> bool:
    """Whether the column's current Arrow type is the one the header implies, so
    that restoring it cannot wrap, truncate or turn a null into a value."""
    np, pd, pa = _libs()
    t = arr.type
    dtype = str(field["dtype"])
    if dtype == "category":
        return bool(_is_text(t) or pa.types.is_integer(t) or pa.types.is_floating(t))
    if dtype == "object":
        return _is_text(t)
    resolved = _resolved(name, field)
    if isinstance(resolved, pd.StringDtype):
        return _is_text(t)
    if isinstance(resolved, pd.ArrowDtype):
        wanted = resolved.pyarrow_dtype
        same = (
            pa.types.is_integer,
            pa.types.is_floating,
            pa.types.is_boolean,
            _is_text,
        )
        return bool(t.equals(wanted) or any(k(t) and k(wanted) for k in same))
    numpy_dtype = (
        resolved if isinstance(resolved, np.dtype) else getattr(resolved, "numpy_dtype", None)
    )
    if numpy_dtype is None:
        return False
    kind = {"b": pa.types.is_boolean, "i": pa.types.is_integer, "u": pa.types.is_integer}.get(
        numpy_dtype.kind, pa.types.is_floating
    )
    if not kind(t):
        return False
    return not (isinstance(resolved, np.dtype) and resolved.kind != "f" and arr.null_count)


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
    resolved = _resolved(name, field)
    # From pandas 3 on, copy-on-write makes the read-only Arrow buffer is safe to share, so a
    # numpy column without nulls keeps it; otherwise a column is copied.
    share = int(pd.__version__.split(".")[0]) >= 3
    if isinstance(resolved, np.dtype):
        if resolved.kind == "f" and arr.null_count:
            return arr.fill_null(math.nan).to_numpy(zero_copy_only=False).astype(resolved)
        values = arr.cast(pa.from_numpy_dtype(resolved), safe=True).to_numpy(zero_copy_only=False)
        return values.astype(resolved, copy=not share)
    if isinstance(resolved, pd.ArrowDtype):
        return pd.arrays.ArrowExtensionArray(arr.cast(resolved.pyarrow_dtype, safe=True))
    if isinstance(resolved, pd.StringDtype):
        return resolved.__from_arrow__(pa.chunked_array([arr]))
    numpy_dtype = getattr(resolved, "numpy_dtype", None)
    if numpy_dtype is not None and isinstance(resolved, pd.api.extensions.ExtensionDtype):
        fill = False if numpy_dtype == np.dtype(bool) else 0
        filled = arr.fill_null(fill) if arr.null_count else arr
        data = (
            filled.cast(pa.from_numpy_dtype(numpy_dtype), safe=True)
            .to_numpy(zero_copy_only=False)
            .astype(numpy_dtype, copy=not share)
        )
        mask = arr.is_null().to_numpy(zero_copy_only=False)
        return resolved.construct_array_type()(data, mask)
    raise TypeError(f"dtype {field['dtype']} cannot be restored")


def _exact_pandas(frame: DataFrame, header: Header) -> pd.DataFrame | None:
    """The exact pandas frame a table header describes, or None when a column of
    ``frame`` no longer has the name or Arrow type the header implies."""
    _, pd, pa = _libs()
    fields: list[dict[str, Any]] = header["fields"]
    names = [str(f["name"]) for f in fields]
    if frame._names() != names:
        return None
    table = frame.to_arrow()
    columns = [_flat(table.column(i)) for i in range(len(names))]
    if not all(_fits(n, a, f) for n, a, f in zip(names, columns, fields)):
        return None
    try:
        data = {n: _restore(n, a, f) for n, a, f in zip(names, columns, fields)}
    except pa.ArrowInvalid:
        return None
    df = pd.DataFrame(data, columns=names, copy=False)
    index_fields: list[str] = header.get("index", [])
    if index_fields:
        df = df.set_index(index_fields)
        df.index.names = header.get("index_names") or index_fields
    return df


def _merged_header(headers: list[Header]) -> Header:
    """The header of the concatenated parts: the first part's, with the total
    row count and each category field's labels unified in first-seen order. The
    native reader has checked that the parts agree on names, dtypes, ordered,
    elem, arrow_type and index names."""
    merged: Header = json.loads(json.dumps(headers[0]))
    merged["nrows"] = sum(int(h["nrows"]) for h in headers)
    for position, field in enumerate(merged["fields"]):
        if "categories" not in field:
            continue
        known = list(field["categories"])
        seen = set(map(repr, known))
        for h in headers[1:]:
            for c in h["fields"][position].get("categories", []):
                if repr(c) not in seen:
                    seen.add(repr(c))
                    known.append(c)
        field["categories"] = known
    return merged


def _table_header(texts: list[str]) -> Header:
    """The merged header of table files whose header args are ``texts``."""
    headers: list[Header] = [json.loads(t) for t in texts]
    for h in headers:
        names = [str(f["name"]) for f in h["fields"]]
        if len(set(names)) != len(names):
            raise ValueError(f"duplicate field names {names}")
    return _merged_header(headers)


def _result_header(frame: DataFrame, header: Header) -> Header | None:
    """The header for ``frame``, a result of table rows: ``header``'s fields that
    are the frame's columns, in the frame's order, with the frame's row count.
    None when a column of the frame is not a field of the header, which means
    the plan made it."""
    by_name = {str(f["name"]): f for f in header["fields"]}
    names = frame._names()
    if any(n not in by_name for n in names):
        return None
    out: Header = {**header}
    out["fields"] = [by_name[n] for n in names]
    out["nrows"] = frame.num_rows
    index = [i for i in header.get("index") or []]
    if not all(i in by_name and i in names for i in index):
        out["index"] = []
        out["index_names"] = []
    return out


def _with_header(frame: DataFrame, header: Header) -> DataFrame:
    """``frame`` with the header's index, carrying the header for to_pandas."""
    index_fields: list[str] = header.get("index") or []
    if index_fields:
        frame = _bound(_ext.DataFrame.with_index(frame, index_fields))
    frame._table_header = header
    return frame


def _typed_empty(schema: dict[str, DType]) -> DataFrame | None:
    """A frame of no rows with the columns of ``schema``, or None when a dtype is
    not one a table reads. A streamed plan that filters every row out sends no
    morsel, so its collect has no columns of its own."""
    _, _, pa = _libs()
    from ._enums import DType as Kind

    arrow = {
        Kind.BOOL: pa.bool_(),
        Kind.INT64: pa.int64(),
        Kind.UINT64: pa.uint64(),
        Kind.FLOAT64: pa.float64(),
        Kind.STRING: pa.string(),
    }
    if not schema or any(t not in arrow for t in schema.values()):
        return None
    table = pa.table({n: pa.array([], type=arrow[t]) for n, t in schema.items()})
    return DataFrame.from_arrow(table)


def _carry_header(frame: DataFrame, header: Header | None) -> DataFrame:
    """``frame`` carrying the header made for it, when ``header`` still describes
    its columns."""
    if header is None:
        return frame
    own = _result_header(frame, header)
    return frame if own is None else _with_header(frame, own)
