"""Imports of NumPy, pandas and polars data into native columns, with no pyarrow.

Each function returns a native ``_Series`` or raises ``TypeError`` for data it
cannot read; ``NeedsArrow`` marks data that only pyarrow can read (a pandas
Arrow-backed column, a polars list or struct column), so callers may fall back
to it when it is installed.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, List, Optional, Sequence, Tuple

from . import dftracer_utils_ext as _ext
from .enums import DType

if TYPE_CHECKING:
    import numpy as np

_UNIT_CODES = {"s": 0, "ms": 1, "us": 2, "ns": 3}


class NeedsArrow(TypeError):
    """Data that only pyarrow can import."""


def _retype(
    native: "_ext._Series", dtype: DType, unit: str = "us", zone: str = ""
) -> "_ext._Series":
    return _ext._series_retype(native, int(dtype), _UNIT_CODES[unit], zone)


def _objects(values: Sequence[object], mask: "Optional[np.ndarray]") -> "_ext._Series":
    items = list(values)
    if mask is not None:
        items = [None if m else v for v, m in zip(items, mask.tolist())]
    if all(v is None for v in items):
        return _ext._series_from_list(items, int(DType.STRING))
    return _ext._series_from_list(items)


def _temporal(a: "np.ndarray", mask: "Optional[np.ndarray]", zone: str = "") -> "_ext._Series":
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


def numpy_to_native(a: Any, mask: "Optional[np.ndarray]" = None) -> "_ext._Series":
    """A 1-D NumPy array (or masked array) as a native column; ``mask`` is true
    where null."""
    import numpy as np

    if isinstance(a, np.ma.MaskedArray):
        hidden = np.ma.getmaskarray(a)
        mask = hidden if mask is None else (mask | hidden)
        a = np.ma.getdata(a)
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


def _zone_name(tz: Any) -> str:
    if tz is None:
        return ""
    for attr in ("key", "zone"):
        name = getattr(tz, attr, None)
        if isinstance(name, str):
            return name
    return str(tz)


def pandas_to_native(s: Any) -> "_ext._Series":
    """A ``pandas.Series`` as a native column: a NaN in a float column and a
    ``NaT`` or ``NA`` anywhere read as null."""
    import pandas as pd

    dtype = s.dtype
    if hasattr(pd, "ArrowDtype") and isinstance(dtype, pd.ArrowDtype):
        raise NeedsArrow(f"a pandas column of dtype {dtype} needs pyarrow")
    if isinstance(dtype, pd.CategoricalDtype):
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
            values = s.to_numpy(dtype=dtype.numpy_dtype, na_value=dtype.numpy_dtype.type(0))
        else:
            values = s.to_numpy()
        return numpy_to_native(values, hidden)
    if kind in "Mm":
        return numpy_to_native(s.to_numpy(), hidden)
    if kind == "O" or isinstance(dtype, pd.StringDtype):
        return _objects(s.to_numpy(dtype=object).tolist(), hidden)
    raise TypeError(f"cannot import a pandas column of dtype {dtype}")


def polars_to_native(s: Any) -> "_ext._Series":
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
        return _objects(s.cast(pl.String).to_list(), None)
    if name == "Date":
        phys = s.to_physical().fill_null(0).cast(pl.Int32).to_numpy()
        return _ext._series_retype(_ext._series_from_numpy(phys, mask), int(DType.DATE32))
    if name.startswith("Datetime"):
        native = _ext._series_from_numpy(s.to_physical().fill_null(0).to_numpy(), mask)
        return _retype(native, DType.TIMESTAMP, dtype.time_unit, dtype.time_zone or "")
    if name.startswith("Duration"):
        native = _ext._series_from_numpy(s.to_physical().fill_null(0).to_numpy(), mask)
        return _retype(native, DType.DURATION, dtype.time_unit)
    if name.startswith(("List", "Array", "Struct")):
        return _objects(s.to_list(), None)
    if name == "Null":
        return _ext._series_from_list([None] * len(s), int(DType.STRING))
    raise NeedsArrow(f"a polars column of dtype {dtype} needs pyarrow")


def index_columns(df: Any) -> "List[Tuple[str, Any]]":
    """The columns a non-default pandas index adds, as ``(name, pandas.Series)``,
    named by the level or ``__index_level_<i>__``."""
    import pandas as pd

    index = df.index
    if isinstance(index, pd.RangeIndex):
        return []
    return [
        (
            str(index.names[i]) if index.names[i] is not None else f"__index_level_{i}__",
            pd.Series(index.get_level_values(i)),
        )
        for i in range(index.nlevels)
    ]
