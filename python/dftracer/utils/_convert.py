# SPDX-License-Identifier: MIT
"""Conversions between native columns and NumPy, pandas, polars and pyarrow.

Out of a column (``Series.to_numpy`` / ``to_pandas`` / ``to_polars`` /
``to_arrow``): the helpers that build the NumPy, pandas and polars forms, most
of them without pyarrow. Into a column (``Series.from_numpy`` / ``from_pandas`` /
``from_polars``): the importers, which return a native ``_Series`` or raise
``TypeError`` for data they cannot read; ``NeedsArrow`` marks a pandas
Arrow-backed column, which only its own Arrow export can read.
"""

from __future__ import annotations

import datetime
import functools
import re
from typing import TYPE_CHECKING, Any, Callable, Protocol, Sequence, cast

from . import dftracer_utils_ext as _ext
from ._enums import UNIT_CODES, UNIT_NS, DType, unit_letters

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


# ``Series.encoding`` codes
_DICTIONARY = 2
_CHUNKED = 5


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


def null_mask(native: Series) -> npt.NDArray[Any]:
    """A bool per row, True where the row is null, as pandas masks take it: the
    native mask bytes viewed as bools with no further copy."""
    import numpy as np

    mask = native._null_bytes()
    if mask is None:
        out: npt.NDArray[Any] = np.zeros(native.length, dtype=np.bool_)
        return out
    viewed: npt.NDArray[Any] = np.frombuffer(mask, dtype=np.bool_)
    return viewed


def flat(native: Series, keep_chunks: bool = False) -> Series:
    """``native`` as one flat column: a dictionary, selection, view or constant
    column is decoded natively and a chunked one joined (a copy each). A flat
    column is returned as it is, and so is a chunked one with ``keep_chunks``,
    for a caller that takes the chunks as they are."""
    if native.encoding == 0 or (keep_chunks and native.encoding == _CHUNKED):
        return native
    return native.materialize()


def texts(native: Series, na: NAType | None = None) -> npt.NDArray[Any]:
    """The strings of a flat or dictionary column as an object array, ``na`` (None
    by default) for null."""
    import numpy as np

    out: npt.NDArray[Any] = np.empty(native.length, dtype=object)
    native._str_into(out, na)
    return out


def missing(value: object) -> bool:
    """Whether ``value`` stands for a null: ``None``, ``pd.NA``, ``NaT`` or a
    NaN, with no pandas import."""
    if is_na(value) or type(value).__name__ == "NaTType":
        return True
    if isinstance(value, float):
        return value != value
    letter = getattr(getattr(value, "dtype", None), "kind", None)
    if (
        type(value).__module__ == "numpy"
        and getattr(value, "ndim", 0) == 0
        and letter in ("M", "m", "f")
    ):
        return bool(value != value)
    return False


def is_na(value: object) -> bool:
    """Whether ``value`` is ``None`` or ``pd.NA``, with no pandas import; a NaN is
    not."""
    return value is None or type(value).__name__ == "NAType"


def timedelta_nanos(value: datetime.timedelta) -> int:
    """Whole nanoseconds of a time span; a pandas ``Timedelta`` keeps its own."""
    # The components, not a pandas Timedelta's ``value``, which overflows for a
    # span past int64 nanoseconds.
    whole = (value.days * 86_400 + value.seconds) * 1_000_000_000 + value.microseconds * 1_000
    return whole + int(getattr(value, "nanoseconds", 0))


def clean_dictionary(native: Series) -> Series:
    """``native`` dictionary-encoded over distinct, non-null values: a dictionary
    that repeats a value or holds a null one (Arrow allows both) is encoded
    again (a copy). A chunked column of dictionary chunks is joined first."""
    native = _one_dictionary(native)
    if native.encoding == _DICTIONARY:
        _, values = native._dictionary_parts()
        if not values.null_count and values.n_unique() == len(values):
            return native
        native = native.materialize()
    return native.dictionary_encode()


