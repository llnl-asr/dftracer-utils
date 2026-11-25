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
from typing import TYPE_CHECKING, Any, Callable, Dict, List, Tuple, Union

from . import dftracer_utils_ext as _ext
from ._convert import (
    _DICTIONARY,
    _one_dictionary,
    arrow_native_type,
    clean_dictionary,
    flat,
    null_mask,
    texts,
)
from ._dataframe import DataFrame, _bound
from ._enums import DType
from ._sink import _raw_columns

if TYPE_CHECKING:
    import numpy as np
    import numpy.typing as npt
    import pandas as pd

    from ._series import Series

    PathLike = Union[str, "os.PathLike[str]"]
    # A MultiIndex level as its values and a code per row.
    CodedLevel = Tuple[pd.Index, npt.NDArray[Any]]
    JsonValue = Union[None, bool, int, float, str, List["JsonValue"], Dict[str, "JsonValue"]]
    Meta = Dict[str, JsonValue]
    SetType = Callable[[List[JsonValue]], object]
    Header = Dict[str, Any]
    Column = Union["np.ndarray[Any, Any]", "pd.api.extensions.ExtensionArray", "pd.Series"]
    Dtype = Union["np.dtype[Any]", "pd.api.extensions.ExtensionDtype"]

_FLOAT_TEXT = {"NaN": math.nan, "Infinity": math.inf, "-Infinity": -math.inf}
_INT64_MAX = 2**63 - 1


def _float_text(value: float) -> str:
    """The text a table stores for a float that JSON cannot hold."""
    if math.isnan(value):
        return "NaN"
    return "Infinity" if value > 0 else "-Infinity"


def _pandas_libs() -> tuple[types.ModuleType, types.ModuleType]:
    import numpy as np
    import pandas as pd

    return np, pd


# -- encoding ---------------------------------------------------------------


