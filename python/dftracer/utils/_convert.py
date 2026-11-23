# SPDX-License-Identifier: MIT
"""Conversions between native columns and NumPy, pandas, polars and pyarrow.

Out of a column (``Series.to_numpy`` / ``to_pandas`` / ``to_polars`` /
``to_arrow``): the helpers that build the NumPy, pandas and polars forms, most
of them without pyarrow. Into a column (``Series.from_numpy`` / ``from_pandas`` /
``from_polars``): the importers, which return a native ``_Series`` or raise
``TypeError`` for data they cannot read; ``NeedsArrow`` marks data that only
pyarrow can read (a pandas Arrow-backed column, a polars list or struct column),
so callers may fall back to it when it is installed.
"""

from __future__ import annotations

import functools
import re
from typing import TYPE_CHECKING, Any, Callable, Protocol, Sequence, cast

from . import dftracer_utils_ext as _ext
from ._enums import UNIT_CODES, DType

_ARROW_ARRAY = hasattr(_ext.Series, "__arrow_c_array__")
_ARROW_IMPORT = hasattr(_ext, "_series_from_arrow")


@functools.lru_cache(maxsize=None)
def _pyarrow() -> ModuleType | None:
    """pyarrow, or None when it is not installed; looked up once."""
    try:
        import pyarrow as pa
    except ImportError:
        return None
    return cast("ModuleType", pa)


if TYPE_CHECKING:
    from datetime import tzinfo
    from types import ModuleType

    import numpy as np
    import numpy.typing as npt
    import pandas as pd
    import polars as pl
    import pyarrow as pa
    from pandas._libs.missing import NAType
    from pandas.api.extensions import ExtensionArray
    from polars import Series as PolarsSeries

    from ._series import Series


class _MaskedDtype(Protocol):
    numpy_dtype: np.dtype[Any]


def _to_pandas(
    arrow_obj: pa.Table | pa.Array | pa.ChunkedArray, arrow: bool
) -> pd.DataFrame | pd.Series:
    """``pyarrow`` to pandas: NumPy dtypes (a copy) or, with ``arrow``, the
    ``pd.ArrowDtype`` columns that share the Arrow buffers."""
    if not arrow:
        return arrow_obj.to_pandas()
    import pandas as pd

    return arrow_obj.to_pandas(types_mapper=pd.ArrowDtype)


_POLARS_DTYPES: dict[DType, str] = {
    DType.BOOL: "Boolean",
    DType.INT8: "Int8",
    DType.INT16: "Int16",
    DType.INT32: "Int32",
    DType.INT64: "Int64",
    DType.UINT8: "UInt8",
    DType.UINT16: "UInt16",
    DType.UINT32: "UInt32",
    DType.UINT64: "UInt64",
    DType.FLOAT32: "Float32",
    DType.FLOAT64: "Float64",
    DType.STRING: "String",
    DType.LARGE_STRING: "String",
    DType.BINARY: "Binary",
    DType.LARGE_BINARY: "Binary",
}


_NATIVE_OBJECT_TYPES = (
    DType.STRING,
    DType.LARGE_STRING,
    DType.BINARY,
    DType.LARGE_BINARY,
    DType.LIST,
    DType.LARGE_LIST,
    DType.STRUCT,
)


_NAT = -(1 << 63)


_TEMPORAL_KINDS = {
    DType.DATE32: "D",
    DType.DATE64: "ms",
    DType.TIMESTAMP: None,
    DType.DURATION: None,
}


_PYTHON_OBJECT_TYPES = (
    DType.TIME32,
    DType.TIME64,
    DType.DECIMAL128,
    DType.DECIMAL256,
    DType.FIXED_SIZE_BINARY,
    DType.FIXED_SIZE_LIST,
    DType.MAP,
)


def _temporal_numpy(native: Series, dtype: DType) -> npt.NDArray[Any]:
    """A date, timestamp or duration column as ``datetime64`` or ``timedelta64``
    (UTC values for a zoned timestamp), with ``NaT`` for null."""
    import numpy as np

    unit = _TEMPORAL_KINDS[dtype] or tuple(UNIT_CODES)[native.time_unit]
    phys: Series = native._physical()
    if dtype == DType.DATE32:
        phys = phys._cast(int(DType.INT64))
    if phys.null_count:
        phys = phys.fill_null(_NAT)
    kind = "timedelta64" if dtype == DType.DURATION else "datetime64"
    return np.array(memoryview(phys)).view(f"{kind}[{unit}]")