def arrow_pandas(native: Series) -> pd.Series:
    """The column as a ``pd.ArrowDtype`` Series over its own buffers, a chunk per
    native chunk, through the Arrow C stream; a view string is made flat (a
    copy), as pandas and the pyarrow kernels do not take one."""
    import pandas as pd

    chunks = _require_pyarrow().chunked_array(native._plain_text())
    return pd.Series(pd.arrays.ArrowExtensionArray(chunks), copy=False)


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

    unit = _TEMPORAL_KINDS[dtype] or unit_letters(native.time_unit)
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
    if DType(native.type).is_text:
        return texts(native)
    native = flat(native)
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
    native = _one_dictionary(native)
    pylist: Any = native._to_pylist()
    if native.encoding == _DICTIONARY and dtype.is_text:
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

    if DType(native.type).is_text and _ARROW_ARRAY:
        import numpy as np

        string = pd.Series(np.array(["x"], dtype=object)).dtype
        pa = _pyarrow()
        if isinstance(string, pd.StringDtype) and string.storage == "pyarrow" and pa is not None:
            # pandas holds this dtype in pyarrow: hand it our buffers, a chunk
            # per native chunk. A view is made flat natively first (a copy), and
            # a dictionary or selection is gathered (a copy), as this dtype has
            # neither.
            chunks = pa.chunked_array(flat(native, keep_chunks=True)._plain_text())
            return pd.Series(pd.array(chunks, dtype=string), copy=False)
    values = _native_numpy(native)
    if DType(native.type) != DType.TIMESTAMP or not native.timezone:
        return values

    return pd.Series(values, copy=False).dt.tz_localize("UTC").dt.tz_convert(native.timezone)


def _pandas_categorical(native: Series) -> pd.Categorical:
    """A dictionary-encoded string column as ``pd.Categorical``: the codes and
    the dictionary taken as they are, so one string is built per category, not
    per row, and the categories keep the dictionary order without its unused
    entries."""
    import numpy as np
    import pandas as pd

    # pandas refuses repeated or null categories, which Arrow allows.
    codes, values = clean_dictionary(native)._dictionary_parts()
    ints = np.asarray(codes._values_buffer())
    if codes.null_count:
        # a copy: pandas marks a null row with code -1
        ints = np.where(null_mask(codes), -1, ints)
    out = pd.Categorical.from_codes(ints, categories=pd.Index(texts(values)))
    return out.remove_unused_categories()


def _one_dictionary(native: Series) -> Series:
    """A chunked column of dictionary chunks joined into one column (its codes
    copied, as pandas holds one codes array per category column), a dictionary
    where the values allow one, so the dtype does not depend on how the rows
    were batched; any other column as it is."""
    joined = native._joined_dictionary()
    return native if joined is None else joined


def _pandas_default(native: Series) -> pd.Series | npt.NDArray[Any] | ExtensionArray:
    """The default ``to_pandas`` column: ``pd.Categorical`` for a dictionary
    column, pandas' nullable dtype for an integer, bool or (where the default
    string dtype is ``object``) string column that holds nulls, and the NumPy
    form for the rest."""
    native = _one_dictionary(native)
    if native.encoding == _DICTIONARY and DType(native.type).is_text:
        return _pandas_categorical(native)
    if native.null_count:
        dtype = DType(native.type)
        if (
            dtype.is_integer
            or dtype == DType.BOOL
            or (dtype.is_text and not _default_string_dtype())
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

    native = _one_dictionary(native)
    dtype = DType(native.type)
    if native.encoding == _DICTIONARY and dtype.is_text:
        return _pandas_categorical(native)
    if dtype.is_text:
        out = texts(native, pd.NA)
        string = pd.StringDtype()
        if getattr(string, "storage", "python") == "python":
            return cast(Any, string.construct_array_type())(out)
        return pd.array(out, dtype=string)
    native = flat(native)
    if dtype.is_numeric:
        arr = np.asarray(native._values_buffer())
        m = null_mask(native)
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


_EPOCH_UTC = datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc)
_EPOCH = datetime.datetime(1970, 1, 1)