def _scalar(value: object) -> object:
    """The JSON value of one element of a set or list cell. A NaN or infinity
    element would read back as the string it is written as, so it is
    refused."""
    np, pd = _pandas_libs()
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
    elements. A column of str and null cells only gives no cells, as the
    column imports as it is. A column of sets and lists mixed, of nested containers, or of
    any other object is refused.
    """
    _, pd = _pandas_libs()
    if pd.api.types.infer_dtype(values, skipna=True) in ("string", "empty"):
        return None, []
    kinds = set()
    for v in values:
        if isinstance(v, (set, frozenset)):
            kinds.add("set")
        elif isinstance(v, (list, tuple)):
            kinds.add("list")
    if len(kinds) > 1:
        raise ValueError(f"column {name!r} mixes sets and lists")
    elem = kinds.pop() if kinds else None
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
    np, pd = _pandas_libs()
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
        # JSON has no infinity: an infinite label is the text of an infinite cell.
        floats = [float(c) for c in categories]
        return [f if math.isfinite(f) else _float_text(f) for f in floats], "float"
    if all(isinstance(c, str) for c in categories):
        return list(categories), "str"
    raise TypeError(f"column {name!r}: categories must be all str or all numbers")


def _arrow_alias(name: str, dtype: pd.ArrowDtype) -> str:
    """The text that rebuilds an ArrowDtype's pyarrow type with
    ``pa.type_for_alias``, which the reader needs because ``string[pyarrow]``
    names both ArrowDtype(pa.string()) and StringDtype("pyarrow")."""
    import pyarrow as pa

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
    _, pd = _pandas_libs()
    if isinstance(dtype, pd.StringDtype) and dtype.na_value is pd.NA:
        return f"string[{dtype.storage}]"
    return str(dtype)


def _arrow_describable(dtype: pd.ArrowDtype) -> bool:
    import pyarrow as pa

    t = dtype.pyarrow_dtype
    return bool(
        pa.types.is_integer(t)
        or pa.types.is_boolean(t)
        or pa.types.is_string(t)
        or pa.types.is_large_string(t)
        or (pa.types.is_floating(t) and not pa.types.is_float16(t))
    )


def _pandas_field(name: str, series: pd.Series) -> tuple[dict[str, object], pd.Series] | None:
    """The header field of one pandas column and the column to import (an object
    column of sets or lists as the JSON text of its cells). None for a dtype a table cannot describe, which the frame's
    schema then decides when it is written. Raises ``TypeError`` or
    ``ValueError`` for a value the format refuses."""
    np, pd = _pandas_libs()
    dtype = series.dtype
    field: dict[str, object] = {"name": name, "dtype": _dtype_text(dtype)}
    if isinstance(dtype, pd.CategoricalDtype):
        labels, _ = _category_labels(name, dtype.categories)
        field["categories"] = labels
        field["categories_dtype"] = str(dtype.categories.dtype)
        field["ordered"] = bool(dtype.ordered)
        return field, series
    if dtype == np.dtype(object):
        elem, cells = _object_cells(name, series)
        if elem is None:
            return field, series
        field["elem"] = elem
        return field, pd.Series(cells, index=series.index, dtype=object)
    if isinstance(dtype, pd.StringDtype):
        return field, series
    if isinstance(dtype, pd.ArrowDtype):
        if not _arrow_describable(dtype):
            return None
        field["arrow_type"] = _arrow_alias(name, dtype)
        return field, series
    if isinstance(dtype, np.dtype):
        if dtype.kind in "iub" or (dtype.kind == "f" and dtype.itemsize in (4, 8)):
            return field, series
        return None
    if getattr(dtype, "numpy_dtype", None) is not None and getattr(dtype, "kind", "") in "iufb":
        return field, series
    return None


def _describe_pandas(
    df: pd.DataFrame,
    columns: list[tuple[str, pd.Series]],
    levels: list[tuple[str, pd.Series]],
) -> tuple[list[tuple[str, pd.Series]], Header | None]:
    """The columns ``DataFrame.from_pandas`` imports for ``df`` and the table
    header that describes them, built from the pandas dtypes with no Arrow
    conversion, so ``write_table`` stores every dtype, category and set or list
    cell and ``to_pandas`` gives ``df`` back. The header is None when one column
    has a dtype a table cannot describe; the schema path then decides."""
    for level in df.index.names if levels else ():
        if level is not None and not isinstance(level, (str, int)):
            raise TypeError(f"index level name {level!r} cannot be stored in a table")
    fields: list[dict[str, object]] = []
    imported: list[tuple[str, pd.Series]] = []
    describable = True
    for name, series in columns + levels:
        described = _pandas_field(name, series)
        if described is None:
            describable = False
            imported.append((name, series))
            continue
        field, column = described
        fields.append(field)
        imported.append((name, column))
    if not describable:
        return imported, None
    index_fields = [n for n, _ in levels]
    header: Header = {
        "format": "dftu.table",
        "format_version": _ext.TABLE_FORMAT_VERSION,
        "table": "",
        "nrows": len(df),
        "index": index_fields,
        "index_names": [None if n is None else n for n in df.index.names] if levels else [],
        "fields": fields,
        "meta": {},
    }
    return imported, header


def _column(frame: DataFrame, name: str) -> Series:
    return _ext.DataFrame.__getitem__(frame, name)


def _prepare_native(df: DataFrame, name: str, meta: Meta | None) -> Header:
    """The header of a dftracer.utils frame, from its native column types. A
    column keeps its width; one that holds a null takes the nullable dtype, so
    a null stays apart from NaN. Strings are ``object``, a dictionary column of
    strings a ``category``, and the frame's row index names become the
    header's index."""
    names = df._names()
    if len(set(names)) != len(names):
        raise ValueError(f"table {name!r}: duplicate field names {names}")
    fields: list[dict[str, object]] = []
    for col in names:
        native = _one_dictionary(_column(df, col))
        t = DType(native.type)
        nulls = native.null_count > 0
        field: dict[str, object] = {"name": col}
        if native.encoding == _DICTIONARY and t.is_text:
            _, values = native._dictionary_parts()
            field.update(
                dtype="category",
                categories=values.to_list(),
                categories_dtype="object",
                ordered=False,
            )
        elif t.is_integer:
            stem = t.name.lower().capitalize() if nulls else t.name.lower()
            field["dtype"] = stem.replace("Uint", "UInt")
        elif t in (DType.FLOAT32, DType.FLOAT64):
            field["dtype"] = t.name.capitalize() if nulls else t.name.lower()
        elif t == DType.BOOL:
            field["dtype"] = "boolean" if nulls else "bool"
        elif t.is_text:
            field["dtype"] = "object"
        else:
            raise TypeError(
                f"column {col!r} has the type {t.name.lower()}, which a table cannot store"
            )
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