def _object_numpy(values: Sequence[object]) -> npt.NDArray[Any]:
    import numpy as np

    out = np.empty(len(values), dtype=object)
    for i, v in enumerate(values):
        out[i] = v
    return out


def _parts(
    native: Series, nullable: bool, na: NAType | None = None
) -> tuple[bytearray, bytearray | None, str]:
    """The column's values, null mask and numpy dtype string from one native
    pass (``Series._np_parts``): bytearrays that numpy wraps without a copy."""
    return native._np_parts(nullable, na)


def _frombuffer(data: bytes | bytearray | memoryview, dtype: str) -> npt.NDArray[Any]:
    import numpy as np

    return np.frombuffer(data, dtype=dtype)


def _native_numpy(native: Series) -> npt.NDArray[Any]:
    """The native column as a NumPy array without pyarrow; TypeError for a
    type with no native conversion."""
    import numpy as np

    try:
        # memoryview forces the native buffer protocol (a plain
        # np.asarray(handle) can fall back to a 0-d object array).
        # The native buffer support is not in the stub (PEP 688 is 3.12+).
        return np.asarray(memoryview(native))
    except (BufferError, TypeError):
        pass
    if DType(native.type) in (DType.STRING, DType.LARGE_STRING):
        out = np.empty(native.length, dtype=object)
        native._str_into(out, None)
        return out
    if native.encoding != 0:
        native = native.materialize()
    dtype = DType(native.type)
    nulls = native.null_count
    if dtype.is_numeric:
        values, _, dt = _parts(native, False)
        return _frombuffer(values, dt)
    if dtype in _TEMPORAL_KINDS:
        return _temporal_numpy(native, dtype)
    if dtype == DType.FLOAT16:
        return np.array(
            [float("nan") if v is None else v for v in native._to_pylist()], dtype=np.float16
        )
    if dtype == DType.BOOL and not nulls:
        values, _, _ = _parts(native, False)
        return _frombuffer(values, "u1").view(np.bool_)
    if dtype == DType.BOOL or dtype in _NATIVE_OBJECT_TYPES or dtype in _PYTHON_OBJECT_TYPES:
        return _object_numpy(native._to_pylist())
    raise TypeError(f"no native conversion for column type '{dtype.name.lower()}'")


_POLARS_NATIVE_ONLY = (DType.FLOAT16, DType.DECIMAL128, DType.DECIMAL256)


def _polars_via_arrow(native: Series) -> bool:
    """Whether the Arrow capsule gives polars the dtype the native path does: not
    for Float16 and decimals."""
    return _ARROW_ARRAY and DType(native.type) not in _POLARS_NATIVE_ONLY


def _polars_native(native: Series, pl: ModuleType) -> pl.Series:
    """The column as a polars Series built natively, with no pyarrow."""
    dtype = DType(native.type)
    if dtype in _TEMPORAL_KINDS:
        values = _temporal_numpy(native, dtype)
        if values.dtype.name.endswith("[s]"):
            values = values.astype(values.dtype.name.replace("[s]", "[ms]"))
        out: PolarsSeries = pl.Series(values)
        if dtype == DType.TIMESTAMP and native.timezone:
            out = out.dt.replace_time_zone("UTC").dt.convert_time_zone(native.timezone)
        return out
    if dtype == DType.FLOAT16:
        import numpy as np

        floats: PolarsSeries = pl.Series(_native_numpy(native).astype(np.float32))
        return floats
    pylist: Any = native._to_pylist()
    if native.encoding == 2 and dtype in (DType.STRING, DType.LARGE_STRING):
        categorical: PolarsSeries = pl.Series(pylist, dtype=pl.Categorical)
        return categorical
    if dtype == DType.MAP:
        pylist = [
            None if row is None else [{"key": k, "value": v} for k, v in row] for row in pylist
        ]
    polars_dtype = _POLARS_DTYPES.get(dtype)
    result: PolarsSeries = pl.Series(
        pylist, dtype=getattr(pl, polars_dtype) if polars_dtype else None
    )
    return result