_NUMPY_UNIT_NS = {
    "W": 604_800_000_000_000,
    "D": 86_400_000_000_000,
    "h": 3_600_000_000_000,
    "m": 60_000_000_000,
    "s": 1_000_000_000,
    "ms": 1_000_000,
    "us": 1_000,
    "ns": 1,
}


def _nanos(value: object) -> int:
    """Nanoseconds of a datetime (from the epoch, UTC when aware), date or
    timedelta, as an exact Python integer that no unit bounds; a pandas
    ``Timestamp`` or ``Timedelta`` is read as its NumPy value in its own unit,
    as its ``value`` overflows past int64 nanoseconds."""
    if type(value).__module__.startswith("pandas"):
        # NumPy keeps the value's own unit; datetime arithmetic would not.
        to_numpy = getattr(value, "to_datetime64", None) or getattr(value, "to_timedelta64", None)
        if to_numpy is not None:
            value = to_numpy()
    letter = getattr(getattr(value, "dtype", None), "kind", None)
    if type(value).__module__ == "numpy" and letter in ("M", "m"):
        import numpy as np

        span: Any = value
        name, count = np.datetime_data(span.dtype)
        if name in ("Y", "M"):
            if letter == "m":
                raise ValueError(f"timedelta64[{name}] has no fixed length; convert it first")
            span, name, count = span.astype("datetime64[D]"), "D", 1
        per = _NUMPY_UNIT_NS.get(name)
        if per is None:
            # finer than a nanosecond
            kind = "datetime64" if letter == "M" else "timedelta64"
            return int(span.astype(f"{kind}[ns]").astype(np.int64))
        return int(span.astype(np.int64)) * int(count) * per
    if isinstance(value, datetime.datetime):
        return timedelta_nanos(value - (_EPOCH if value.tzinfo is None else _EPOCH_UTC))
    if isinstance(value, datetime.date):
        return timedelta_nanos(datetime.datetime(value.year, value.month, value.day) - _EPOCH)
    if isinstance(value, datetime.timedelta):
        return timedelta_nanos(value)
    raise TypeError(f"{type(value).__name__} is not a date, time or time span")


def arrow_native_type(t: pa.DataType) -> tuple[DType, str, str]:
    """The native type, time unit and time zone of a pyarrow type. TypeError for
    a type with no native column that Python values build."""
    import pyarrow as pa

    if pa.types.is_integer(t):
        sign = "INT" if pa.types.is_signed_integer(t) else "UINT"
        return DType[f"{sign}{t.bit_width}"], "", ""
    if pa.types.is_floating(t) and t.bit_width in (32, 64):
        return (DType.FLOAT32 if t.bit_width == 32 else DType.FLOAT64), "", ""
    if pa.types.is_timestamp(t):
        return DType.TIMESTAMP, t.unit, t.tz or ""
    if pa.types.is_duration(t):
        return DType.DURATION, t.unit, ""
    simple = {
        pa.bool_(): DType.BOOL,
        pa.string(): DType.STRING,
        pa.large_string(): DType.LARGE_STRING,
        pa.binary(): DType.BINARY,
        pa.large_binary(): DType.LARGE_BINARY,
        pa.date32(): DType.DATE32,
    }
    if t in simple:
        return simple[t], "", ""
    raise TypeError(f"no native column type for the Arrow type {t}")