def _write_header(
    df: DataFrame, name: str | None, stem: str, meta: Meta | None
) -> tuple[Header, list[str]]:
    """The header ``df.write`` stores, and its raw columns: the header the frame
    carries while it still describes the frame's columns and types, else one
    made from the frame's column types."""
    carried = getattr(df, "_table_header", None)
    if carried is not None:
        own = _result_header(df, carried)
        if own is not None:
            fields: list[dict[str, Any]] = own["fields"]
            if all(_fits(str(f["name"]), _column(df, str(f["name"])), f) for f in fields):
                own = {**own, "table": name or own.get("table") or stem}
                if meta is not None:
                    own["meta"] = meta
                return own, _raw_columns(own)
    header = _prepare_native(df, name or stem, meta)
    return header, []


# -- decoding ---------------------------------------------------------------


class _DoesNotFit(ValueError):
    """A column value that the header's dtype cannot hold."""


def _in_range(name: str, native: Series, dtype: np.dtype[Any]) -> None:
    """``_DoesNotFit`` when an integer value of ``native`` is outside ``dtype``."""
    np, _ = _pandas_libs()
    if dtype.kind not in "iu" or native.length == native.null_count:
        return
    own = DType(native.type)
    if own.is_integer and np.can_cast(np.dtype(own.name.lower()), dtype, "safe"):
        return
    info = np.iinfo(dtype)
    low, high = native.min(), native.max()
    if low < info.min or high > info.max:
        raise _DoesNotFit(f"column {name!r}: a value does not fit {dtype}")


def _numpy(
    name: str,
    native: Series,
    dtype: np.dtype[Any],
    fill: bool | float | None = None,
) -> npt.NDArray[Any]:
    """``native`` as one writable NumPy array of ``dtype``, nulls set to
    ``fill``: one copy of the native values, in the target dtype."""
    np, _ = _pandas_libs()
    _in_range(name, native, dtype)
    if native.null_count and fill is not None:
        native = native.fill_null(fill)
    native = flat(native)
    values: npt.NDArray[Any]
    if DType(native.type) == DType.BOOL:
        # already a copy: the native bits widened to a byte per row
        values = np.asarray(native.to_numpy())
        owned = True
    else:
        values = np.asarray(native._values_buffer())
        owned = False
    out: npt.NDArray[Any] = values.astype(dtype, copy=not owned)
    return out


def _resolve(dtype: str) -> Dtype:
    _, pd = _pandas_libs()
    try:
        return pd.api.types.pandas_dtype(dtype)
    except TypeError:
        if dtype == "str":
            return pd.api.types.pandas_dtype("object")
        raise