def _pandas_values(native: Series) -> pd.Series | npt.NDArray[Any]:
    """The column as a pandas Series or NumPy array built without pyarrow: a
    zoned timestamp gets its zone, everything else its NumPy array."""
    import pandas as pd

    if DType(native.type) in (DType.STRING, DType.LARGE_STRING) and _ARROW_ARRAY:
        import numpy as np

        string = pd.Series(np.array(["x"], dtype=object)).dtype
        if isinstance(string, pd.StringDtype) and string.storage == "pyarrow":
            try:
                import pyarrow as pa
            except ImportError:
                pass
            else:
                arr = pa.array(native)
                if pa.types.is_dictionary(arr.type):
                    arr = pa.DictionaryArray.from_arrays(
                        arr.indices, arr.dictionary.cast(pa.large_string())
                    )
                return pd.Series(pd.array(arr, dtype=string))
    values = _native_numpy(native)
    if DType(native.type) != DType.TIMESTAMP or not native.timezone:
        return values

    return pd.Series(values).dt.tz_localize("UTC").dt.tz_convert(native.timezone)


def _pandas_categorical(native: Series) -> pd.Categorical:
    """A dictionary-encoded string column as ``pd.Categorical``. With pyarrow the
    codes and the dictionary are taken as they are, so no string is built per
    row, and the categories keep the dictionary order without its unused
    entries. Without pyarrow the codes are not reachable, so the strings are
    built once and the categories come out sorted."""
    import numpy as np
    import pandas as pd

    pa = _pyarrow() if _ARROW_ARRAY else None
    if pa is not None:
        arr = pa.array(native)
        if pa.types.is_dictionary(arr.type):
            # pandas refuses repeated or null categories, which Arrow allows.
            if arr.dictionary.null_count or len(arr.dictionary.unique()) != len(arr.dictionary):
                arr = arr.dictionary_decode().dictionary_encode()
            codes = arr.indices.fill_null(-1).to_numpy(zero_copy_only=False)
            categories = arr.dictionary.to_pandas()
            out = pd.Categorical.from_codes(codes, categories=pd.Index(categories))
            return out.remove_unused_categories()
    values = np.empty(native.length, dtype=object)
    native._str_into(values, None)
    return pd.Categorical(values)


def _pandas_default(native: Series) -> pd.Series | npt.NDArray[Any] | ExtensionArray:
    """The default ``to_pandas`` column: ``pd.Categorical`` for a dictionary
    column, pandas' nullable dtype for an integer, bool or (where the default
    string dtype is ``object``) string column that holds nulls, and the NumPy
    form for the rest."""
    if native.encoding == 2 and DType(native.type) in (DType.STRING, DType.LARGE_STRING):
        return _pandas_categorical(native)
    if native.null_count:
        dtype = DType(native.type)
        if (
            dtype.is_integer
            or dtype == DType.BOOL
            or (dtype in (DType.STRING, DType.LARGE_STRING) and not _default_string_dtype())
        ):
            values = _pandas_nullable(native)
            if values is not None:
                return values
    return _pandas_values(native)


def _default_string_dtype() -> bool:
    """Whether pandas already stores strings in a ``StringDtype`` that holds
    missing values (pandas 3)."""
    import numpy as np
    import pandas as pd

    return isinstance(pd.Series(np.array(["x"], dtype=object)).dtype, pd.StringDtype)


def _pandas_nullable(native: Series) -> ExtensionArray | None:
    """The column as pandas' own nullable extension array (``Int8`` .. ``UInt64``,
    ``Float32``, ``Float64``, ``boolean``, ``string``), built without pyarrow; a
    null is ``pd.NA`` and a float NaN stays NaN. ``None`` for a type with no such
    dtype, which the caller converts as usual."""
    import numpy as np
    import pandas as pd

    dtype = DType(native.type)
    if native.encoding == 2 and dtype in (DType.STRING, DType.LARGE_STRING):
        return _pandas_categorical(native)
    if dtype in (DType.STRING, DType.LARGE_STRING):
        out = np.empty(native.length, dtype=object)
        native._str_into(out, pd.NA)
        string = pd.StringDtype()
        if getattr(string, "storage", "python") == "python":
            return cast(Any, string.construct_array_type())(out)
        return pd.array(out, dtype=string)
    if native.encoding != 0:
        native = native.materialize()
    if dtype.is_numeric:
        values, mask, dt = _parts(native, True)
        arr = _frombuffer(values, dt)
        m = _frombuffer(mask, "?") if mask is not None else np.zeros(len(arr), dtype=bool)
        return (
            pd.arrays.IntegerArray(arr, m) if dtype.is_integer else pd.arrays.FloatingArray(arr, m)
        )
    if dtype == DType.BOOL:
        values, mask, _ = _parts(native, True)
        arr = _frombuffer(values, "u1").view(np.bool_)
        m = _frombuffer(mask, "?") if mask is not None else np.zeros(len(arr), dtype=bool)
        return pd.arrays.BooleanArray(arr, m)
    return None