def _tick_column(values: Sequence[object], np_type: type, convert: Callable[[Any], int]) -> Series:
    """A native integer column of ``convert(value)`` for each value, null where
    the value is ``None``, ``NA``, ``NaT`` or NaN."""
    import numpy as np

    ticks: npt.NDArray[Any] = np.zeros(len(values), dtype=np_type)
    nulls = np.zeros(len(values), dtype=np.bool_)
    for i, value in enumerate(values):
        if missing(value):
            nulls[i] = True
        else:
            ticks[i] = convert(value)
    return _ext._series_from_numpy(ticks, nulls if nulls.any() else None)


def temporal_from_values(
    values: Sequence[object], dtype: DType, unit: str, zone: str = ""
) -> Series:
    """``values`` as a native timestamp or duration column counted in ``unit``,
    with no pyarrow; ``None`` and ``NaT`` are null. ``ValueError`` for a value
    that is not a whole tick of ``unit``."""
    import numpy as np

    tick = UNIT_NS[unit]

    def convert(value: object) -> int:
        if isinstance(value, int) and not isinstance(value, bool):
            # a raw tick count of the column's unit
            return value
        if _is_span(value) != (dtype == DType.DURATION):
            want = "a time span" if dtype == DType.DURATION else "a point in time"
            raise TypeError(f"{value!r} is not {want}")
        whole, rest = divmod(_nanos(value), tick)
        if rest:
            raise ValueError(f"{value!r} is not a whole number of {unit}")
        return whole

    return _retype(_tick_column(values, np.int64, convert), dtype, unit, zone)


def _is_span(value: object) -> bool:
    """Whether ``value`` is a time span (``timedelta``, NumPy ``timedelta64``,
    pandas ``Timedelta``) and not a point in time."""
    if isinstance(value, datetime.timedelta):
        return True
    return getattr(getattr(value, "dtype", None), "kind", None) == "m"


def _list_like(values: Sequence[object], like: Series) -> Series:
    """``values`` (each a sequence or null) as a List or LargeList column whose
    items take the type of ``like``'s items, built natively from the flattened
    items and the row ends."""
    import numpy as np

    items: list[object] = []
    ends: list[int] = []
    valid = np.ones(len(values), dtype=np.bool_)
    for i, row in enumerate(values):
        if missing(row):
            valid[i] = False
        elif isinstance(row, (str, bytes, bytearray)) or not hasattr(row, "__len__"):
            raise TypeError(f"{row!r} is not a list")
        else:
            items.extend(cast("Sequence[object]", row))
        ends.append(len(items))
    try:
        child = values_like(items, like.child(0))
    except OverflowError as exc:
        raise ValueError(str(exc)) from None
    ends_column = cast("Series[int]", _ext._series_from_numpy(np.array(ends, dtype=np.int64), None))
    out = _ext._series_list_of(child, ends_column)
    if not valid.all():
        out = out.where(_ext._series_from_numpy(valid, None), out._nulls_like(len(values)))
    return out._cast(int(like.type)) if DType(like.type) == DType.LARGE_LIST else out


_BINARY_WIDTH_TYPES = {1: "u1", 2: "u2", 4: "u4", 8: "u8"}


def _fixed_binary_like(values: Sequence[object], like: Series) -> Series:
    """``values`` (each ``bytes`` of the column's width, or null) as a
    FixedSizeBinary column of ``like``'s width. The native column reads the same
    buffer as an integer of that width, so only widths of 1, 2, 4 and 8 bytes
    build."""
    import numpy as np

    width = like._fixed_size
    dtype = _BINARY_WIDTH_TYPES.get(width)
    if dtype is None:
        raise TypeError(f"no native build of a fixed_size_binary column of {width} bytes")
    data = bytearray(width * len(values))
    nulls = np.zeros(len(values), dtype=np.bool_)
    for i, value in enumerate(values):
        if missing(value):
            nulls[i] = True
            continue
        if not isinstance(value, (bytes, bytearray)):
            raise TypeError(f"{value!r} is not bytes")
        if len(value) != width:
            raise ValueError(f"{value!r} is not {width} bytes")
        data[i * width : (i + 1) * width] = value
    ints = np.frombuffer(data, dtype=dtype)
    native = _ext._series_from_numpy(ints, nulls if nulls.any() else None)
    return _ext._series_retype(native, int(DType.FIXED_SIZE_BINARY), 0, "", 0, 0, width)