def _restore_category(name: str, native: Series, field: dict[str, Any]) -> pd.Categorical:
    """A category column. Only a null is missing; a value that is not a label is
    an error. A file with no ``categories_dtype`` (older DFAnalyzer) also takes
    the string "NaN" as missing when "NaN" is no label, and a NaN float."""
    np, pd = _pandas_libs()
    labels = list(field.get("categories") or [])
    dtype_text = field.get("categories_dtype")
    cat_dtype: Any = _resolve(dtype_text) if dtype_text else None
    legacy = dtype_text is None
    if cat_dtype is not None and getattr(cat_dtype, "kind", "") == "f":
        labels = [_FLOAT_TEXT.get(c, c) if isinstance(c, str) else c for c in labels]
    elif legacy and not all(isinstance(c, str) for c in labels):
        labels = [_FLOAT_TEXT.get(c, c) if isinstance(c, str) else c for c in labels]
    t = DType(native.type)
    missing = null_mask(native)
    text = all(isinstance(c, str) for c in labels)
    if t.is_text and text:
        # One lookup per distinct string, not per row.
        codes, dictionary = clean_dictionary(native)._dictionary_parts()
        distinct = texts(dictionary)
        position = pd.Index(labels, dtype=object).get_indexer(distinct)
        # the code under a null row is unspecified, so it is set to 0 first
        raw = np.where(null_mask(codes), 0, np.asarray(flat(codes)._values_buffer()))
        codes_out = position[raw] if len(distinct) else np.full(len(raw), -1)
        if legacy and "NaN" not in labels:
            missing |= (distinct == "NaN")[raw] if len(distinct) else False
        shown = distinct[raw] if len(distinct) else raw
    elif t.is_integer and all(isinstance(c, int) and not isinstance(c, bool) for c in labels):
        values = _numpy(name, native, np.dtype(np.int64), 0)
        codes_out = pd.Index(np.array(labels, dtype=np.int64)).get_indexer(values)
        shown = values
    elif t.is_float and all(
        isinstance(c, (int, float)) and not isinstance(c, bool) for c in labels
    ):
        values = _numpy(name, native, np.dtype(np.float64), 0.0)
        codes_out = pd.Index(np.array(labels, dtype=np.float64)).get_indexer(values)
        if legacy:
            missing |= np.isnan(values)
        shown = values
    else:
        # Labels of mixed kinds come as the JSON text of each cell.
        lookup = {c: i for i, c in enumerate(labels)}
        decode = cat_dtype is not None and getattr(cat_dtype, "kind", "") == "f"
        decoded = []
        for cell in texts(native):
            v = None if cell is None else json.loads(cell)
            decoded.append(_FLOAT_TEXT.get(v, v) if decode and isinstance(v, str) else v)
        codes_out = np.array(
            [lookup.get(v, -1) if v is not None else -1 for v in decoded], dtype=np.int64
        )
        shown = np.array(decoded, dtype=object)
    bad = (codes_out < 0) & ~missing
    if bad.any():
        raise ValueError(
            f"column {name!r}: {shown[int(np.argmax(bad))]!r} is not one of its categories"
        )
    codes_out = np.where(missing, -1, codes_out)
    categories = pd.Index(labels, dtype=cat_dtype) if cat_dtype is not None else pd.Index(labels)
    return pd.Categorical.from_codes(
        codes_out, categories=categories, ordered=bool(field.get("ordered", False))
    )


def _resolved(name: str, field: dict[str, Any]) -> Dtype:
    """The pandas dtype a non-category, non-object field restores to."""
    _, pd = _pandas_libs()
    resolved = _resolve(str(field["dtype"]))
    if "arrow_type" in field:
        import pyarrow as pa

        try:
            resolved = pd.ArrowDtype(pa.type_for_alias(str(field["arrow_type"])))
        except ValueError:
            raise ValueError(
                f"column {name!r}: unknown Arrow type {field['arrow_type']!r}"
            ) from None
    return resolved


def _kind(t: DType) -> str:
    """The family of a native type that a header dtype may restore from."""
    if t.is_integer:
        return "i"
    if t.is_float:
        return "f"
    if t == DType.BOOL:
        return "b"
    return "s" if t.is_text else ""