def _apply_index(
    pdf: pd.DataFrame, index: str | Sequence[str], restore_unnamed: bool = False
) -> pd.DataFrame:
    """``pdf`` with the named columns moved into the pandas index, in the order
    given (a MultiIndex for several names). KeyError names an unknown column and
    ValueError a repeated one. With ``restore_unnamed`` a level named
    ``__index_level_<i>__`` (what :func:`index_columns` gives an unnamed level)
    is unnamed again."""
    names = [index] if isinstance(index, str) else list(index)
    if not names:
        raise ValueError("to_pandas: index must name at least one column")
    seen: set[str] = set()
    for name in names:
        if name not in pdf.columns:
            raise KeyError(f"to_pandas: no column named {name!r} to use as the index")
        if name in seen:
            raise ValueError(f"to_pandas: index column {name!r} is repeated")
        seen.add(name)
    out = pdf.set_index(names if len(names) > 1 else names[0])
    if restore_unnamed:
        out.index.names = [
            None if re.fullmatch(r"__index_level_\d+__", n) else n for n in out.index.names
        ]
    return out


def _arrow_native(name: str) -> Callable[..., Any]:
    """The native Arrow entry point `name`."""
    return cast("Callable[..., Any]", getattr(_ext, name))


def _require_pyarrow() -> ModuleType:
    try:
        import pyarrow as pa
    except ImportError:
        raise ImportError(
            "pyarrow is required for Arrow conversion. Install with: pip install pyarrow"
        ) from None
    return cast("ModuleType", pa)


class NeedsArrow(TypeError):
    """Data that only pyarrow can import."""


def _retype(native: Series, dtype: DType, unit: str = "us", zone: str = "") -> Series:
    return _ext._series_retype(native, int(dtype), UNIT_CODES[unit], zone)


def _objects(values: Sequence[object], mask: npt.NDArray[Any] | None) -> Series:
    items = list(values)
    if mask is not None:
        items = [None if m else v for v, m in zip(items, mask.tolist())]
    if all(v is None for v in items):
        return _ext._series_from_list(items, int(DType.STRING))
    return _ext._series_from_list(items)


def _temporal(a: npt.NDArray[Any], mask: npt.NDArray[Any] | None, zone: str = "") -> Series:
    import numpy as np

    unit = np.datetime_data(a.dtype)[0]
    delta = a.dtype.kind == "m"
    if unit in ("Y", "M", "W") and delta:
        raise ValueError(f"timedelta64[{unit}] has no fixed length; convert it first")
    if unit in ("h", "m") or (delta and unit in ("D", "W")):
        unit = "s"
    elif unit in ("Y", "M", "W"):
        unit = "D"
    if unit not in ("D", "s", "ms", "us", "ns"):
        raise ValueError(f"cannot import {a.dtype}: its unit is finer than a nanosecond or generic")
    a = a.astype(f"{'timedelta64' if delta else 'datetime64'}[{unit}]")
    nat = np.isnat(a)
    if nat.any():
        mask = nat if mask is None else (mask | nat)
    if unit == "D" and not delta:
        days = np.where(np.isnat(a), 0, a.view("int64")).astype(np.int32)
        return _ext._series_retype(_ext._series_from_numpy(days, mask), int(DType.DATE32))
    phys = np.ascontiguousarray(np.where(np.isnat(a), 0, a.view("int64")))
    native = _ext._series_from_numpy(phys, mask)
    if delta:
        return _retype(native, DType.DURATION, unit)
    return _retype(native, DType.TIMESTAMP, unit, zone)