def values_like(values: Sequence[object], like: Series) -> Series:
    """``values`` as a column of ``like``'s full type: its time unit and zone,
    its decimal precision and scale, the item type of a list and the width of a
    fixed-size binary, date64 and time types included. A value that the type
    cannot hold exactly raises ``ValueError``; ``None``, ``pd.NA`` and ``NaT`` are
    null. ``TypeError`` for a struct, map or fixed-size list, which no native
    build gives the type of ``like``. A type with no parameters builds as
    ``Series.from_list(values, dtype)`` does."""
    kind = DType(like.type)
    if kind in (DType.TIMESTAMP, DType.DURATION):
        return temporal_from_values(values, kind, unit_letters(like.time_unit), like.timezone)
    if kind in (DType.DATE64, DType.TIME32, DType.TIME64):
        import numpy as np

        tick = 86_400_000 if kind == DType.DATE64 else UNIT_NS[unit_letters(like.time_unit)]

        def convert(value: object) -> int:
            if kind == DType.DATE64:
                if not isinstance(value, datetime.date) or isinstance(value, datetime.datetime):
                    raise TypeError(f"{value!r} is not a date")
                return (value - datetime.date(1970, 1, 1)).days * tick
            if not isinstance(value, datetime.time):
                raise TypeError(f"{value!r} is not a time of day")
            nanos = ((value.hour * 60 + value.minute) * 60 + value.second) * 1_000_000_000 + (
                value.microsecond * 1_000
            )
            whole, rest = divmod(nanos, tick)
            if rest:
                raise ValueError(f"{value!r} is not a whole number of the column's unit")
            return whole

        native = _tick_column(values, np.int32 if kind == DType.TIME32 else np.int64, convert)
        return _ext._series_retype(native, int(kind), like.time_unit)
    if kind in (DType.LIST, DType.LARGE_LIST):
        return _list_like(values, like)
    if kind == DType.FIXED_SIZE_BINARY:
        return _fixed_binary_like(values, like)
    if kind in (DType.STRUCT, DType.MAP, DType.FIXED_SIZE_LIST):
        raise TypeError(f"no native build of a {kind.name.lower()} column of the labels' type")
    if kind == DType.DECIMAL256:
        raise TypeError("no native build of a decimal256 column from Python values")
    if kind == DType.DECIMAL128:
        import decimal

        precision, scale = like._decimal
        # Wide enough for 76 digits; the default context rounds past 28.
        context = decimal.Context(prec=100)
        step = decimal.Decimal(1).scaleb(-scale)
        exact: list[object] = []
        for value in values:
            if missing(value):
                exact.append(None)
                continue
            if not isinstance(value, (decimal.Decimal, int)) or isinstance(value, bool):
                raise TypeError(f"{value!r} is not a decimal")
            fixed = decimal.Decimal(value).quantize(step, context=context)
            if fixed != value:
                raise ValueError(f"{value!r} does not fit scale {scale}")
            if len(fixed.as_tuple().digits) > precision:
                raise ValueError(f"{value!r} does not fit precision {precision}")
            # The builder never stores a negative scale, so a value of one is
            # passed as its unscaled integer and the retype sets the scale.
            exact.append(fixed.scaleb(scale, context=context) if scale < 0 else fixed)
        if all(v is None for v in exact):
            return like._nulls_like(len(exact))
        built = _ext._series_from_list(exact)
        return _ext._series_retype(built, int(kind), 0, "", precision, scale)
    from ._series import Series as _Series

    return _Series.from_list(list(values), kind)


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


