# SPDX-License-Identifier: MIT
"""The public ``Series``: the native columnar type, subclassed.

The C extension's ``Series`` carries the SIMD ops and the Arrow C Data Interface
capsule, and its factories build the class bound here (``_bind_types``), so a
native op returns a ``Series`` with no wrapper object in between. What needs
Python lives in this class: the constructor from Python data, ``to_arrow`` /
``to_pandas`` / ``to_numpy`` / ``to_polars``, NumPy-style ``+ - * /`` and
``s[i]`` indexing, the ``np.asarray`` protocol, pickling and the pandas and
polars spellings. ``dataframe.DataFrame`` is the frame counterpart.
"""

from __future__ import annotations

import builtins
import json
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    List,
    Literal,
    Mapping,
    Optional,
    Sequence,
    Tuple,
    TypeVar,
    Union,
    cast,
    overload,
)

from . import dftracer_utils_ext as _ext
from ._pandas_series import _SeriesPandasMixin
from .enums import _DTYPE_BY_NAME, DType

if TYPE_CHECKING:
    from types import ModuleType

    import numpy as np  # ty: ignore[import-not-found]
    import pandas as pd  # ty: ignore[import-not-found]
    import polars as pl  # ty: ignore[import-not-found]
    import pyarrow as pa  # ty: ignore[import-not-found]
    from pandas._libs.missing import NAType  # ty: ignore[import-not-found]
    from pandas.api.extensions import ExtensionArray  # ty: ignore[import-not-found]
    from typing_extensions import Self

    from .dataframe import DataFrame
    from .dftracer_utils_ext import Scalar, T_co
else:
    # The checkers read T_co (covariant, defaulting to Scalar) from the stub.
    T_co = TypeVar("T_co", covariant=True)

# Python types and the NumPy names that differ from a DType member name.
_PY_DTYPES: Dict[type, DType] = {
    str: DType.STRING,
    int: DType.INT64,
    float: DType.FLOAT64,
    bool: DType.BOOL,
}
_NUMPY_NAMES = {"str": "string", "str_": "string", "bool_": "bool"}


_INT_BITS = {DType.INT8: 8, DType.INT16: 16, DType.INT32: 32, DType.INT64: 64}
_UINT_BITS = {DType.UINT8: 8, DType.UINT16: 16, DType.UINT32: 32, DType.UINT64: 64}


def _dtype_limits(dtype: DType) -> "Optional[Tuple[Union[int, float], Union[int, float]]]":
    """The lowest and highest value a numeric column of ``dtype`` can hold (the
    missing bound of a one-sided ``clip``); ``None`` for a type with no limits
    here."""
    if dtype in _INT_BITS:
        bits = _INT_BITS[dtype]
        return -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    if dtype in _UINT_BITS:
        return 0, (1 << _UINT_BITS[dtype]) - 1
    if dtype in (DType.FLOAT32, DType.FLOAT64):
        return float("-inf"), float("inf")
    return None


def _resolve_dtype(dtype: "Union[str, int, DType, type]") -> int:
    """A dtype spelled as a name, a DType member, a raw int code, a Python type
    (``str``, ``int``, ``float``, ``bool``) or a NumPy scalar type or dtype that
    names a supported type -> int code. Raises ValueError on an unrecognized
    name and TypeError on any other argument."""
    if isinstance(dtype, str):
        try:
            return int(_DTYPE_BY_NAME[dtype.lower()])
        except KeyError:
            raise ValueError(f"unknown dtype name: {dtype!r}") from None
    if isinstance(dtype, type) or hasattr(dtype, "kind"):
        py_dtype = _PY_DTYPES.get(cast(type, dtype))
        if py_dtype is not None:
            return int(py_dtype)
        name = getattr(dtype, "name", None)
        if not isinstance(name, str):
            name = getattr(dtype, "__name__", "")
        name = _NUMPY_NAMES.get(name, name)
        try:
            return int(_DTYPE_BY_NAME[name.lower()])
        except KeyError:
            pass
    else:
        try:
            return int(dtype)
        except (TypeError, ValueError):
            pass
    raise TypeError(
        "astype: dtype must be a DType, an int code, a dtype name, a Python type "
        f"(str, int, float, bool) or a NumPy type that names a supported dtype; got {dtype!r}"
    )


_R = TypeVar("_R", bound="Scalar")


def _to_pandas(arrow_obj: Any, arrow: bool) -> Any:
    """``pyarrow`` to pandas: NumPy dtypes (a copy) or, with ``arrow``, the
    ``pd.ArrowDtype`` columns that share the Arrow buffers."""
    if not arrow:
        return arrow_obj.to_pandas()
    import pandas as pd  # ty: ignore[import-not-found]

    return arrow_obj.to_pandas(types_mapper=pd.ArrowDtype)