def numpy_to_native(a: npt.NDArray[Any], mask: npt.NDArray[Any] | None = None) -> Series:
    """A 1-D NumPy array (or masked array) as a native column; ``mask`` is true
    where null."""
    import numpy as np

    if isinstance(a, np.ma.MaskedArray):
        # np.ma.getmaskarray and np.ma.getdata have no annotations in older
        # numpy; the mask is nomask (a scalar False) when nothing is hidden.
        hidden = np.zeros(a.shape, dtype=np.bool_) | a.mask
        mask = hidden if mask is None else (mask | hidden)
        a = a.data
    a = np.asarray(a)
    if a.ndim != 1:
        raise TypeError("expected a 1-D array")
    kind = a.dtype.kind
    if kind in "biuf":
        if a.dtype.byteorder not in ("=", "|"):
            a = a.astype(a.dtype.newbyteorder("="))
        return _ext._series_from_numpy(np.ascontiguousarray(a), mask)
    if kind in "Mm":
        return _temporal(a, mask)
    if kind in "US":
        if len(a) == 0:
            return _ext._series_from_list([], int(DType.STRING if kind == "U" else DType.BINARY))
        return _objects(a.tolist(), mask)
    if kind == "O":
        return _objects(a.tolist(), mask)
    raise TypeError(f"cannot import a NumPy array of dtype {a.dtype}")


def _zone_name(tz: str | tzinfo | None) -> str:
    if tz is None:
        return ""
    for attr in ("key", "zone"):
        name = getattr(tz, attr, None)
        if isinstance(name, str):
            return name
    return str(tz)


def _dictionary(codes: npt.NDArray[Any], categories: list[str]) -> Series | None:
    """A dictionary column from integer codes (negative for null) and their
    strings, built through pyarrow so no string is made per row; None without
    pyarrow or an Arrow-enabled extension."""
    pa = _pyarrow()
    if not _ARROW_IMPORT or pa is None:
        return None
    indices = pa.array(codes, mask=codes < 0)
    return _ext._series_from_arrow(
        pa.DictionaryArray.from_arrays(indices, pa.array(categories, type=pa.string()))
    )


def _arrow_dictionary(s: pl.Series) -> Series | None:
    """A polars ``Categorical`` or ``Enum`` as a dictionary column from its
    Arrow export, which carries the codes and the values together; None
    without pyarrow or an Arrow-enabled extension."""
    if not _ARROW_IMPORT or _pyarrow() is None:
        return None
    return _ext._series_from_arrow(s.to_arrow())


def pandas_to_native(s: pd.Series) -> Series:
    """A ``pandas.Series`` as a native column: a NaN in a float column and a
    ``NaT`` or ``NA`` anywhere read as null. A ``category`` column whose
    categories are strings becomes a dictionary-encoded string column, in
    category order where pyarrow is installed; other categories keep their
    values."""
    import pandas as pd

    dtype = s.dtype
    if hasattr(pd, "ArrowDtype") and isinstance(dtype, pd.ArrowDtype):
        raise NeedsArrow(f"a pandas column of dtype {dtype} needs pyarrow")
    if isinstance(dtype, pd.CategoricalDtype):
        categories = dtype.categories.tolist()
        if all(isinstance(c, str) for c in categories):
            native = _dictionary(s.cat.codes.to_numpy(), categories)
            if native is not None:
                return native
            return _objects(
                s.astype(object).where(s.notna(), None).tolist(), None
            ).dictionary_encode()
        s = s.astype(object)
        dtype = s.dtype
    mask = s.isna().to_numpy()
    hidden = mask if mask.any() else None
    if isinstance(dtype, pd.DatetimeTZDtype):
        naive = s.dt.tz_convert("UTC").dt.tz_localize(None).to_numpy()
        base = _temporal(naive, hidden, _zone_name(dtype.tz))
        return base
    kind = getattr(dtype, "kind", "O")
    if kind in "biuf":
        if pd.api.types.is_extension_array_dtype(dtype):
            masked = cast("_MaskedDtype", dtype)
            values = s.to_numpy(dtype=masked.numpy_dtype, na_value=masked.numpy_dtype.type(0))
        else:
            values = s.to_numpy()
        return numpy_to_native(values, hidden)
    if kind in "Mm":
        return numpy_to_native(s.to_numpy(), hidden)
    if kind == "O" or isinstance(dtype, pd.StringDtype):
        return _objects(s.to_numpy(dtype=object).tolist(), hidden)
    raise TypeError(f"cannot import a pandas column of dtype {dtype}")