def _dictionary(codes: npt.NDArray[Any], categories: list[str]) -> Series:
    """A dictionary column from integer codes (negative for null) and their
    strings, so one string is made per category, not per row. The codes are
    copied (pandas writes ``-1`` into its own codes in place on a later
    ``s[i] = None``)."""
    import numpy as np

    ints = cast("Series[int]", _ext._series_from_numpy(np.array(codes, dtype=np.int32), None))
    return _ext._series_from_dictionary(ints, _objects(categories, None))


def _pyarrow_strings(s: pd.Series) -> Series:
    """A pandas ``str`` column that pyarrow holds (the pandas 3 default), with
    its text buffer shared, not one Python string per cell. Its 64-bit offsets
    become 32-bit ones, the string type the kernels take, when the text fits;
    only the offsets are rewritten."""
    import pyarrow as pa

    native = _ext._series_from_arrow(pa.Array.from_pandas(s))
    if DType(native.type) != DType.LARGE_STRING:
        return native
    try:
        return native._cast(int(DType.STRING))
    except TypeError:
        return native


def pandas_to_native(s: pd.Series) -> Series:
    """A ``pandas.Series`` as a native column: a NaN in a float column and a
    ``NaT`` or ``NA`` anywhere read as null. A ``category`` column whose
    categories are strings becomes a dictionary-encoded string column, in
    category order; other categories keep their values."""
    import numpy as np
    import pandas as pd

    dtype = s.dtype
    if hasattr(pd, "ArrowDtype") and isinstance(dtype, pd.ArrowDtype):
        raise NeedsArrow(f"a pandas column of dtype {dtype} needs pyarrow")
    if isinstance(dtype, pd.StringDtype) and dtype.storage == "pyarrow":
        return _pyarrow_strings(s)
    if isinstance(dtype, pd.CategoricalDtype):
        categories = dtype.categories.tolist()
        if all(isinstance(c, str) for c in categories):
            # the codes as pandas holds them, not the copy s.cat.codes makes
            return _dictionary(cast("pd.Categorical", s.array).codes, categories)
        s = s.astype(object)
        dtype = s.dtype
    if isinstance(dtype, pd.StringDtype) or getattr(dtype, "kind", None) == "O":
        # None where null in one pass, so no mask is merged in a Python loop
        return _objects(s.to_numpy(dtype=object, na_value=None).tolist(), None)
    mask = s.isna().to_numpy()
    hidden = mask if mask.any() else None
    if isinstance(dtype, pd.DatetimeTZDtype):
        naive = s.dt.tz_convert("UTC").dt.tz_localize(None).to_numpy()
        base = _temporal(naive, hidden, _zone_name(dtype.tz))
        return base
    kind = getattr(dtype, "kind", "O")
    if kind in "biuf":
        if pd.api.types.is_extension_array_dtype(dtype):
            # A masked array's own values are borrowed; the value under a null
            # is never read, as for a numpy masked array.
            data = getattr(s.array, "_data", None)
            masked = cast("_MaskedDtype", dtype)
            if isinstance(data, np.ndarray) and data.dtype == masked.numpy_dtype:
                values = data
            else:
                values = s.to_numpy(dtype=masked.numpy_dtype, na_value=masked.numpy_dtype.type(0))
        else:
            values = s.to_numpy()
        return numpy_to_native(values, hidden)
    if kind in "Mm":
        return numpy_to_native(s.to_numpy(), hidden)
    raise TypeError(f"cannot import a pandas column of dtype {dtype}")


def polars_to_native(s: pl.Series) -> Series:
    """A ``polars.Series`` as a native column: through the Arrow C stream that
    polars exports (polars >= 1.3), sharing its buffers, else natively type by
    type."""
    import polars as pl

    if _ARROW_IMPORT and hasattr(s, "__arrow_c_stream__"):
        return _ext._series_from_arrow(s)
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
    raise TypeError(f"cannot import a polars column of dtype {dtype}")


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