def _fits(name: str, native: Series, field: dict[str, Any]) -> bool:
    """Whether the column's current type is one the header's dtype restores
    from, so that restoring it cannot wrap, truncate or turn a null into a
    value."""
    np, pd = _pandas_libs()
    kind = _kind(DType(native.type))
    dtype = str(field["dtype"])
    if dtype == "category":
        labels = field.get("categories") or []
        float_labels = str(field.get("categories_dtype") or "").startswith("float")
        if labels and all(isinstance(c, str) for c in labels) and not float_labels:
            return kind == "s"
        return kind in "sif" and kind != ""
    if dtype == "object":
        return kind == "s"
    resolved = _resolved(name, field)
    if isinstance(resolved, pd.StringDtype):
        return kind == "s"
    if isinstance(resolved, pd.ArrowDtype):
        try:
            wanted, _, _ = arrow_native_type(resolved.pyarrow_dtype)
        except TypeError:
            return False
        return kind != "" and kind == _kind(wanted)
    numpy_dtype = (
        resolved if isinstance(resolved, np.dtype) else getattr(resolved, "numpy_dtype", None)
    )
    if numpy_dtype is None:
        return False
    if kind != {"b": "b", "i": "i", "u": "i"}.get(numpy_dtype.kind, "f"):
        return False
    return not (isinstance(resolved, np.dtype) and resolved.kind != "f" and native.null_count)


def _restore(name: str, native: Series, field: dict[str, Any], set_type: SetType = set) -> Column:
    np, pd = _pandas_libs()
    dtype = str(field["dtype"])
    if dtype == "category":
        return _restore_category(name, native, field)
    if dtype == "object":
        elem = field.get("elem")
        if not elem:
            return pd.Series(texts(native), dtype=object)
        make: SetType = list if elem == "list" else set_type
        out = np.empty(native.length, dtype=object)
        for i, t in enumerate(texts(native)):
            if t is None:
                out[i] = None
                continue
            cell = json.loads(t)
            if not isinstance(cell, list):
                raise ValueError(f"column {name!r}: row {i} is not a JSON array")
            out[i] = make(cell)
        return pd.Series(out, dtype=object)
    resolved = _resolved(name, field)
    if isinstance(resolved, np.dtype):
        if resolved.kind == "f" and native.null_count:
            return _numpy(name, native, resolved, math.nan)
        return _numpy(name, native, resolved)
    if isinstance(resolved, pd.ArrowDtype):
        # The column the user stored as an Arrow dtype: our buffers, retyped
        # natively when the width differs.
        import pyarrow as pa

        wanted, _, _ = arrow_native_type(resolved.pyarrow_dtype)
        # the dtype holds plain values: a dictionary, one column or chunked, is
        # decoded natively (a copy)
        native = flat(_one_dictionary(native), keep_chunks=True)
        if DType(native.type) != wanted:
            if wanted.is_integer:
                _in_range(name, native, np.dtype(wanted.name.lower()))
            native = native._cast(int(wanted))
        return pd.arrays.ArrowExtensionArray(pa.chunked_array(native._plain_text()))
    if isinstance(resolved, pd.StringDtype):
        if resolved.storage == "pyarrow":
            import pyarrow as pa

            plain = flat(_one_dictionary(native), keep_chunks=True)._plain_text()
            return resolved.__from_arrow__(pa.chunked_array(plain))
        return pd.array(texts(native, resolved.na_value), dtype=resolved)
    numpy_dtype = getattr(resolved, "numpy_dtype", None)
    if numpy_dtype is not None and isinstance(resolved, pd.api.extensions.ExtensionDtype):
        fill = False if numpy_dtype == np.dtype(bool) else 0
        mask = null_mask(native)
        data = _numpy(name, native, numpy_dtype, fill)
        return resolved.construct_array_type()(data, mask)
    raise TypeError(f"dtype {field['dtype']} cannot be restored")