def polars_to_native(s: pl.Series) -> Series:
    """A ``polars.Series`` as a native column."""
    import polars as pl

    dtype = s.dtype
    name = str(dtype)
    mask = s.is_null().to_numpy() if s.null_count() else None
    if name.startswith(("Int", "UInt")):
        return numpy_to_native(s.fill_null(0).to_numpy(), mask)
    if name.startswith("Float"):
        return numpy_to_native(s.fill_null(0.0).to_numpy(), mask)
    if name == "Boolean":
        return numpy_to_native(s.fill_null(False).to_numpy(), mask)
    if name in ("String", "Utf8", "Binary", "Time") or name.startswith("Decimal"):
        return _objects(s.to_list(), None)
    if name in ("Categorical", "Enum") or name.startswith(("Categorical", "Enum")):
        native = _arrow_dictionary(s)
        if native is not None:
            return native
        return _objects(s.cast(pl.String).to_list(), None).dictionary_encode()
    if name == "Date":
        phys = s.to_physical().fill_null(0).cast(pl.Int32).to_numpy()
        return _ext._series_retype(_ext._series_from_numpy(phys, mask), int(DType.DATE32))
    if name.startswith("Datetime"):
        native = _ext._series_from_numpy(s.to_physical().fill_null(0).to_numpy(), mask)
        datetime = cast("pl.Datetime", dtype)
        return _retype(native, DType.TIMESTAMP, datetime.time_unit, datetime.time_zone or "")
    if name.startswith("Duration"):
        native = _ext._series_from_numpy(s.to_physical().fill_null(0).to_numpy(), mask)
        return _retype(native, DType.DURATION, cast("pl.Duration", dtype).time_unit)
    if name.startswith(("List", "Array", "Struct")):
        return _objects(s.to_list(), None)
    if name == "Null":
        return _ext._series_from_list([None] * len(s), int(DType.STRING))
    raise NeedsArrow(f"a polars column of dtype {dtype} needs pyarrow")


def index_columns(df: pd.DataFrame) -> list[tuple[str, pd.Series]]:
    """The columns a non-default pandas index adds, as ``(name, pandas.Series)``,
    named by the level or ``__index_level_<i>__``. The default ``RangeIndex``
    ``0 .. n-1`` with no name adds none; a range that has a name, starts
    elsewhere or steps is labels."""
    import pandas as pd

    index = df.index
    if (
        isinstance(index, pd.RangeIndex)
        and index.start == 0
        and index.step == 1
        and index.name is None
    ):
        return []
    return [
        (
            str(index.names[i]) if index.names[i] is not None else f"__index_level_{i}__",
            pd.Series(index.get_level_values(i)),
        )
        for i in range(index.nlevels)
    ]


def _indices(indices: Series | Sequence[int]) -> list[int]:
    if isinstance(indices, _ext.Series):
        raw: object = indices
        return list(map(int, cast("list[int]", cast("Series", raw).to_list())))
    return list(map(int, indices))


def _row_indices(indices: Series | Sequence[int]) -> Series | list[int]:
    """``indices`` for a native take: an Int64 Series without nulls goes in as
    it is (the native side reads its buffer), anything else as a list."""
    raw: object = indices
    if isinstance(raw, _ext.Series):
        column = cast("Series", raw)
        if column.dtype == DType.INT64 and column.null_count == 0:
            return column
    return _indices(indices)


def _like(ints: Series, template: Series) -> Series:
    """The int64 column ``ints`` read as the type of ``template``: a timestamp,
    duration, time or date keeps its unit and zone."""
    dtype = DType(template.type)
    if dtype in (DType.TIMESTAMP, DType.DURATION, DType.TIME64, DType.DATE64):
        return _ext._series_retype(ints, int(dtype), template.time_unit, template.timezone)
    if dtype in (DType.DATE32, DType.TIME32):
        return _ext._series_retype(ints.astype(DType.INT32), int(dtype), template.time_unit, "")
    return ints.astype(dtype)