_POLARS_DTYPES: Dict[DType, str] = {
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
_FLOATS = (DType.FLOAT32, DType.FLOAT64)
_INTEGERS = (
    DType.INT8,
    DType.INT16,
    DType.INT32,
    DType.INT64,
    DType.UINT8,
    DType.UINT16,
    DType.UINT32,
    DType.UINT64,
)
_NATIVE_OBJECT_TYPES = (
    DType.STRING,
    DType.LARGE_STRING,
    DType.BINARY,
    DType.LARGE_BINARY,
    DType.LIST,
    DType.LARGE_LIST,
    DType.STRUCT,
)


_NUMPY_UNITS = ("s", "ms", "us", "ns")
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


def _temporal_numpy(native: "Series", dtype: DType) -> "np.ndarray":
    """A date, timestamp or duration column as ``datetime64`` or ``timedelta64``
    (UTC values for a zoned timestamp), with ``NaT`` for null."""
    import numpy as np  # ty: ignore[import-not-found]

    unit = _TEMPORAL_KINDS[dtype] or _NUMPY_UNITS[native.time_unit]
    phys = native._physical()
    if dtype == DType.DATE32:
        phys = phys.cast(int(DType.INT64))
    if phys.null_count:
        phys = phys.fillna(_NAT)
    kind = "timedelta64" if dtype == DType.DURATION else "datetime64"
    return np.array(memoryview(phys)).view(f"{kind}[{unit}]")  # ty: ignore[invalid-argument-type]


def _object_numpy(values: Sequence[object]) -> "np.ndarray":
    import numpy as np  # ty: ignore[import-not-found]

    out = np.empty(len(values), dtype=object)
    for i, v in enumerate(values):
        out[i] = v
    return out


def _parts(
    native: "Series", nullable: bool, na: "Optional[NAType]" = None
) -> Tuple[bytearray, Optional[bytearray], str]:
    """The column's values, null mask and numpy dtype string from one native
    pass (``Series._np_parts``): bytearrays that numpy wraps without a copy."""
    return native._np_parts(nullable, na)


def _frombuffer(data: Union[bytes, bytearray, memoryview], dtype: str) -> "np.ndarray":
    import numpy as np  # ty: ignore[import-not-found]

    return np.frombuffer(data, dtype=dtype)


def _native_numpy(native: "Series") -> "np.ndarray":
    """The native column as a NumPy array without pyarrow; TypeError for a
    type with no native conversion."""
    import numpy as np  # ty: ignore[import-not-found]

    try:
        # memoryview forces the native buffer protocol (a plain
        # np.asarray(handle) can fall back to a 0-d object array).
        # The native buffer support is not in the stub (PEP 688 is 3.12+).
        return np.asarray(memoryview(native))  # ty: ignore[invalid-argument-type]
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
    if dtype in _FLOATS or dtype in _INTEGERS:
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


def _polars_via_arrow(native: "Series") -> bool:
    """Whether the Arrow capsule gives polars the dtype the native path does: not
    for Float16 and decimals."""
    return hasattr(native, "__arrow_c_array__") and DType(native.type) not in _POLARS_NATIVE_ONLY


def _polars_native(native: "Series", pl: Any) -> "pl.Series":
    """The column as a polars Series built natively, with no pyarrow."""
    dtype = DType(native.type)
    if dtype in _TEMPORAL_KINDS:
        values = _temporal_numpy(native, dtype)
        if values.dtype.name.endswith("[s]"):
            values = values.astype(values.dtype.name.replace("[s]", "[ms]"))
        out = pl.Series(values)
        if dtype == DType.TIMESTAMP and native.timezone:
            out = out.dt.replace_time_zone("UTC").dt.convert_time_zone(native.timezone)
        return out
    if dtype == DType.FLOAT16:
        import numpy as np  # ty: ignore[import-not-found]

        return pl.Series(_native_numpy(native).astype(np.float32))  # type: ignore[union-attr]
    values: Any = native._to_pylist()
    if native.encoding == 2 and dtype in (DType.STRING, DType.LARGE_STRING):
        return pl.Series(values, dtype=pl.Categorical)
    if dtype == DType.MAP:
        values = [
            None if row is None else [{"key": k, "value": v} for k, v in row] for row in values
        ]
    polars_dtype = _POLARS_DTYPES.get(dtype)
    return pl.Series(values, dtype=getattr(pl, polars_dtype) if polars_dtype else None)


def _pandas_values(native: "Series") -> "Union[pd.Series, np.ndarray]":
    """The column as a pandas Series or NumPy array built without pyarrow: a
    zoned timestamp gets its zone, everything else its NumPy array."""
    import pandas as pd  # ty: ignore[import-not-found]

    if DType(native.type) in (DType.STRING, DType.LARGE_STRING) and hasattr(
        native, "__arrow_c_array__"
    ):
        import numpy as np  # ty: ignore[import-not-found]

        string = pd.Series(np.array(["x"], dtype=object)).dtype
        if isinstance(string, pd.StringDtype) and string.storage == "pyarrow":
            try:
                import pyarrow as pa  # ty: ignore[import-not-found]
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


def _pandas_categorical(native: "Series") -> "pd.Categorical":
    """A dictionary-encoded string column as ``pd.Categorical``. With pyarrow the
    codes and the dictionary are taken as they are, so no string is built per
    row, and the categories keep the dictionary order without its unused
    entries. Without pyarrow the codes are not reachable, so the strings are
    built once and the categories come out sorted."""
    import numpy as np  # ty: ignore[import-not-found]
    import pandas as pd  # ty: ignore[import-not-found]

    if hasattr(native, "__arrow_c_array__"):
        try:
            import pyarrow as pa  # ty: ignore[import-not-found]
        except ImportError:
            pass
        else:
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


def _pandas_default(native: "Series") -> "Union[pd.Series, np.ndarray, ExtensionArray]":
    """The default ``to_pandas`` column: ``pd.Categorical`` for a dictionary
    column, pandas' nullable dtype for an integer, bool or (where the default
    string dtype is ``object``) string column that holds nulls, and the NumPy
    form for the rest."""
    if native.encoding == 2 and DType(native.type) in (DType.STRING, DType.LARGE_STRING):
        return _pandas_categorical(native)
    if native.null_count:
        dtype = DType(native.type)
        if (
            dtype in _INTEGERS
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
    import numpy as np  # ty: ignore[import-not-found]
    import pandas as pd  # ty: ignore[import-not-found]

    return isinstance(pd.Series(np.array(["x"], dtype=object)).dtype, pd.StringDtype)


def _pandas_nullable(native: "Series") -> "Optional[ExtensionArray]":
    """The column as pandas' own nullable extension array (``Int8`` .. ``UInt64``,
    ``Float32``, ``Float64``, ``boolean``, ``string``), built without pyarrow; a
    null is ``pd.NA`` and a float NaN stays NaN. ``None`` for a type with no such
    dtype, which the caller converts as usual."""
    import numpy as np  # ty: ignore[import-not-found]
    import pandas as pd  # ty: ignore[import-not-found]

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
    if dtype in _INTEGERS or dtype in _FLOATS:
        values, mask, dt = _parts(native, True)
        arr = _frombuffer(values, dt)
        m = _frombuffer(mask, "?") if mask is not None else np.zeros(len(arr), dtype=bool)
        return (
            pd.arrays.IntegerArray(arr, m)
            if dtype in _INTEGERS
            else pd.arrays.FloatingArray(arr, m)
        )
    if dtype == DType.BOOL:
        values, mask, _ = _parts(native, True)
        arr = _frombuffer(values, "u1").view(np.bool_)
        m = _frombuffer(mask, "?") if mask is not None else np.zeros(len(arr), dtype=bool)
        return pd.arrays.BooleanArray(arr, m)
    return None


def _apply_index(pdf: "pd.DataFrame", index: "Union[str, Sequence[str]]") -> "pd.DataFrame":
    """``pdf`` with the named columns moved into the pandas index, in the order
    given (a MultiIndex for several names). KeyError names an unknown column and
    ValueError a repeated one."""
    names = [index] if isinstance(index, str) else list(index)
    if not names:
        raise ValueError("to_pandas: index must name at least one column")
    seen: "set[str]" = set()
    for name in names:
        if name not in pdf.columns:
            raise KeyError(f"to_pandas: no column named {name!r} to use as the index")
        if name in seen:
            raise ValueError(f"to_pandas: index column {name!r} is repeated")
        seen.add(name)
    return pdf.set_index(names if len(names) > 1 else names[0])


def _arrow_native(name: str) -> Callable[..., Any]:
    """The native Arrow entry point `name`."""
    return getattr(_ext, name)


def _require_pyarrow() -> "ModuleType":
    try:
        import pyarrow as pa  # ty: ignore[import-not-found]
    except ImportError:
        raise ImportError(
            "pyarrow is required for Arrow conversion. Install with: pip install pyarrow"
        ) from None
    return pa


_LOGICAL_AND = 0
_LOGICAL_OR = 1

_NormForm = Literal["NFC", "NFKC", "NFD", "NFKD"]
# c_api.h DFTU_NORM_* and DFTU_WRAP_*.
_NORM_FORMS: Dict[str, int] = {"NFC": 0, "NFKC": 1, "NFD": 2, "NFKD": 3}
_WRAP_FLAG_BITS = (1, 2, 4, 8, 16)


def _norm_form_code(form: str) -> int:
    code = _NORM_FORMS.get(form) if isinstance(form, str) else None
    if code is None:
        raise ValueError(f"invalid normalization form {form!r}")
    return code


def _translate_pairs(
    table: Mapping[Union[int, str], Union[str, int, None]],
) -> Tuple[List[str], List[Optional[str]]]:
    """The keys (one codepoint each) and the replacements (``None`` deletes) of
    a ``str.translate`` table: an int key or value is a codepoint."""
    keys: List[str] = []
    values: List[Optional[str]] = []
    for key, value in table.items():
        if isinstance(key, int) and not isinstance(key, bool):
            key = chr(key)
        if not isinstance(key, str) or len(key) != 1:
            raise ValueError(f"translate keys must be a codepoint or one character, got {key!r}")
        if isinstance(value, int) and not isinstance(value, bool):
            value = chr(value)
        elif value is not None and not isinstance(value, str):
            raise TypeError(f"translate values must be str, int or None, got {value!r}")
        keys.append(key)
        values.append(value)
    if len(set(keys)) != len(keys):
        raise ValueError("translate table has a codepoint twice")
    return keys, values


def _wrap_flag_bits(
    expand_tabs: bool,
    replace_whitespace: bool,
    drop_whitespace: bool,
    break_long_words: bool,
    break_on_hyphens: bool,
) -> int:
    given = (expand_tabs, replace_whitespace, drop_whitespace, break_long_words, break_on_hyphens)
    return sum(bit for bit, on in zip(_WRAP_FLAG_BITS, given) if on)


# Right-hand operand for a Series op: another Series or a Python scalar.
_Scalar = Union[int, float]
_SeriesOrScalar = Union["Series", int, float]
_INTEGRAL = {
    DType.INT8,
    DType.INT16,
    DType.INT32,
    DType.INT64,
    DType.UINT8,
    DType.UINT16,
    DType.UINT32,
    DType.UINT64,
    DType.BOOL,
}


class _OpsAccessor:
    """Runs any registered op as a method on a Series: a built-in, with the
    host's ``dftu.`` prefix elided (``s.ops.series.add(other)``,
    ``s.ops.series.count()``), or a module namespace holding a @jit.series
    user op, by its own literal path (``s.ops.stats.zscore()``).
    The Series is the first operand; extra operands follow. Returns a Series for
    a column op, or a scalar for a reducer."""

    __slots__ = ("_series", "_prefix")

    def __init__(self, series: "Series", prefix: str = "") -> None:
        self._series = series
        self._prefix = prefix

    def __getattr__(self, name: str) -> object:
        from .jit import ops as _ops  # lazy: jit imports series

        full = f"{self._prefix}.{name}" if self._prefix else name
        key = _ops._resolve(full)
        if key is None and _ops._is_module(full):
            return _OpsAccessor(self._series, full)
        resolved = key if key is not None else full

        def call(*rest: "Union[Series, int, float, str]") -> object:
            return _ops.run(resolved, self._series, *rest)

        call.__name__ = name
        return call


class Series(_SeriesPandasMixin, _ext.Series[T_co]):
    """A typed column: SIMD ops from the native engine plus Arrow/NumPy conversion.

    Supports NumPy-style ``+ - * /`` against another Series (elementwise) or a
    scalar, ``s[i]`` / ``s[a:b]`` indexing, and ``np.asarray(s)``. Reflected
    scalar division (``2 / s``) is not provided; use ``2 / s.to_numpy()``.
    Comparisons (``<``, ``<=``, ``>``, ``>=``, ``gt``/``ge``/``lt``/``le``/``eq``/
    ``ne``) return a boolean mask Series.

    ``Series([1, 2, 3])`` builds one from a Python sequence, the pandas / polars
    constructor; an Arrow array (anything with ``__arrow_c_array__``) imports
    zero-copy with its own type, so an empty or all-null one keeps it;
    ``Series(series)`` shares the values of another Series. ``name=`` names the
    Series (see :attr:`name`); without it a Series keeps its own name."""

    __slots__ = ()

    # The native type has no constructor of its own: its factories build this
    # class directly (_bind_types), so only a Python-data call comes through
    # here, and __new__ hands back the Series a native importer built.
    def __new__(
        cls,
        data: "Union[Series[Scalar], Sequence[object]]" = (),
        name: Optional[builtins.str] = None,
        dtype: "Union[DType, int, pa.DataType, None]" = None,
    ) -> "Series[Scalar]":
        if cls is not Series:
            raise TypeError(
                "Series cannot be subclassed: the native calls build the class bound by "
                "_bind_types, so an instance of a subclass would come back as a Series"
            )
        if isinstance(data, Series):
            out = data.share() if dtype is None else data.cast(int(dtype))
        elif hasattr(data, "__arrow_c_array__"):
            out = _series_from_arrow(data)
        else:
            out = Series.from_list(list(data), dtype)
        if name is not None:
            out.name = name
        return out

    # Only the checkers read these overloads; the runtime __init__ has nothing
    # left to do after __new__.
    @overload
    def __init__(
        self: "Series[bool]",
        data: Sequence[bool],
        name: Optional[builtins.str] = None,
        dtype: "Union[DType, int, pa.DataType, None]" = None,
    ) -> None: ...
    @overload
    def __init__(
        self: "Series[int]",
        data: Sequence[int],
        name: Optional[builtins.str] = None,
        dtype: "Union[DType, int, pa.DataType, None]" = None,
    ) -> None: ...
    @overload
    def __init__(
        self: "Series[float]",
        data: Sequence[float],
        name: Optional[builtins.str] = None,
        dtype: "Union[DType, int, pa.DataType, None]" = None,
    ) -> None: ...
    @overload
    def __init__(
        self: "Series[builtins.str]",
        data: Sequence[builtins.str],
        name: Optional[builtins.str] = None,
        dtype: "Union[DType, int, pa.DataType, None]" = None,
    ) -> None: ...
    @overload
    def __init__(
        self,
        data: "Union[Series[Scalar], Sequence[object]]" = (),
        name: Optional[builtins.str] = None,
        dtype: "Union[DType, int, pa.DataType, None]" = None,
    ) -> None: ...
    def __init__(
        self,
        data: "Union[Series[Scalar], Sequence[object]]" = (),
        name: Optional[builtins.str] = None,
        dtype: "Union[DType, int, pa.DataType, None]" = None,
    ) -> None:
        pass

    @classmethod
    def from_arrow(cls, arr: "Union[pa.Array, pa.ChunkedArray]") -> "Series":
        """Import a ``pyarrow.Array`` (or any ``__arrow_c_array__`` provider),
        zero-copy via the Arrow C Data Interface. A ``pyarrow.ChunkedArray``
        (or any ``__arrow_c_stream__`` provider) keeps one chunk per batch."""
        return _series_from_arrow(arr)

    @classmethod
    def from_pandas(cls, s: "pd.Series") -> "Series":
        """Import a ``pandas.Series`` through NumPy, with no pyarrow: a NaN in a
        float column and a ``NaT`` or ``NA`` read as null, a categorical as its
        values, a zoned ``datetime64`` as a zoned timestamp. An Arrow-backed
        column imports through pyarrow. The name is ``s.name`` when that is a
        ``str``; any other name (``None``, an int, a tuple) is dropped."""
        from ._import import NeedsArrow, pandas_to_native

        try:
            out = pandas_to_native(s)
        except NeedsArrow:
            out = _series_from_arrow(_require_pyarrow().Array.from_pandas(s))
        if isinstance(s.name, str):
            out.name = s.name
        return out

    @classmethod
    def from_polars(cls, s: "pl.Series") -> "Series":
        """Import a ``polars.Series`` with no pyarrow. The name is ``s.name``;
        polars' default name ``""`` means no name."""
        from ._import import NeedsArrow, polars_to_native

        try:
            out = polars_to_native(s)
        except NeedsArrow:
            _require_pyarrow()
            out = _series_from_arrow(s.to_arrow())
        if s.name:
            out.name = s.name
        return out

    @classmethod
    def from_numpy(cls, a: "np.ndarray") -> "Series":
        """Import a NumPy array with no pyarrow: a 1-D C-contiguous fixed-width
        numeric array borrows the array's buffer; other numbers, bools,
        ``datetime64``, ``timedelta64``, text, bytes and object arrays are
        copied. A NaN stays a NaN; a ``NaT`` or a masked value is null."""
        from ._import import numpy_to_native

        if not type(a).__module__.startswith("numpy.ma"):
            try:
                return _ext._series_from_numpy(a)
            except (TypeError, BufferError, RuntimeError, ValueError):
                pass
        try:
            return numpy_to_native(a)
        except TypeError:
            pa = _require_pyarrow()
            return _series_from_arrow(pa.array(a))

    @classmethod
    def from_list(
        cls, values: Sequence[object], dtype: "Union[DType, int, pa.DataType, None]" = None
    ) -> "Series":
        """Import a Python sequence with no pyarrow: bool, int, float, str,
        bytes, ``datetime.datetime`` (naive or aware), ``date``, ``time``,
        ``timedelta`` and ``decimal.Decimal`` values, ``None`` for null. Int
        columns are int64 (uint64 when too large), decimals decimal128 with the
        widest scale, times and datetimes microseconds. A native ``DType`` types
        an empty or all-null list; a pyarrow type imports through pyarrow. With
        no type, an empty or all-null list is a ``string`` column of nulls, the
        type :meth:`DataFrame.from_pandas` gives a column of ``None``.
        Lists and tuples become list columns and dicts struct columns (keys in
        first-seen order, a missing key null), to any depth."""
        values = list(values)
        if dtype is None and all(v is None for v in values):
            dtype = DType.STRING
        if dtype is None or isinstance(dtype, (DType, int)):
            try:
                if dtype is None:
                    return _ext._series_from_list(values)
                return _ext._series_from_list(values, int(dtype))
            except TypeError:
                if dtype is not None:
                    raise
        pa = _require_pyarrow()
        return _series_from_arrow(pa.array(values, type=dtype))

    def to_arrow(self) -> "pa.Array":
        """This column as a ``pyarrow.Array`` (zero-copy via the C Data
        Interface)."""
        return _require_pyarrow().array(self)

    def to_pandas(self, *, arrow: bool = False, nullable: bool = False) -> "pd.Series":
        """This column as a pandas Series: a NumPy dtype by default (a copy,
        built natively with no pyarrow), except that a dictionary-encoded string
        column is ``category`` and an integer, bool or string column that holds
        nulls is pandas' own nullable dtype (``Int64``, ``boolean``, ``string``).
        On pandas 3 a string column with nulls is the default ``str`` dtype,
        which holds missing values already. A float column keeps NaN. With
        ``arrow=True`` the Series is Arrow-backed, sharing this column's
        buffers, and with ``nullable=True`` it is pandas' own nullable dtype
        (``Int64``, ``Float64``, ``boolean``, ``string``, ...) even without
        nulls, where the column type has one, a null being ``pd.NA`` and a float
        NaN staying NaN (built without pyarrow). A type with no nullable dtype
        converts as without the option. A dictionary column uses pyarrow, where
        installed, to read its codes without building a string per row, and
        keeps the dictionary order of its used values; without pyarrow the
        categories come out sorted. The pandas Series takes this name."""
        if arrow and nullable:
            raise ValueError("to_pandas: pass either arrow=True or nullable=True, not both")
        if arrow:
            out = _to_pandas(self.to_arrow(), arrow)
        else:
            import pandas as pd  # ty: ignore[import-not-found]

            values = _pandas_nullable(self) if nullable else None
            if values is None:
                values = _pandas_default(self)
            out = values if isinstance(values, pd.Series) else pd.Series(values)
        out.name = self.name
        return out

    def to_numpy(self) -> "np.ndarray":
        """This column as a NumPy array, built natively with no pyarrow.

        A flat, non-null, fixed-width numeric column is a zero-copy view of the
        native buffer. Integers with nulls become float64 with NaN, floats keep
        NaN for null, Bool becomes a bool array, and strings, bytes, nested
        types and any column with nulls other than numbers become an object
        array holding ``None`` for null. Dates and timestamps are ``datetime64``
        (UTC values for a zoned timestamp), durations ``timedelta64``, both with
        ``NaT`` for null; times, decimals, fixed-size binary, fixed-size lists
        and maps are object arrays of Python objects."""
        return _native_numpy(self)

    def to_polars(self) -> "pl.Series":
        """This column as a polars Series. Where the extension has Arrow
        support, every column but a Float16 or decimal one comes through the
        Arrow PyCapsule interface (polars >= 1.3; an older polars raises its own
        error), without pyarrow; those, and builds without Arrow, use the
        native conversion (Float16 becomes Float32, a decimal gets precision
        38). A dictionary column becomes ``Categorical``. The polars Series
        takes this name, ``""`` when there is none."""
        try:
            import polars as pl  # ty: ignore[import-not-found]
        except ImportError:
            raise ImportError(
                "polars is required for to_polars(). Install with: pip install polars"
            ) from None
        if _polars_via_arrow(self):
            out = pl.Series(self)  # type: ignore[arg-type]
        else:
            out = _polars_native(self, pl)
        return out.alias(self.name or "")

    def __array__(
        self, dtype: "Optional[np.dtype]" = None, copy: Optional[bool] = None
    ) -> "np.ndarray":
        """NumPy array protocol, so ``np.asarray(series)`` works."""
        import numpy as np  # ty: ignore[import-not-found]

        return np.asarray(self.to_numpy(), dtype=dtype)

    def __repr__(self) -> builtins.str:
        from ._repr import series_repr

        return series_repr(self)

    # builtins.slice (not bare `slice`): ty otherwise binds it to Series.slice.
    @overload
    def __getitem__(self, key: int) -> Optional[T_co]: ...
    @overload
    def __getitem__(self, key: builtins.slice) -> Self: ...
    def __getitem__(self, key: "Union[int, builtins.slice]") -> "Union[Self, Optional[T_co]]":
        """``s[i]`` returns the element as a Python scalar; ``s[a:b]`` returns a
        Series (step-1 slices only)."""
        if isinstance(key, slice):
            start, stop, step = key.indices(len(self))
            if step != 1:
                raise ValueError("Series slicing supports step 1 only")
            stop = max(stop, start)
            try:
                return self.slice(start, stop - start)
            except RuntimeError:
                return super().take(list(range(start, stop)))
        idx = key + len(self) if key < 0 else key
        return super().item(idx)

    def item(self, i: Optional[int] = None) -> Optional[T_co]:
        """The element at row ``i``; with no ``i`` the only element of a
        one-element Series (pandas ``item()``)."""
        if i is None:
            if len(self) != 1:
                raise ValueError(f"item: the Series has {len(self)} elements, not one")
            i = 0
        return super().item(i)

    def rfloordiv(self, other: _Scalar) -> "Series[Scalar]":
        return other // self if isinstance(other, Series) else self.__rfloordiv__(other)

    def rmod(self, other: _Scalar) -> "Series[Scalar]":
        return other % self if isinstance(other, Series) else self.__rmod__(other)

    def rpow(self, other: _Scalar) -> "Series[Scalar]":
        return other**self if isinstance(other, Series) else self.__rpow__(other)

    def divmod(self, other: _SeriesOrScalar) -> "Tuple[Series[Scalar], Series[Scalar]]":
        return self // other, self % other

    def rdivmod(self, other: _Scalar) -> "Tuple[Series[Scalar], Series[Scalar]]":
        return other // self, other % self

    pad = _ext.Series.ffill
    backfill = _ext.Series.bfill

    # Comparisons return a boolean mask Series, ``==`` and ``!=`` included,
    # as pandas and polars; a Series is therefore unhashable, as theirs are.
    # A string scalar compares the text of a String column.
    __hash__ = None  # type: ignore[assignment]

    def gt(self, value: _Scalar) -> "Series[bool]":
        """Boolean mask where the value is greater than ``value``."""
        return self > value

    def ge(self, value: _Scalar) -> "Series[bool]":
        """Boolean mask where the value is greater than or equal to ``value``."""
        return self >= value

    def lt(self, value: _Scalar) -> "Series[bool]":
        """Boolean mask where the value is less than ``value``."""
        return self < value

    def le(self, value: _Scalar) -> "Series[bool]":
        """Boolean mask where the value is less than or equal to ``value``."""
        return self <= value

    def eq(self, value: _Scalar) -> "Series[bool]":
        """Boolean mask where the value equals ``value``."""
        return self == value

    def ne(self, value: _Scalar) -> "Series[bool]":
        """Boolean mask where the value does not equal ``value``."""
        return self != value

    # -- native engine ops (SIMD kernels, wrapped from the C extension) --------

    def div(self, other: "Union[Series, int, float]") -> "Series[float]":
        """True division (pandas ``div``): integer operands widen to Float64
        first. The engine's integer-truncating kernel is ``ops.series.div``."""
        return self / other

    def apply(self, func: "Callable[[T_co], _R]") -> "Series[_R]":
        """``func`` over each element (pandas ``Series.apply``), engine-first:
        the function is traced once with a symbolic column and, when it builds
        a column expression, runs fused in the engine; a ``@jit.series`` or
        ``@jit.op`` function runs through its own path; anything else runs in
        Python per element with a warning naming why. See ``_apply``."""
        from ._apply import apply_series

        return cast("Series[_R]", apply_series(self, func, "Series.apply"))

    def map(self, func: "Callable[[T_co], _R]") -> "Series[_R]":
        """``func`` over each element; the pandas ``Series.map`` spelling of
        :meth:`apply`."""
        from ._apply import apply_series

        return cast("Series[_R]", apply_series(self, func, "Series.map"))

    @overload
    def astype(
        self,
        dtype: Literal[
            DType.INT8,
            DType.INT16,
            DType.INT32,
            DType.INT64,
            DType.UINT8,
            DType.UINT16,
            DType.UINT32,
            DType.UINT64,
        ],
    ) -> "Series[int]": ...
    @overload
    def astype(self, dtype: Literal[DType.FLOAT32, DType.FLOAT64]) -> "Series[float]": ...
    @overload
    def astype(self, dtype: Literal[DType.BOOL]) -> "Series[bool]": ...
    @overload
    def astype(
        self, dtype: Literal[DType.STRING, DType.LARGE_STRING]
    ) -> "Series[builtins.str]": ...
    @overload
    def astype(self, dtype: "builtins.type[int]") -> "Series[int]": ...
    @overload
    def astype(self, dtype: "builtins.type[float]") -> "Series[float]": ...
    @overload
    def astype(self, dtype: "builtins.type[bool]") -> "Series[bool]": ...
    @overload
    def astype(self, dtype: "builtins.type[builtins.str]") -> "Series[builtins.str]": ...
    @overload
    def astype(
        self, dtype: "Union[builtins.str, int, DType, builtins.type]"
    ) -> "Series[Scalar]": ...
    def astype(self, dtype: "Union[builtins.str, int, DType, builtins.type]") -> "Series[Scalar]":
        """Cast to ``dtype``: a :class:`~dftracer.utils.enums.DType`, its
        int code, a dtype name (``"int64"``, ``"Float64"``, case-insensitive;
        matches the ``DType`` member names), a Python type (``str``, ``int``,
        ``float``, ``bool``) or a NumPy scalar type or dtype. Integers, floats
        and bools cast to string (floats as the shortest text that shows a point
        or exponent, bools as ``True`` / ``False`` as Python and pandas print
        them; the native cast under it and the C++ and C APIs spell them
        ``true`` / ``false``); integers and floats cast to
        bool (zero false, a NaN or null null). A pair the engine does not
        support raises ``TypeError`` naming both types. The pandas-style primary
        spelling of :meth:`cast`."""
        target = _resolve_dtype(dtype)
        out = super().cast(target)
        if int(target) == int(DType.STRING) and int(self.dtype) == int(DType.BOOL):
            out = _StrAccessor(
                out
            ).capitalize()  # the engine says true/false; Python says True/False
        return out

    cast = astype

    def compare_series(self, other: "Series", op: int) -> "Series[bool]":
        """Row by row against ``other`` (``op`` a ``_CMP_*`` code): a Bool
        mask, null where either side is null; the engine's
        ``dftu.series.compare_series``. ``==`` / ``<`` / ... spell it."""
        return cast("Series[bool]", _ext.op_run("dftu.series.compare_series", self, other, op))

    def compare(
        self, other: "Series", keep_shape: bool = False, keep_equal: bool = False
    ) -> "DataFrame":
        """The rows where this Series and ``other`` differ, as a frame with
        ``index`` (the row position), ``self`` and ``other`` columns, as pandas
        ``Series.compare``. ``keep_shape`` keeps every row; ``keep_equal``
        keeps the equal values instead of nulling them. The engine's
        ``compare(op, scalar)`` kernel is ``s.ops.series.compare``."""
        from .dataframe import DataFrame

        if not isinstance(other, Series):
            raise TypeError("compare() expects a Series; the scalar kernel is s.ops.series.compare")
        if len(self) != len(other):
            raise ValueError("compare() needs two Series of the same length")
        a = self.to_list()
        b = other.to_list()
        idx: List[int] = []
        keep_left: List[bool] = []
        keep_right: List[bool] = []
        for i, (x, y) in enumerate(zip(a, b)):
            same = x == y or (x is None and y is None)
            if same and not keep_shape:
                continue
            idx.append(i)
            keep_left.append(not same or keep_equal)
            keep_right.append(not same or keep_equal)
        # Typed columns, so no difference at all is still a well-typed frame.
        dtype = DType(self.type)

        def picked(column: "Series", keep: List[bool]) -> "Series":
            rows = column.take(idx)
            if all(keep):
                return rows
            return rows.where(Series.from_list(keep), Series.from_list([None] * len(idx), dtype))

        return DataFrame(
            {
                "index": Series.from_list(idx, DType.INT64),
                "self": picked(self, keep_left),
                "other": picked(other, keep_right),
            }
        )

    def __and__(self, other: "Series") -> "Series[bool]":
        return self.logical(_LOGICAL_AND, other)

    def __or__(self, other: "Series") -> "Series[bool]":
        return self.logical(_LOGICAL_OR, other)

    def __invert__(self) -> "Series[bool]":
        return self.logical_not()

    def value_counts(
        self, normalize: bool = False, sort: bool = True, ascending: bool = False
    ) -> "DataFrame":
        """A ``value`` / ``count`` frame of the distinct values, most frequent
        first (the pandas arguments: ``normalize`` reports a ``proportion``
        column instead, ``ascending`` flips the order, ``sort=False`` keeps
        first-seen order). A frame rather than an indexed Series: this engine
        has no index."""
        from .dataframe import DataFrame

        vc = DataFrame(super().value_counts())
        if normalize:
            total = float(self.length)
            vc = vc.with_column("proportion", vc["count"].astype("float64") / total).select(
                "value", "proportion"
            )
        if sort and ascending:
            vc = vc.reverse()
        return vc

    def clip(
        self,
        lo: Union[int, float, None] = None,
        hi: Union[int, float, None] = None,
        *,
        lower: Union[int, float, None] = None,
        upper: Union[int, float, None] = None,
        lower_bound: Union[int, float, None] = None,
        upper_bound: Union[int, float, None] = None,
    ) -> Self:
        """Clamp to ``[lo, hi]``. ``lower`` / ``upper`` (pandas) and
        ``lower_bound`` / ``upper_bound`` (polars) name the same two bounds. One
        bound is enough: ``clip(lower=0)`` raises values below 0 to 0 and leaves
        the rest, ``clip(upper=b)`` is the mirror. A null stays null. With no
        bound it raises ``TypeError``."""
        lo = _first_set(lo, lower, lower_bound)
        hi = _first_set(hi, upper, upper_bound)
        if lo is None and hi is None:
            raise TypeError("clip() needs a lower bound, an upper bound, or both")
        if lo is None or hi is None:
            limits = _dtype_limits(self.dtype)
            if limits is None:
                raise TypeError(
                    f"clip() with one bound needs a numeric column, not {self.dtype.name.lower()}"
                )
            lo = limits[0] if lo is None else lo
            hi = limits[1] if hi is None else hi
        return super().clip(lo, hi)

    def round(self, decimals: int = 0) -> Self:
        """Round to ``decimals`` places, halves to even, as numpy and pandas:
        the value scaled by ``10**decimals``, rounded and scaled back. A
        negative ``decimals`` rounds to tens, hundreds and so on. An integer
        column stays integer."""
        if decimals == 0 or (decimals > 0 and self.dtype in _INTEGERS):
            return super().round()
        scale = 10.0**decimals if decimals > 0 else 10.0 ** (-decimals)
        if decimals > 0:
            out = (self * scale).round() / scale
        else:
            out = (self / scale).round() * scale
        return cast("Self", out.astype(self.dtype) if self.dtype in _INTEGERS else out)

    def rank(
        self,
        method: Literal["average", "min", "max", "dense", "ordinal", "first"] = "average",
        descending: bool = False,
        *,
        ascending: Optional[bool] = None,
        pct: bool = False,
    ) -> "Series[float]":
        """Rank each element (1-based); ties per ``method``. Always returns a
        Float64 Series, matching pandas ``rank()`` (even ``method="dense"``,
        which pandas also returns as float). Call ``.astype("int64")`` on the
        result for integer ranks. ``ascending`` is the pandas spelling of
        ``not descending``. With ``pct=True`` each rank is divided by the count
        of ranked values (by the number of distinct values for
        ``method="dense"``, as pandas does), a percentile rank in (0, 1]. A null
        and a float ``NaN`` are unranked: each gets a null rank and is not
        counted."""
        if ascending is not None:
            descending = not ascending
        return super().rank(method, descending, pct)

    @overload
    def rolling(self, window: int) -> "_Rolling": ...
    @overload
    def rolling(self, window: int, op: Literal["sum", "mean", "min", "max"]) -> "Series[float]": ...
    def rolling(
        self,
        window: int,
        op: Optional[Literal["sum", "mean", "min", "max"]] = None,
    ) -> "Union[Series[float], _Rolling]":
        """A trailing window of ``window`` rows. With ``op`` the reduced Series;
        without it a window object with ``.sum() .mean() .min() .max() .var()
        .std() .median() .quantile(q)``, the pandas ``s.rolling(n).mean()``
        spelling."""
        if op is None:
            return _Rolling(self, window)
        return super().rolling(window, op)

    def rolling_sum(self, window: int) -> "Series[float]":
        return super().rolling(window, "sum")

    def rolling_mean(self, window: int) -> "Series[float]":
        return super().rolling(window, "mean")

    def rolling_min(self, window: int) -> "Series[float]":
        return super().rolling(window, "min")

    def rolling_max(self, window: int) -> "Series[float]":
        return super().rolling(window, "max")

    def ewm(
        self,
        alpha: Optional[float] = None,
        *,
        span: Optional[float] = None,
        com: Optional[float] = None,
        halflife: Optional[float] = None,
    ) -> "_Ewm":
        """Exponentially weighted window: ``s.ewm(alpha).mean()`` / ``.std()``,
        the pandas spelling of :meth:`ewm_mean` / :meth:`ewm_std`; ``span``,
        ``com`` and ``halflife`` are the pandas spellings of ``alpha``."""
        return _Ewm(self, ewm_alpha(alpha, span, com, halflife))

    def expanding(self) -> "_Expanding":
        """The window from the first row to each row (pandas ``expanding``):
        ``.sum() .mean() .min() .max() .count() .var() .std()``, each from
        the cumulative kernels."""
        return _Expanding(self)

    def cut(self, breaks: "Series", right: bool = False, outer: bool = True) -> "Series[int]":
        """Bin each value by the ascending ``breaks``. By default the intervals are
        ``[a, b)`` and the bin is the count of breaks at or below the value
        (0 to ``len(breaks)``). ``right=True`` closes each interval on the right,
        ``(a, b]``. ``outer=False`` gives null for a value outside the interior
        intervals and numbers them from 0; ``cut(breaks, right=True, outer=False)``
        equals ``pandas.cut(labels=False)``. A null value gives a null bin."""
        flags = (1 if right else 0) | (0 if outer else 2)
        return super().cut(breaks, flags)

    def where(
        self, cond: "Series", other: "Union[Series, int, float, None]" = None
    ) -> "Series[Scalar]":
        """This value where ``cond`` is true, else ``other`` (pandas
        ``Series.where``; a scalar ``other`` broadcasts, a float one widens
        the result to Float64; a null ``other`` (``None`` or ``pd.NA``, the
        default) makes every row where ``cond`` is false null, in this
        column's own type; a null in ``cond`` takes ``other``)."""
        a, b = self._arms(other)
        return _ext.Series.where(a, cond, b)

    def mask(
        self, cond: "Series", other: "Union[Series, int, float, None]" = None
    ) -> "Series[Scalar]":
        """``other`` where ``cond`` is true (null by default), else this value
        (pandas ``Series.mask``)."""
        a, b = self._arms(other)
        return _ext.Series.where(b, cond, a)

    def _arms(
        self, other: "Union[Series, int, float, None]"
    ) -> "Tuple[Series[Scalar], Series[Scalar]]":
        if isinstance(other, Series):
            return self, other
        if other is None or type(other).__name__ == "NAType":
            return self, _ext._series_nulls(int(self.dtype), len(self))
        if isinstance(other, bool) or not isinstance(other, (int, float)):
            raise TypeError("other must be a Series, a number or null")
        me = self
        if isinstance(other, float) and self.dtype not in (DType.FLOAT32, DType.FLOAT64):
            me = self.astype(DType.FLOAT64)
        return me, me.full_like(other)

    def full_like(self, value: Union[int, float]) -> "Series[Scalar]":
        """A Series of this length holding ``value`` on every row, in this
        Series' numeric type (Float64 for a float value, Int64 when this
        Series is not numeric). The constant rides the null mask (0 / 1,
        never NaN or null), so no cell of this Series leaks into it."""
        target = self.dtype
        if isinstance(value, float) or target not in _NUMERIC_DTYPES:
            target = DType.FLOAT64 if isinstance(value, float) else DType.INT64
        return self.isna().astype(target).mul_scalar(0).add_scalar(value)

    def mode(self) -> "Series[Scalar]":
        """The most frequent value as a one-element Series (pandas returns every
        mode; the engine keeps one)."""
        return Series([super().mode()])

    def take(self, indices: "Union[Series, Sequence[int]]") -> Self:
        """The rows at ``indices`` (a Series or a list; any order, repeats
        allowed)."""
        return super().take(_indices(indices))

    @overload
    def is_null(self) -> "Series[bool]": ...
    @overload
    def is_null(self, i: int) -> bool: ...
    def is_null(self, i: Optional[int] = None) -> "Union[bool, Series[bool]]":
        """With ``i``, whether row ``i`` is null; without, the Bool mask of
        null rows (polars ``is_null()``, pandas ``isna()``)."""
        if i is None:
            return self._null_mask()
        return super().is_null(i)

    is_not_null = _ext.Series._valid_mask

    @property
    def is_unique(self) -> bool:
        """Whether every value occurs once (the pandas property). The per-row
        mask is :meth:`unique_mask`."""
        return not self.is_duplicated().any()

    unique_mask = _ext.Series.is_unique

    def is_in(self, values: "Union[Series, Sequence[object]]") -> "Series[bool]":
        """Bool mask: the value is one of ``values`` (a Series or a list; an
        empty list matches nothing)."""
        if not isinstance(values, Series):
            items = list(values)
            if not items:
                # Present and null at once: false on every row.
                return self.isna().logical(0, self.notna())
            values = Series.from_list(items)
        return super().is_in(values)

    def shift(self, n: int = 1, *, periods: Optional[int] = None) -> Self:
        """Shift by ``n`` rows (``periods`` is the pandas spelling)."""
        if periods is not None:
            n = periods
        return super().shift(n)

    def sample(self, n: int, seed: int = 0, *, random_state: Optional[int] = None) -> Self:
        """A deterministic ``n``-row sample (``random_state`` is the pandas
        spelling of ``seed``)."""
        if random_state is not None:
            seed = random_state
        return super().sample(n, seed)

    @property
    def ops(self: "Series[Scalar]") -> "_OpsAccessor":
        """Call any registered op (built-in or @jit.series user op) as a method,
        e.g. ``s.ops.series.add(other)``, ``s.ops.series.count()``, ``s.ops.stats.zscore()``."""
        return _OpsAccessor(self)

    # -- pandas / polars spellings ---------------------------------------------
    # Thin aliases over the native methods above, so either library's muscle
    # memory works on the same object. pandas names are primary; a polars name
    # that differs stays alongside.
    @property
    def dtype(self) -> DType:
        """The element type as a :class:`~dftracer.utils.enums.DType`."""
        return DType(self.type)

    @property
    def size(self) -> int:
        return self.length

    @property
    def shape(self) -> Tuple[int]:
        return (self.length,)

    def len(self) -> int:
        return self.length

    def to_list(self) -> List[Optional[T_co]]:  # ty: ignore[invalid-generic-class]
        values = self._to_pylist()
        if self.is_json:
            return [None if v is None else json.loads(cast(str, v)) for v in values]
        return values

    def tolist(self) -> List[Optional[T_co]]:  # ty: ignore[invalid-generic-class]
        return self.to_list()

    isna = _ext.Series._null_mask

    isnull = _ext.Series._null_mask

    notna = _ext.Series._valid_mask

    notnull = _ext.Series._valid_mask

    def isin(self, values: "Union[Series, Sequence[object]]") -> "Series[bool]":
        return self.is_in(values)

    dropna = _ext.Series.drop_nulls

    fill_null = _ext.Series.fillna

    def var(self, ddof: int = 1) -> float:
        """Variance with ``ddof`` degrees of freedom (0 = population, 1 =
        sample, the pandas default); the engine has kernels for those two.
        NaN below ``ddof + 1`` present values, as pandas (the kernel's
        readout is 0)."""
        if self.count() < ddof + 1:
            return float("nan")
        return self.variance(_ddof_sample(ddof))

    def std(self, ddof: int = 1) -> float:
        if self.count() < ddof + 1:
            return float("nan")
        return self.stddev(_ddof_sample(ddof))

    skew = _ext.Series.skewness

    kurt = _ext.Series.kurtosis

    n_unique = _ext.Series.nunique

    prod = _ext.Series.product

    argmin = _ext.Series.arg_min

    argmax = _ext.Series.arg_max

    idxmin = _ext.Series.arg_min

    idxmax = _ext.Series.arg_max

    def arg_sort(self, descending: bool = False) -> "Series[int]":
        return self.argsort(descending)

    cumprod = _ext.Series.cum_prod

    cum_sum = _ext.Series.cumsum

    cum_max = _ext.Series.cummax

    cum_min = _ext.Series.cummin

    def sort_values(self, ascending: bool = True) -> Self:
        return self.sort(not ascending)

    def nlargest(self, n: int = 5) -> Self:
        return self.top_k(n)

    def nsmallest(self, n: int = 5) -> Self:
        return self.bottom_k(n)

    searchsorted = _ext.Series.search_sorted

    def gather(self, indices: "Union[Series, Sequence[int]]") -> Self:
        return self.take(indices)

    # ``s.str``: string methods under both spellings, pandas (``s.str.lower()``,
    # ``s.str.contains("x")``, ``s.str.len()``) and polars
    # (``s.str.to_lowercase()``, ``s.str.len_bytes()``). The class-scope names
    # ``str`` and ``list`` shadow the builtins in this class's annotations, so
    # they read ``builtins.str`` and ``builtins.list`` here.
    @property
    def str(self: "Series[builtins.str]") -> "_StrAccessor":
        return _StrAccessor(self)

    @property
    def list(self: "Series[Scalar]") -> "_ListAccessor":
        return _ListAccessor(self)

    @property
    def dt(self: "Series[Scalar]") -> "_DtAccessor":
        return _DtAccessor(self)

    def __reduce__(self) -> "Tuple[Callable[[bytes], Series], Tuple[bytes, ...]]":
        return (_series_from_bytes, (self.to_bytes(),))


def _indices(indices: "Union[Series, Sequence[int]]") -> List[int]:
    if isinstance(indices, Series):
        return [int(cast(int, i)) for i in indices.to_list()]
    return [int(i) for i in indices]


def _first_set(*values: "Optional[Union[int, float]]") -> "Optional[Union[int, float]]":
    for v in values:
        if v is not None:
            return v
    return None


def _ddof_sample(ddof: int) -> bool:
    if ddof == 0:
        return False
    if ddof == 1:
        return True
    raise ValueError("ddof must be 0 (population) or 1 (sample)")


class _Rolling:
    """``s.rolling(window)``: the pandas window object."""

    __slots__ = ("_series", "_window")

    def __init__(self, series: Series, window: int) -> None:
        self._series = series
        self._window = window

    def sum(self) -> "Series[float]":
        return self._series.rolling_sum(self._window)

    def mean(self) -> "Series[float]":
        return self._series.rolling_mean(self._window)

    def min(self) -> "Series[float]":
        return self._series.rolling_min(self._window)

    def max(self) -> "Series[float]":
        return self._series.rolling_max(self._window)

    def var(self) -> "Series[float]":
        return self._series.rolling_var(self._window)

    def std(self) -> "Series[float]":
        return self._series.rolling_std(self._window)

    def median(self) -> "Series[float]":
        return self._series.rolling_median(self._window)

    def quantile(self, q: float) -> "Series[float]":
        return self._series.rolling_quantile(self._window, q)


def ewm_alpha(
    alpha: Optional[float], span: Optional[float], com: Optional[float], halflife: Optional[float]
) -> float:
    """The smoothing ``alpha`` from whichever pandas ``ewm`` parameter is given."""
    given = [p for p in (alpha, span, com, halflife) if p is not None]
    if len(given) != 1:
        raise ValueError("ewm: pass exactly one of alpha, span, com or halflife")
    if alpha is not None:
        if not 0 < alpha <= 1:
            raise ValueError("ewm: alpha must be in (0, 1]")
        return float(alpha)
    if span is not None:
        if span < 1:
            raise ValueError("ewm: span must be >= 1")
        return 2.0 / (span + 1.0)
    if com is not None:
        if com < 0:
            raise ValueError("ewm: com must be >= 0")
        return 1.0 / (1.0 + com)
    assert halflife is not None
    if halflife <= 0:
        raise ValueError("ewm: halflife must be > 0")
    return 1.0 - 0.5 ** (1.0 / halflife)


class _Expanding:
    """``s.expanding()``: the pandas expanding window object, from the
    cumulative kernels (``var`` / ``std`` from the running sum and sum of
    squares, sample ``ddof=1``). A null row repeats the previous value, as
    pandas: the window still holds the earlier rows."""

    __slots__ = ("_series",)

    def __init__(self, series: Series) -> None:
        self._series = series

    def sum(self) -> Series:
        return self._series.cumsum().ffill()

    def min(self) -> Series:
        return self._series.cummin().ffill()

    def max(self) -> Series:
        return self._series.cummax().ffill()

    def count(self) -> "Series[int]":
        return self._series.notna().astype(DType.INT64).cumsum()

    def mean(self) -> "Series[float]":
        return self.sum().astype("float64") / self.count()

    def var(self) -> "Series[float]":
        s = self._series.astype("float64")
        n = self.count()
        total = s.cumsum().ffill()
        squares = (s * s).cumsum().ffill()
        # (S2 - S^2 / n) / (n - 1); a null divisor where n < 2.
        divisor = n.sub_scalar(1).where(n.ge(2), n.shift(len(n)))
        return (squares - total * total / n) / divisor

    def std(self) -> "Series[float]":
        return self.var().sqrt()


class _Ewm:
    """``s.ewm(alpha)``: the pandas exponentially weighted window object."""

    __slots__ = ("_series", "_alpha")

    def __init__(self, series: Series, alpha: float) -> None:
        self._series = series
        self._alpha = alpha

    def mean(self) -> "Series[float]":
        return self._series.ewm_mean(self._alpha)

    def std(self) -> "Series[float]":
        return self._series.ewm_std(self._alpha)


_NUMERIC_DTYPES = {
    DType.INT8,
    DType.INT16,
    DType.INT32,
    DType.INT64,
    DType.UINT8,
    DType.UINT16,
    DType.UINT32,
    DType.UINT64,
    DType.FLOAT32,
    DType.FLOAT64,
}

_TIME_UNITS = {"s": 0, "sec": 0, "ms": 1, "us": 2, "ns": 3}
_FREQ_UNITS = {
    "ns": 1,
    "us": 1_000,
    "ms": 1_000_000,
    "s": 1_000_000_000,
    "sec": 1_000_000_000,
    "min": 60_000_000_000,
    "t": 60_000_000_000,
    "h": 3_600_000_000_000,
    "d": 86_400_000_000_000,
}


class _DtAccessor:
    """``s.dt``: the calendar parts of a Timestamp / Date / Duration Series
    in its own unit, or of an Int64 Series read as microseconds
    (``s.dt("ns")`` picks another unit). Naive: a timezone is ignored and
    every part is UTC, as pandas on a tz-naive column."""

    __slots__ = ("_s", "_unit")

    def __init__(self, series: Series, unit: Optional[str] = None) -> None:
        self._s = series
        if unit is None:
            # A temporal column carries its own unit; an Int64 reads as us.
            unit = "us"
            if series.dtype in (DType.TIMESTAMP, DType.DURATION, DType.TIME64):
                unit = ("s", "ms", "us", "ns")[series.time_unit]
            elif series.dtype == DType.DATE64:
                unit = "ms"
        if unit not in _TIME_UNITS:
            raise ValueError("dt: unit must be s, ms, us or ns")
        self._unit = unit

    def __call__(self, unit: str) -> "_DtAccessor":
        return _DtAccessor(self._s, unit)

    def _part(self, code: int) -> Series:
        return self._s.dt_part(code, _TIME_UNITS[self._unit])

    def strftime(self, fmt: str) -> Series:
        """Format each instant as UTC text (pandas ``strftime``) for the
        directives %Y %y %m %d %H %I %M %S %f %j %a %A %b %B %p %F %T %s %z
        %Z %%; another directive raises ValueError. Unlike pandas on a naive
        column, %z prints ``+0000`` and %Z prints ``UTC`` rather than empty."""
        return self._s.dt_format(fmt, _TIME_UNITS[self._unit])

    @property
    def year(self) -> Series:
        return self._part(0)

    @property
    def month(self) -> Series:
        return self._part(1)

    @property
    def day(self) -> Series:
        return self._part(2)

    @property
    def hour(self) -> Series:
        return self._part(3)

    @property
    def minute(self) -> Series:
        return self._part(4)

    @property
    def second(self) -> Series:
        return self._part(5)

    @property
    def millisecond(self) -> Series:
        return self._part(6)

    @property
    def microsecond(self) -> Series:
        """Microseconds within the second (pandas: the whole sub-second part
        in microseconds)."""
        return self._part(7)

    @property
    def nanosecond(self) -> Series:
        """Nanoseconds within the microsecond, as pandas."""
        return self._part(8)

    @property
    def dayofweek(self) -> Series:
        """Monday is 0 (``weekday`` / ``day_of_week`` likewise)."""
        return self._part(9)

    weekday = dayofweek
    day_of_week = dayofweek

    @property
    def dayofyear(self) -> Series:
        return self._part(10)

    day_of_year = dayofyear

    @property
    def quarter(self) -> Series:
        return self._part(11)

    @property
    def is_leap_year(self) -> Series:
        return self._part(12).eq(1)

    @property
    def days_in_month(self) -> Series:
        return self._part(13)

    daysinmonth = days_in_month

    @property
    def is_month_start(self) -> Series:
        return self.day.eq(1)

    @property
    def is_month_end(self) -> Series:
        return (self.day - self.days_in_month).eq(0)

    @property
    def is_year_start(self) -> Series:
        return self.dayofyear.eq(1)

    @property
    def is_year_end(self) -> Series:
        return self.month.eq(12).logical(0, self.day.eq(31))

    @property
    def is_quarter_start(self) -> Series:
        return self.day.eq(1).logical(0, (self.month - 1).mod(3).eq(0))

    @property
    def is_quarter_end(self) -> Series:
        return self.is_month_end.logical(0, self.month.mod(3).eq(0))

    def isocalendar(self) -> "DataFrame":
        """A frame of ISO ``year``, ``week`` and ``day`` (Monday = 1)."""
        from .dataframe import DataFrame

        return DataFrame(
            {
                "year": self._part(15),
                "week": self._part(14),
                "day": self.dayofweek + 1,
            }
        )

    def _every(self, freq: "Union[int, str]") -> int:
        from .indexing import rule_to_units

        return rule_to_units(freq, self._unit)

    def floor(self, freq: "Union[int, str]") -> Series:
        """Each instant floored to a multiple of ``freq`` (``"5min"``, ``"1h"``,
        ``"D"``, or a count of the column's units); ``ceil`` and ``round``
        (half to even) likewise."""
        return self._s.dt_round(self._every(freq), 0)

    def ceil(self, freq: "Union[int, str]") -> Series:
        return self._s.dt_round(self._every(freq), 1)

    def round(self, freq: "Union[int, str]") -> Series:
        return self._s.dt_round(self._every(freq), 2)

    def normalize(self) -> Series:
        """Each instant at midnight of its day."""
        return self.floor("1d")

    def total_seconds(self) -> Series:
        """Each value as seconds (Float64), for a Duration or an Int64 read
        in the accessor's unit."""
        per_second = 1_000_000_000 / _FREQ_UNITS[self._unit]
        return self._s.astype("float64") / per_second

    @property
    def tz(self) -> Optional[str]:
        """The zone name of a Timestamp column, or None when naive."""
        return self._s.timezone or None

    def tz_localize(self, tz: Optional[str]) -> Series:
        """A naive Timestamp column marked as ``tz`` (pandas
        ``tz_localize``); ``None`` strips the zone of an aware one. The
        engine has no zone database, so only ``"UTC"`` (and its spellings)
        can mark a naive column: its ticks already are UTC instants."""
        if tz is None:
            return self._s.with_timezone("")
        if self.tz is not None:
            raise TypeError(f"tz_localize: already tz-aware ({self.tz!r}); use tz_convert")
        if tz.upper() not in ("UTC", "ETC/UTC", "Z", "GMT"):
            raise ValueError(
                f"tz_localize: only UTC can mark a naive column, not {tz!r} "
                "(no zone database in the engine)"
            )
        return self._s.with_timezone(tz)

    def tz_convert(self, tz: Optional[str]) -> Series:
        """The same instants shown in zone ``tz`` (pandas ``tz_convert``):
        the type's zone changes, no tick moves. Needs a tz-aware column;
        ``None`` gives the naive UTC column. The ``.dt`` calendar parts
        stay UTC."""
        if self.tz is None:
            raise TypeError("tz_convert: a naive column; tz_localize('UTC') first")
        return self._s.with_timezone(tz or "")


class _ListAccessor:
    """``s.list``: the List column accessors (polars ``s.list.len()`` /
    ``s.list.get(i)``)."""

    __slots__ = ("_s",)

    def __init__(self, series: Series) -> None:
        self._s = series

    def len(self) -> Series:
        return self._s.list_len()

    def get(self, index: int) -> Series:
        return self._s.list_get(index)

    def first(self) -> Series:
        return self._s.list_get(0)

    def last(self) -> Series:
        return self._s.list_get(-1)


class _StrAccessor:
    """``s.str``: the string kernels under pandas and polars names."""

    __slots__ = ("_s",)

    def __init__(self, series: Series) -> None:
        self._s = series

    # pandas
    def lower(self) -> "Series[builtins.str]":
        return self._s.to_lowercase()

    def upper(self) -> "Series[builtins.str]":
        return self._s.to_uppercase()

    def strip(self) -> "Series[builtins.str]":
        return self._s.str_strip()

    def lstrip(self) -> "Series[builtins.str]":
        return self._s.str_lstrip()

    def rstrip(self) -> "Series[builtins.str]":
        return self._s.str_rstrip()

    def len(self) -> "Series[int]":
        """Codepoint count, as pandas ``str.len()``."""
        return self._s.str_len_chars()

    def contains(self, pat: str, case: bool = True, regex: bool = False) -> "Series[bool]":
        """Substring (or with ``regex=True`` a regex search) predicate."""
        if regex:
            if not case:
                raise NotImplementedError("case-insensitive regex has no kernel; lower() first")
            return self._s.str_search(pat)
        if case:
            return self._s.str_contains(pat)
        return self._s.to_lowercase().str_contains(pat.lower())

    def startswith(self, pat: str) -> "Series[bool]":
        return self._s.str_starts_with(pat)

    def endswith(self, pat: str) -> "Series[bool]":
        return self._s.str_ends_with(pat)

    def match(self, pat: str) -> "Series[bool]":
        """Regex anchored at the start, as pandas ``str.match``."""
        return self._s.str_search("^(?:" + pat + ")")

    def fullmatch(self, pat: str) -> "Series[bool]":
        return self._s.str_matches(pat)

    def find(self, sub: str) -> "Series[int]":
        return self._s.str_find(sub)

    def replace(self, pat: str, repl: str, n: int = -1) -> "Series[builtins.str]":
        """Literal replace: every occurrence (``n=-1``) or the first (``n=1``)."""
        if n == -1:
            return self._s.str_replace_all(pat, repl)
        if n == 1:
            return self._s.str_replace(pat, repl)
        raise NotImplementedError("replace() supports n=-1 (all) or n=1 (first)")

    def slice(self, start: int = 0, stop: Optional[int] = None) -> "Series[builtins.str]":
        """Byte substring ``[start, stop)``, as pandas ``str.slice``."""
        length = -1 if stop is None else max(stop - start, 0)
        return self._s.str_slice(start, length)

    def pad(
        self, width: int, side: Literal["left", "right"] = "left", fillchar: str = " "
    ) -> "Series[builtins.str]":
        if side == "left":
            return self._s.str_pad_start(width, fillchar)
        if side == "right":
            return self._s.str_pad_end(width, fillchar)
        raise ValueError("side must be 'left' or 'right'")

    def zfill(self, width: int) -> "Series[builtins.str]":
        return self._s.str_zfill(width)

    def split(self, pat: str, expand: bool = False) -> "Union[Series, DataFrame]":
        """Split on the literal ``pat``: a ``list<string>`` Series, or with
        ``expand=True`` one String column per piece named ``"0"``, ``"1"``,
        ... (a short row's missing pieces are null), as pandas."""
        parts = self._s.str_split(pat)
        if not expand:
            return parts
        from .dataframe import DataFrame

        width = parts.list_len().max()
        n = int(width) if width is not None else 0
        return DataFrame.from_dict({str(i): parts.list_get(i) for i in range(n)})

    def regex_replace(self, pat: str, to: str) -> "Series[builtins.str]":
        """:meth:`Series.str_regex_replace`: ``$n``/``${name}`` group references."""
        return self._s.str_regex_replace(pat, to)

    def extract(self, pat: str, group: int = 1) -> "Series[builtins.str]":
        """The capture ``group`` of the first regex ``pat`` match per row, null
        where there is no match (pandas ``str.extract`` for one group)."""
        return self._s.str_extract(pat, group)

    # polars
    def to_lowercase(self) -> "Series[builtins.str]":
        return self._s.to_lowercase()

    def to_uppercase(self) -> "Series[builtins.str]":
        return self._s.to_uppercase()

    def strip_chars(self) -> "Series[builtins.str]":
        return self._s.str_strip()

    def len_bytes(self) -> "Series[int]":
        return self._s.str_len_bytes()

    def len_chars(self) -> "Series[int]":
        return self._s.str_len_chars()

    def starts_with(self, prefix: str) -> "Series[bool]":
        return self._s.str_starts_with(prefix)

    def ends_with(self, suffix: str) -> "Series[bool]":
        return self._s.str_ends_with(suffix)

    def replace_all(self, pat: str, repl: str) -> "Series[builtins.str]":
        return self._s.str_replace_all(pat, repl)

    def pad_start(self, width: int, fill: str = " ") -> "Series[builtins.str]":
        return self._s.str_pad_start(width, fill)

    def pad_end(self, width: int, fill: str = " ") -> "Series[builtins.str]":
        return self._s.str_pad_end(width, fill)

    # pandas, the rest of the surface (ASCII; a byte outside ASCII is left as
    # it is by the case forms and fails the character classes).
    def capitalize(self) -> "Series[builtins.str]":
        return self._s.str_case(0)

    def title(self) -> "Series[builtins.str]":
        return self._s.str_case(1)

    def swapcase(self) -> "Series[builtins.str]":
        return self._s.str_case(2)

    def casefold(self) -> "Series[builtins.str]":
        return self._s.to_lowercase()

    def isalnum(self) -> "Series[bool]":
        return self._s.str_is(0)

    def encode(self, encoding: str = "utf-8", errors: str = "strict") -> Series:
        """The UTF-8 bytes of each string, as a binary column (pandas
        ``str.encode``). Only UTF-8, the engine's text encoding."""
        from .columnar import _require_utf8

        _require_utf8("encode", encoding)
        return self._s.astype("binary")

    def decode(self, encoding: str = "utf-8", errors: str = "strict") -> Series:
        """Each binary value read as UTF-8 text (pandas ``str.decode``). With
        ``errors="strict"`` a value that is not valid UTF-8 raises
        ``ValueError``; with ``errors="null"`` it becomes null."""
        from .columnar import _require_utf8

        _require_utf8("decode", encoding)
        if errors not in ("strict", "null"):
            raise ValueError(f"decode: errors must be 'strict' or 'null', not {errors!r}")
        out = self._s.astype("string")
        if errors == "strict" and out.null_count > self._s.null_count:
            raise ValueError("decode: a value is not valid utf-8")
        return out

    def isascii(self) -> "Series[bool]":
        """True where every character is ASCII, as ``str.isascii`` (an empty
        string too)."""
        return self.fullmatch("[\\x00-\\x7f]*")

    def isalpha(self) -> "Series[bool]":
        return self._s.str_is(1)

    def isdigit(self) -> "Series[bool]":
        return self._s.str_is(2)

    def isdecimal(self) -> "Series[bool]":
        return self._s.str_is(3)

    def isnumeric(self) -> "Series[bool]":
        return self._s.str_is(4)

    def isspace(self) -> "Series[bool]":
        return self._s.str_is(5)

    def islower(self) -> "Series[bool]":
        return self._s.str_is(6)

    def isupper(self) -> "Series[bool]":
        return self._s.str_is(7)

    def istitle(self) -> "Series[bool]":
        return self._s.str_is(8)

    def count(self, pat: str) -> "Series[int]":
        """Non-overlapping occurrences of the literal ``pat`` per row."""
        return self._s.str_count(pat)

    def rfind(self, sub: str) -> "Series[int]":
        """Byte index of the last ``sub`` per row, or -1."""
        return self._s.str_rfind(sub)

    def index(self, sub: str) -> "Series[int]":
        """As :meth:`find`, but a row without ``sub`` raises (``ValueError``),
        as Python's ``str.index``."""
        out = self._s.str_find(sub)
        if out.eq(-1).any():
            raise ValueError(f"index: substring {sub!r} not found in every row")
        return out

    def rindex(self, sub: str) -> "Series[int]":
        out = self._s.str_rfind(sub)
        if out.eq(-1).any():
            raise ValueError(f"rindex: substring {sub!r} not found in every row")
        return out

    def removeprefix(self, prefix: str) -> "Series[builtins.str]":
        return self._s.str_remove_prefix(prefix)

    def removesuffix(self, suffix: str) -> "Series[builtins.str]":
        return self._s.str_remove_suffix(suffix)

    def repeat(self, repeats: int) -> "Series[builtins.str]":
        return self._s.str_repeat(repeats)

    def center(self, width: int, fillchar: str = " ") -> "Series[builtins.str]":
        return self._s.str_center(width, fillchar)

    def ljust(self, width: int, fillchar: str = " ") -> "Series[builtins.str]":
        return self._s.str_pad_end(width, fillchar)

    def rjust(self, width: int, fillchar: str = " ") -> "Series[builtins.str]":
        return self._s.str_pad_start(width, fillchar)

    def cat(self, others: "Union[Series, str, None]" = None, sep: str = "") -> "Union[Series, str]":
        """Row-wise concatenation with ``others`` (a Series or one string),
        ``sep`` between; with no ``others`` the rows joined into one string,
        as pandas."""
        s = self._s
        if others is None:
            return sep.join(str(v) for v in s.to_list() if v is not None)
        if isinstance(others, str):
            others = Series.from_list([others] * len(s))
        if sep:
            s = s.str_cat(Series.from_list([sep] * len(s)))
        return s.str_cat(others)

    def findall(self, pat: str) -> Series:
        """Every regex match per row, as a List<String>."""
        return self._s.str_findall(pat)

    def normalize(self, form: _NormForm) -> "Series[builtins.str]":
        """Each string in the Unicode normalization ``form`` (``"NFC"``,
        ``"NFKC"``, ``"NFD"`` or ``"NFKD"``); a value that is not valid UTF-8
        is null."""
        return self._s.str_normalize(_norm_form_code(form))

    def translate(
        self, table: Mapping[Union[int, str], Union[str, int, None]]
    ) -> "Series[builtins.str]":
        """Each codepoint replaced by ``table`` (as ``str.translate``): a key is
        a codepoint or a one-character string, a value a string, a codepoint or
        ``None`` to delete it. A codepoint not in the table is kept."""
        keys, values = _translate_pairs(table)
        return self._s.str_translate(
            Series.from_list(keys, dtype=DType.STRING),
            Series.from_list(values, dtype=DType.STRING),
        )

    def wrap(
        self,
        width: int,
        expand_tabs: bool = True,
        replace_whitespace: bool = True,
        drop_whitespace: bool = True,
        break_long_words: bool = True,
        break_on_hyphens: bool = True,
    ) -> Series:
        """Each string wrapped to lines of at most ``width`` codepoints, joined
        by newlines: ``textwrap.TextWrapper`` of CPython 3.13.10, 3.14.1 and later with these
        options, as pandas on those versions."""
        if width < 1:
            raise ValueError(f"invalid width {width!r} (must be > 0)")
        flags = _wrap_flag_bits(
            expand_tabs, replace_whitespace, drop_whitespace, break_long_words, break_on_hyphens
        )
        return self._s.str_wrap(width, flags)

    def extractall(self, pat: str) -> "DataFrame":
        """Every regex ``pat`` match per row, one output row per match: the
        columns ``row`` (input position) and ``match`` (0-based order within
        the row), then one per capture group, named as in ``pat`` or
        ``"0"``, ``"1"``, ...  Rows without a match are left out (pandas
        ``str.extractall`` with its index as columns)."""
        import re

        from .dataframe import DataFrame

        rx = re.compile(pat)
        if rx.groups == 0:
            raise ValueError("extractall: the pattern has no capture group")
        names = {i: n for n, i in rx.groupindex.items()}
        labels = [names.get(g, str(g - 1)) for g in range(1, rx.groups + 1)]
        found = DataFrame.from_dict({"m": self._s.str_findall(pat)})
        rows = found.with_row_index("row").explode("m")
        rows = rows[rows["m"].notna()]
        if len(rows) == 0:
            ints = {c: Series.from_list([], dtype=DType.INT64) for c in ["row", "match"]}
            text = {c: Series.from_list([], dtype=DType.STRING) for c in labels}
            return DataFrame.from_dict({**ints, **text})
        rows = rows.assign(match=rows.groupby("row").cumcount()["cumcount"])
        m = rows["m"]
        groups = {c: _StrAccessor(m).extract(pat, g) for g, c in enumerate(labels, 1)}
        return DataFrame.from_dict({"row": rows["row"], "match": rows["match"], **groups})

    def partition(self, sep: str = " ", expand: bool = True) -> "Union[Series, DataFrame]":
        """Split at the first ``sep`` into (head, sep, tail): a frame of
        columns ``"0"``, ``"1"``, ``"2"`` (or with ``expand=False`` the
        List<String>); ``rpartition`` splits at the last."""
        return self._expand3(self._s.str_partition(sep, False), expand)

    def rpartition(self, sep: str = " ", expand: bool = True) -> "Union[Series, DataFrame]":
        return self._expand3(self._s.str_partition(sep, True), expand)

    def _expand3(self, parts: Series, expand: bool) -> "Union[Series, DataFrame]":
        if not expand:
            return parts
        from .dataframe import DataFrame

        return DataFrame.from_dict({str(i): parts.list_get(i) for i in range(3)})

    def rsplit(self, pat: str, expand: bool = False) -> "Union[Series, DataFrame]":
        """As :meth:`split` (every occurrence, so the same parts)."""
        return self.split(pat, expand)

    def join(self, sep: str) -> Series:
        """Each row's list of strings joined with ``sep`` (a List<String>
        Series, as :meth:`split` gives)."""
        return self._s.list_join(sep)

    def get(self, i: int) -> Series:
        """The element at ``i`` of each row: a list's element (negative
        counts from the end), or a string's byte ``i`` (non-negative; empty
        past the end)."""
        from .enums import DType

        s = self._s
        if s.dtype == DType.LIST:
            return s.list_get(i)
        if i < 0:
            raise ValueError("get: a negative position needs a List column; use str.slice")
        nulls = Series.from_list([None] * len(s), dtype=DType.STRING)
        return s.str_slice(i, 1).where(s.str_len_bytes().gt(i), nulls)

    def slice_replace(
        self, start: int = 0, stop: Optional[int] = None, repl: str = ""
    ) -> "Series[builtins.str]":
        """Bytes ``[start, stop)`` of each row replaced with ``repl``."""
        s = self._s
        head = s.str_slice(0, start)
        tail = s.str_slice(stop, 1 << 62) if stop is not None else s.str_slice(0, 0)
        return head.str_cat(Series.from_list([repl] * len(s))).str_cat(tail)

    def get_dummies(self, sep: str = "|") -> "DataFrame":
        """One column per distinct ``sep``-separated token, 1 where a row
        holds it (pandas ``str.get_dummies``)."""
        from .dataframe import DataFrame

        parts = self._s.str_split(sep)
        exploded = DataFrame.from_dict({"v": parts}).with_row_index("r").explode("v")
        wide = exploded.pivot(index="r", on="v", values="v", agg="count").drop("r").fillna(0)
        # Presence (1), not a count; an empty token is no category.
        names = [c for c in wide.columns if c != ""]
        return DataFrame({c: wide[c].gt(0).astype("int64") for c in names})


def _like(ints: Series, template: Series) -> Series:
    """The int64 column ``ints`` read as the type of ``template``: a timestamp,
    duration, time or date keeps its unit and zone."""
    dtype = DType(template.type)
    if dtype in (DType.TIMESTAMP, DType.DURATION, DType.TIME64, DType.DATE64):
        return _ext._series_retype(ints, int(dtype), template.time_unit, template.timezone)
    if dtype in (DType.DATE32, DType.TIME32):
        return _ext._series_retype(ints.astype(DType.INT32), int(dtype), template.time_unit, "")
    return ints.astype(dtype)


def _series_from_bytes(data: bytes) -> Series:
    """The Series a pickle holds (``Series.to_bytes`` of the native frame format)."""
    return _ext._series_from_bytes(data)


def _series_from_arrow(array: "Union[pa.Array, pa.ChunkedArray]") -> Series:
    """Import a pyarrow Array into a native Series wrapper."""
    return _arrow_native("_series_from_arrow")(array)


_ext._bind_types(Series=Series)