def _exact_pandas(frame: DataFrame, header: Header, set_type: SetType = set) -> pd.DataFrame | None:
    """The exact pandas frame a table header describes, or None when a column of
    ``frame`` no longer has the name or type the header implies."""
    _, pd = _pandas_libs()
    fields: list[dict[str, Any]] = header["fields"]
    names = [str(f["name"]) for f in fields]
    if frame._names() != names:
        return None
    columns = [_column(frame, n) for n in names]
    if not all(_fits(n, a, f) for n, a, f in zip(names, columns, fields)):
        return None
    index_fields: list[str] = header.get("index", [])
    # Levels of a MultiIndex come from the column dictionaries; a level that
    # cannot is restored whole and factorized by pandas.
    coded: dict[str, CodedLevel | None] = {}
    try:
        if len(index_fields) > 1:
            for n in index_fields:
                i = names.index(n)
                coded[n] = _index_level(n, columns[i], fields[i], set_type)
        data = {
            n: _restore(n, a, f, set_type)
            for n, a, f in zip(names, columns, fields)
            if coded.get(n) is None
        }
    except _DoesNotFit:
        return None
    if not index_fields:
        return pd.DataFrame(data, columns=names, copy=False)
    # The index is set in place: set_index would copy every data column.
    level_names = header.get("index_names") or index_fields
    if len(index_fields) == 1:
        level = data.pop(index_fields[0])
        df = pd.DataFrame(data, columns=list(data), copy=False)
        df.index = pd.Index(level, name=level_names[0])
        return df
    levels: list[Any] = []
    codes: list[Any] = []
    for n in index_fields:
        pair = coded.get(n)
        if pair is None:
            alone = pd.MultiIndex.from_arrays([data.pop(n)])
            pair = (alone.levels[0], alone.codes[0])
        levels.append(pair[0])
        codes.append(pair[1])
    del coded
    df = pd.DataFrame(data, columns=list(data), copy=False)
    df.index = pd.MultiIndex(levels=levels, codes=codes, names=level_names, verify_integrity=False)
    return df


def _index_level(
    name: str, native: Series, field: dict[str, Any], set_type: SetType
) -> CodedLevel | None:
    """One MultiIndex level as its sorted values and a code per row, as
    ``MultiIndex.from_arrays`` makes it, built natively so no value is made per
    row: from the dictionary of a string column, from the sorted distinct values
    of an integer column. None for a column of another kind."""
    np, pd = _pandas_libs()
    if str(field["dtype"]) == "category" or field.get("elem") or native.length == 0:
        return None
    kind = DType(native.type)
    if kind.is_integer:
        codes, distinct = native.factorize()
        values = pd.Index(_restore(name, distinct, field, set_type))
        return values, np.asarray(codes._values_buffer()).astype(np.intp)
    if not kind.is_text:
        return None
    encoded = clean_dictionary(native)
    codes, dictionary = encoded._dictionary_parts()
    valid = null_mask(codes) == 0
    # the code under a null row is unspecified: it points past the dictionary
    raw = np.where(valid, np.asarray(flat(codes)._values_buffer()), len(dictionary)).astype(np.intp)
    # The dictionary may hold an entry only null rows point at; it is no level value.
    used = np.bincount(raw[valid], minlength=len(dictionary)) > 0
    kept = np.flatnonzero(used)
    values = pd.Index(_restore(name, dictionary.take(kept.tolist()), field, set_type))
    order = values.argsort()
    rank: npt.NDArray[Any] = np.full(len(dictionary) + 1, -1, dtype=np.intp)
    rank[kept[order]] = np.arange(len(order))
    out: npt.NDArray[Any] = np.where(valid, rank[raw], -1)
    return values.take(order), out


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
    kinds = (DType.BOOL, DType.INT64, DType.UINT64, DType.FLOAT64, DType.STRING)
    if not schema or any(t not in kinds for t in schema.values()):
        return None
    columns = [_ext._series_from_list([], int(t)) for t in schema.values()]
    return DataFrame(_ext._dataframe_from_columns(list(schema), columns))


def _carry_header(frame: DataFrame, header: Header | None) -> DataFrame:
    """``frame`` carrying the header made for it, when ``header`` still describes
    its columns."""
    if header is None:
        return frame
    own = _result_header(frame, header)
    return frame if own is None else _with_header(frame, own)
