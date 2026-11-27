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
import datetime
import json
import math
import numbers
import struct
from typing import (
    TYPE_CHECKING,
    Callable,
    Literal,
    Mapping,
    NoReturn,
    Optional,
    Sequence,
    TypeVar,
    Union,
    cast,
    overload,
)

from . import dftracer_utils_ext as _ext
from ._aliases import install as _install_aliases
from ._convert import (
    NeedsArrow,
    _arrow_native,
    _native_numpy,
    _pandas_default,
    _pandas_nullable,
    _polars_native,
    _polars_via_arrow,
    _require_pyarrow,
    _row_indices,
    arrow_native_type,
    arrow_pandas,
    is_na,
    numpy_to_native,
    pandas_to_native,
    polars_to_native,
    temporal_from_values,
)
from ._enums import (
    DT_UNIT_CODES,
    DTYPE_BY_NAME,
    EPOCH_NS,
    FREQ_NS,
    UNIT_CODES,
    DType,
    dt_unit,
    rule_to_units,
)
from ._pandas_series import SeriesPandasMixin, differs
from ._series_index import (
    default_name,
    drop_levels,
    levels_of,
    names_of,
    pandas_index,
    pandas_labels,
    positional,
    positions,
    with_levels,
)
from ._value_counts import series_value_counts

if TYPE_CHECKING:
    import numpy as np
    import numpy.typing as npt
    import pandas as pd
    import polars as pl
    import pyarrow as pa
    from typing_extensions import Self

    from ._dataframe import DataFrame
    from .dftracer_utils_ext import Scalar, T_co
else:
    # The checkers read T_co (covariant, defaulting to Scalar) from the stub.
    T_co = TypeVar("T_co", covariant=True)

# Python types and the NumPy names that differ from a DType member name.
_PY_DTYPES: dict[type, DType] = {
    str: DType.STRING,
    int: DType.INT64,
    float: DType.FLOAT64,
    bool: DType.BOOL,
}
_NUMPY_NAMES = {"str": "string", "str_": "string", "bool_": "bool"}


def _dtype_limits(dtype: DType) -> tuple[int | float, int | float] | None:
    """The lowest and highest value a numeric column of ``dtype`` can hold (the
    missing bound of a one-sided ``clip``); ``None`` for a type with no limits
    here."""
    if dtype.int_range is not None:
        return dtype.int_range
    if dtype.is_float:
        return float("-inf"), float("inf")
    return None


def _resolve_dtype(dtype: str | int | DType | type) -> int:
    """A dtype spelled as a name, a DType member, a raw int code, a Python type
    (``str``, ``int``, ``float``, ``bool``) or a NumPy scalar type or dtype that
    names a supported type -> int code. Raises ValueError on an unrecognized
    name and TypeError on any other argument."""
    if isinstance(dtype, str):
        try:
            return int(DTYPE_BY_NAME[dtype.lower()])
        except KeyError:
            raise ValueError(f"unknown dtype name: {dtype!r}") from None
    if isinstance(dtype, type) or hasattr(dtype, "kind"):
        py_dtype = _PY_DTYPES.get(cast(type, dtype))
        if py_dtype is not None:
            return int(py_dtype)
        raw_name = getattr(dtype, "name", None)
        name = raw_name if isinstance(raw_name, str) else getattr(dtype, "__name__", "")
        name = _NUMPY_NAMES.get(name, name)
        try:
            return int(DTYPE_BY_NAME[name.lower()])
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
_K = TypeVar("_K")


def require_utf8(op: str, encoding: str) -> None:
    if encoding.lower().replace("_", "-") not in ("utf-8", "utf8"):
        raise ValueError(f"{op}: only utf-8 is supported, not {encoding!r}")


_LOGICAL_AND = 0
_LOGICAL_OR = 1

NormForm = Literal["NFC", "NFKC", "NFD", "NFKD"]
# c_api.h DFTU_NORM_* and DFTU_WRAP_*.
_NORM_FORMS: dict[str, int] = {"NFC": 0, "NFKC": 1, "NFD": 2, "NFKD": 3}
_WRAP_FLAG_BITS = (1, 2, 4, 8, 16)


def norm_form_code(form: str) -> int:
    code = _NORM_FORMS.get(form) if isinstance(form, str) else None
    if code is None:
        raise ValueError(f"invalid normalization form {form!r}")
    return code


def translate_pairs(
    table: Mapping[int | str, str | int | None],
) -> tuple[list[str], list[str | None]]:
    """The keys (one codepoint each) and the replacements (``None`` deletes) of
    a ``str.translate`` table: an int key or value is a codepoint."""
    keys: list[str] = []
    values: list[str | None] = []
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


def wrap_flag_bits(
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


class _OpsAccessor:
    """Runs any registered op as a method on a Series: a built-in, with the
    host's ``dftu.`` prefix elided (``s.ops.series.add(other)``,
    ``s.ops.series.count()``), or a module namespace holding a @jit.series
    user op, by its own literal path (``s.ops.stats.zscore()``).
    The Series is the first operand; extra operands follow. Returns a Series for
    a column op, or a scalar for a reducer."""

    __slots__ = ("_series", "_prefix")

    def __init__(self, series: Series, prefix: str = "") -> None:
        self._series = series
        self._prefix = prefix

    def __getattr__(self, name: str) -> object:
        from .jit import ops as _ops  # lazy: jit imports series

        full = f"{self._prefix}.{name}" if self._prefix else name
        key = _ops._resolve(full)
        if key is None and _ops._is_module(full):
            return _OpsAccessor(self._series, full)
        resolved = key if key is not None else full

        def call(*rest: Series | int | float | str) -> object:
            return _ops.run(resolved, self._series, *rest)

        call.__name__ = name
        return call


def _map_key(key: object, dtype: DType) -> object:
    """``key`` as a value of ``dtype`` that equals it exactly, or ``_NO_KEY``
    when no value of ``dtype`` matches it as a pandas lookup does."""
    if isinstance(key, bool):
        return key if dtype == DType.BOOL else _NO_KEY
    # numpy scalars register as numbers.Integral and numbers.Real.
    if isinstance(key, numbers.Integral):
        key = int(key)
    elif isinstance(key, numbers.Real):
        key = float(key)
    if isinstance(key, (int, float)):
        narrow = _NARROW_FLOAT_FORMATS.get(dtype)
        if not dtype.is_numeric and narrow is None:
            return _NO_KEY
        if dtype.is_float or narrow is not None:
            try:
                as_float = float(key)
                if narrow is not None:
                    as_float = struct.unpack(narrow, struct.pack(narrow, as_float))[0]
            except OverflowError:
                return _NO_KEY
            return as_float if as_float == key else _NO_KEY
        if isinstance(key, float) and not key.is_integer():
            return _NO_KEY
        as_int = int(key)
        limits = dtype.int_range
        if limits is not None and not limits[0] <= as_int <= limits[1]:
            return _NO_KEY
        return as_int
    if isinstance(key, str):
        return key if dtype.is_text else _NO_KEY
    if dtype.is_text or dtype.is_numeric or dtype == DType.BOOL:
        return _NO_KEY
    return key


_NO_KEY = object()
_NARROW_FLOAT_FORMATS = {DType.FLOAT16: "e", DType.FLOAT32: "f"}


def _map_values(s: Series, mapping: Mapping[_K, _R]) -> Series:
    from ._dataframe import DataFrame

    keys: list[object] = []
    values: list[object] = []
    missing_value: object = None
    has_missing = False
    for key, value in mapping.items():
        if key is None or (isinstance(key, float) and key != key):
            missing_value, has_missing = value, True
            continue
        exact = _map_key(key, s.dtype)
        if exact is not _NO_KEY:
            keys.append(exact)
            values.append(value)
    column = positional(s)
    typed = Series.from_list(values + [missing_value] if has_missing else values)
    key_series = Series.from_list(keys).astype(s.dtype) if keys else column.head(0)
    table = DataFrame({"key": key_series, "value": typed.head(len(values))})
    # A left join keeps the left row order, which this relies on.
    out = DataFrame({"key": column}).merge(table, on="key", how="left")["value"]
    if has_missing:
        gap = column.is_null()
        if s.dtype.is_float:
            gap = gap | column.is_nan().fill_null(False)
        fill = typed.take(gap.cast(DType.INT64) * 0 + len(values))
        out = drop_levels(out).mask(drop_levels(gap), drop_levels(fill))
    out = out.rename(s.name)
    return out if not names_of(s) else out.set_axis(s.index)


class Series(SeriesPandasMixin, _ext.Series[T_co]):
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
    Series (see :attr:`name`); without it a Series keeps its own name.
    ``index=`` gives the row labels: a sequence or an array takes them by
    position, while a Series is looked up by them (a label it does not have
    gives null), as pandas."""

    __slots__ = ()

    # The native type has no constructor of its own: its factories build this
    # class directly (_bind_types), so only a Python-data call comes through
    # here, and __new__ hands back the Series a native importer built.
    def __new__(
        cls,
        data: Series[Scalar] | Sequence[object] = (),
        name: builtins.str | None = None,
        dtype: DType | int | builtins.str | pa.DataType | None = None,
        *,
        index: Sequence[object] | None = None,
    ) -> Series[Scalar]:
        if cls is not Series:
            raise TypeError(
                "Series cannot be subclassed: the native calls build the class bound by "
                "_bind_types, so an instance of a subclass would come back as a Series"
            )
        out: Series[Scalar]
        if isinstance(data, Series):
            if dtype is None:
                out = data.share()
            else:
                kind, unit, zone = _list_dtype(dtype)
                if unit and int(kind) != int(data.dtype):
                    raise TypeError(f"Series: cannot cast a {data.dtype.name} column to {dtype}")
                out = data.cast(kind)
                if unit:
                    # the same instants in the unit and zone the dtype names
                    if UNIT_CODES[unit] != out.time_unit:
                        out = out.dt.as_unit(unit)
                    if kind == DType.TIMESTAMP and zone != out.timezone:
                        out = out.with_timezone(zone)
        elif hasattr(data, "__arrow_c_array__"):
            out = _series_from_arrow(data)
        else:
            out = Series.from_list(list(data), dtype)
        if name is not None:
            out.name = name
        if index is not None:
            # A Series is looked up by the new labels, as pandas does; other
            # data takes them by position.
            out = out.reindex(index) if isinstance(data, Series) else out.set_axis(index)
        return out

    # Only the checkers read these overloads; the runtime __init__ has nothing
    # left to do after __new__.
    @overload
    def __init__(
        self: Series[bool],
        data: Sequence[bool],
        name: builtins.str | None = None,
        dtype: DType | int | builtins.str | pa.DataType | None = None,
        *,
        index: Sequence[object] | None = None,
    ) -> None: ...
    @overload
    def __init__(
        self: Series[int],
        data: Sequence[int],
        name: builtins.str | None = None,
        dtype: DType | int | builtins.str | pa.DataType | None = None,
        *,
        index: Sequence[object] | None = None,
    ) -> None: ...
    @overload
    def __init__(
        self: Series[float],
        data: Sequence[float],
        name: builtins.str | None = None,
        dtype: DType | int | builtins.str | pa.DataType | None = None,
        *,
        index: Sequence[object] | None = None,
    ) -> None: ...
    @overload
    def __init__(
        self: Series[builtins.str],
        data: Sequence[builtins.str],
        name: builtins.str | None = None,
        dtype: DType | int | builtins.str | pa.DataType | None = None,
        *,
        index: Sequence[object] | None = None,
    ) -> None: ...
    @overload
    def __init__(
        self,
        data: Series[Scalar] | Sequence[object] = (),
        name: builtins.str | None = None,
        dtype: DType | int | builtins.str | pa.DataType | None = None,
        *,
        index: Sequence[object] | None = None,
    ) -> None: ...
    def __init__(
        self,
        data: Series[Scalar] | Sequence[object] = (),
        name: builtins.str | None = None,
        dtype: DType | int | builtins.str | pa.DataType | None = None,
        *,
        index: Sequence[object] | None = None,
    ) -> None:
        pass

    @classmethod
    def from_arrow(cls, arr: pa.Array | pa.ChunkedArray) -> Series:
        """Import a ``pyarrow.Array`` (or any ``__arrow_c_array__`` provider),
        zero-copy via the Arrow C Data Interface. A ``pyarrow.ChunkedArray``
        (or any ``__arrow_c_stream__`` provider) keeps one chunk per batch."""
        return _series_from_arrow(arr)

    @classmethod
    def from_pandas(cls, s: pd.Series) -> Series:
        """Import a ``pandas.Series`` through NumPy, with no pyarrow: a NaN in a
        float column and a ``NaT`` or ``NA`` read as null, a categorical as its
        values, a zoned ``datetime64`` as a zoned timestamp. An Arrow-backed
        column imports through pyarrow, and a ``str`` column that pyarrow holds
        (the pandas 3 default) shares its text buffer. The name is ``s.name`` when that is a
        ``str``; any other name (``None``, an int, a tuple) is dropped. An index
        other than the default ``RangeIndex`` (``0 .. n-1`` with no name) becomes
        the row labels (a ``MultiIndex`` one level each; an unnamed level is named
        ``__index_level_<i>__``)."""
        out = cls._from_pandas_values(s)
        labels = pandas_labels(s)
        if labels is not None:
            out = with_levels(out, *labels)
        return out

    @classmethod
    def _from_pandas_values(cls, s: pd.Series) -> Series:
        """The values and the name of ``s``, with no index."""
        try:
            out = pandas_to_native(s)
        except NeedsArrow:
            out = _series_from_arrow(_require_pyarrow().Array.from_pandas(s))
        if isinstance(s.name, str):
            out.name = s.name
        return out

    @classmethod
    def from_polars(cls, s: pl.Series) -> Series:
        """Import a ``polars.Series`` with no pyarrow. The name is ``s.name``;
        polars' default name ``""`` means no name."""
        out = polars_to_native(s)
        if s.name:
            out.name = s.name
        return out

    @classmethod
    def from_numpy(cls, a: npt.NDArray[np.generic]) -> Series:
        """Import a NumPy array with no pyarrow: a 1-D C-contiguous fixed-width
        numeric array borrows the array's buffer; other numbers, bools,
        ``datetime64``, ``timedelta64``, text, bytes and object arrays are
        copied. A NaN stays a NaN; a ``NaT`` or a masked value is null."""
        if not type(a).__module__.startswith("numpy.ma"):
            try:
                return _ext._series_from_numpy(a)
            except (TypeError, BufferError, RuntimeError, ValueError):
                pass
        return numpy_to_native(a)

    @classmethod
    def from_list(
        cls, values: Sequence[object], dtype: DType | int | builtins.str | pa.DataType | None = None
    ) -> Series:
        """Import a Python sequence with no pyarrow: bool, int, float, str,
        bytes, ``datetime.datetime`` (naive or aware), ``date``, ``time``,
        ``timedelta`` and ``decimal.Decimal`` values, ``None`` for null. NumPy
        scalars and arrays read as their Python values, and a list of only
        ``datetime64`` (or only ``timedelta64``) keeps the NumPy unit. Int
        columns are int64 (uint64 when too large), decimals decimal128 with the
        widest scale, times and datetimes microseconds. ``dtype`` takes any
        spelling :meth:`cast` takes, or a pyarrow integer, float, bool, string,
        binary, date32, timestamp or duration type (``TypeError`` for any other);
        it also types an empty or all-null list. A value the integer ``dtype``
        cannot hold raises ``OverflowError``, as does a float with a fraction.
        With no type, an empty or all-null list is a ``string`` column of nulls,
        the type :meth:`DataFrame.from_pandas` gives a column of ``None``. Lists
        and tuples become list columns and dicts struct columns (keys in
        first-seen order, a missing key null), to any depth."""
        values = list(values)
        kind_args: tuple[int, ...] = ()
        if dtype is not None:
            kind, unit, zone = _list_dtype(dtype)
            if unit:
                return temporal_from_values(values, kind, unit, zone)
            kind_args = (int(kind),)
        elif all(v is None for v in values):
            kind_args = (int(DType.STRING),)
        try:
            return _ext._series_from_list(values, *kind_args)
        except TypeError:
            spans = _numpy_spans(values)
            if spans is not None and not kind_args:
                return spans
            plain = _python_scalars(values)
            if plain is values:
                raise
            return _ext._series_from_list(plain, *kind_args)

    def to_arrow(self) -> pa.Array:
        """This column as a ``pyarrow.Array`` (zero-copy via the C Data
        Interface)."""
        return _require_pyarrow().array(self)

    def to_pandas(self, *, arrow: bool = False, nullable: bool = False) -> pd.Series:
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
        converts as without the option. A dictionary column reads its native
        codes, so no string is built per row, and keeps the dictionary order of
        its used values; a column of dictionary chunks (a batched collect) is
        one category too, its codes joined (a copy). The pandas Series takes this name and the row
        labels as its index (a ``MultiIndex`` for several levels; an unnamed level
        has the name ``None``)."""
        if arrow and nullable:
            raise ValueError("to_pandas: pass either arrow=True or nullable=True, not both")
        if arrow:
            out = arrow_pandas(self)
        else:
            import pandas as pd

            values = _pandas_nullable(self) if nullable else None
            if values is None:
                values = _pandas_default(self)
            # one copy, so the result is pandas' own and writable
            out = values if isinstance(values, pd.Series) else pd.Series(values, copy=True)
        out.name = self.name
        if names_of(self):
            out.index = pandas_index(self)
        return out

    def to_numpy(self) -> npt.NDArray[np.generic]:
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

    def to_polars(self) -> pl.Series:
        """This column as a polars Series. Where the extension has Arrow
        support, every column but a Float16 or decimal one comes through the
        Arrow PyCapsule interface (polars >= 1.3; an older polars raises its own
        error), without pyarrow; those, and builds without Arrow, use the
        native conversion (Float16 becomes Float32, a decimal gets precision
        38). A dictionary column becomes ``Categorical``. The polars Series
        takes this name, ``""`` when there is none."""
        from ._polars_frame import _require_polars

        pl = _require_polars()
        if _polars_via_arrow(self):
            out = pl.Series(self)
        else:
            out = _polars_native(self, pl)
        return out.alias(self.name or "")

    def __array__(
        self, dtype: np.dtype[np.generic] | None = None, copy: bool | None = None
    ) -> npt.NDArray[np.generic]:
        """NumPy array protocol, so ``np.asarray(series)`` works."""
        import numpy as np

        return np.asarray(self.to_numpy(), dtype=dtype)

    def __repr__(self) -> builtins.str:
        from ._repr import series_repr

        return series_repr(self)

    # builtins.slice (not bare `slice`): ty otherwise binds it to Series.slice.
    @overload
    def __getitem__(self, key: int) -> T_co | None: ...
    @overload
    def __getitem__(self, key: builtins.slice) -> Self: ...
    @overload
    def __getitem__(self, key: Series[bool]) -> Self: ...
    @overload
    def __getitem__(self, key: builtins.list[object] | tuple[object, ...]) -> Self: ...
    def __getitem__(
        self, key: int | builtins.slice | Series[bool] | builtins.list[object] | tuple[object, ...]
    ) -> Self | T_co | None:
        """``s[label]`` is ``loc``: on a Series with row labels the label's value
        (or the rows of a repeated label), on one without them the element at
        that position. ``s[a:b]`` with integers (or none) is positional, any
        other bounds are labels; ``s[[l1, l2]]`` takes labels as ``loc`` does and
        ``s[mask]`` keeps the rows where the Bool Series ``mask`` is true (a null
        is false). As pandas 3, an integer is never a position on a Series with
        labels (pandas 2 fell back to the position when the labels are not
        integers)."""
        if type(key) is int:
            try:
                return super()._item_unlabelled(key + len(self) if key < 0 else key)
            except NotImplementedError:
                return cast("Self | T_co | None", self.loc[key])
        if isinstance(key, Series):
            if key.dtype != DType.BOOL:
                raise TypeError("a Series key must be a Bool Series")
            return self.filter(key)
        if isinstance(key, slice):
            by_position = all(
                b is None or (isinstance(b, int) and not isinstance(b, bool))
                for b in (key.start, key.stop)
            )
            if not by_position:
                return cast("Self | T_co | None", self.loc[key])
            start, stop, step = key.indices(len(self))
            if step != 1:
                raise ValueError("Series slicing supports step 1 only")
            stop = max(stop, start)
            try:
                return self.slice(start, stop - start)
            except RuntimeError:
                return super().take(list(range(start, stop)))
        return cast("Self | T_co | None", self.loc[key])

    def __setitem__(self, key: object, value: object) -> None:
        """``s[label] = value`` adds a row for a label the Series does not
        have (pandas' setting with enlargement); a label it has is a
        ``TypeError``, as a Series is immutable. Two limits differ from
        pandas: it raises ``BufferError`` while a numpy array or memoryview
        over the Series is alive (the enlarged column cannot move under it),
        and it raises ``TypeError`` for a value of another type than the
        Series holds, where pandas upcasts."""
        from ._loc import set_series_label

        set_series_label(self, key, value)

    def item(self, i: int | None = None) -> T_co | None:
        """The element at row ``i``; with no ``i`` the only element of a
        one-element Series (pandas ``item()``)."""
        if i is None:
            if len(self) != 1:
                raise ValueError(f"item: the Series has {len(self)} elements, not one")
            i = 0
        return super().item(i)

    def rfloordiv(self, other: _Scalar) -> Series[Scalar]:
        return other // self if isinstance(other, Series) else self.__rfloordiv__(other)

    def rmod(self, other: _Scalar) -> Series[Scalar]:
        return other % self if isinstance(other, Series) else self.__rmod__(other)

    def rpow(self, other: _Scalar) -> Series[Scalar]:
        return other**self if isinstance(other, Series) else self.__rpow__(other)

    def divmod(self, other: _SeriesOrScalar) -> tuple[Series[Scalar], Series[Scalar]]:
        return self // other, self % other

    def rdivmod(self, other: _Scalar) -> tuple[Series[Scalar], Series[Scalar]]:
        return other // self, other % self

    # Comparisons return a boolean mask Series, ``==`` and ``!=`` included,
    # as pandas and polars; a Series is therefore unhashable, as theirs are.
    # A string scalar compares the text of a String column.
    if TYPE_CHECKING:

        def __hash__(self) -> NoReturn: ...
    else:
        __hash__ = None

    def gt(self, value: _Scalar) -> Series[bool]:
        """Boolean mask where the value is greater than ``value``."""
        return self > value

    def ge(self, value: _Scalar) -> Series[bool]:
        """Boolean mask where the value is greater than or equal to ``value``."""
        return self >= value

    def lt(self, value: _Scalar) -> Series[bool]:
        """Boolean mask where the value is less than ``value``."""
        return self < value

    def le(self, value: _Scalar) -> Series[bool]:
        """Boolean mask where the value is less than or equal to ``value``."""
        return self <= value

    def eq(self, value: _Scalar) -> Series[bool]:
        """Boolean mask where the value equals ``value``."""
        return self == value

    def ne(self, value: _Scalar) -> Series[bool]:
        """Boolean mask where the value does not equal ``value``."""
        return self != value

    # -- native engine ops (SIMD kernels, wrapped from the C extension) --------

    def truediv(self, other: Series | int | float) -> Series[float]:
        """True division (the polars name; ``div`` and ``divide`` are the
        pandas ones): integer operands widen to Float64 first. The engine's
        integer-truncating kernel is ``ops.series.div``."""
        return self / other

    def add(self, other: _SeriesOrScalar) -> Series:
        """``self + other``; ``other`` is a Series or a scalar."""
        return self + other

    def sub(self, other: _SeriesOrScalar) -> Series:
        """``self - other``; ``other`` is a Series or a scalar."""
        return self - other

    def mul(self, other: _SeriesOrScalar) -> Series:
        """``self * other``; ``other`` is a Series or a scalar."""
        return self * other

    def apply(self, func: Callable[[T_co], _R]) -> Series[_R]:
        """``func`` over each element (pandas ``Series.apply``), engine-first:
        the function is traced once with a symbolic column and, when it builds
        a column expression, runs fused in the engine; a ``@jit.series`` or
        ``@jit.op`` function runs through its own path; anything else runs in
        Python per element with a warning naming why. See ``_apply``."""
        from ._apply import apply_series

        return cast("Series[_R]", apply_series(self, func, "Series.apply"))

    @overload
    def map(self, func: Mapping[_K, _R]) -> Series[_R]: ...
    @overload
    def map(self, func: Callable[[T_co], _R]) -> Series[_R]: ...
    def map(self, func: Callable[[T_co], _R] | Mapping[_K, _R]) -> Series[_R]:
        """``func`` over each element; the pandas ``Series.map`` spelling of
        :meth:`apply`. A mapping maps each value to its entry, null where it
        has none, as pandas; the lookup is a native left join, not a Python
        loop. A key matches a value only when they are equal (``1.5`` never
        matches ``1``); a ``None`` or NaN key maps the null and NaN rows. A
        dict subclass with ``__missing__``, such as ``defaultdict``, is looked
        up per element so its default applies, as pandas."""
        if isinstance(func, Mapping):
            if hasattr(type(func), "__missing__"):
                # Its default is a Python call per key, null rows included.
                lookup = cast("Mapping[object, _R]", func)
                out = Series.from_list([lookup[v] for v in self.to_list()]).rename(self.name)
                return cast("Series[_R]", out if not names_of(self) else out.set_axis(self.index))
            return cast("Series[_R]", _map_values(self, func))
        from ._apply import apply_series

        return cast("Series[_R]", apply_series(self, func, "Series.map"))

    @overload
    def cast(
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
    ) -> Series[int]: ...
    @overload
    def cast(self, dtype: Literal[DType.FLOAT32, DType.FLOAT64]) -> Series[float]: ...
    @overload
    def cast(self, dtype: Literal[DType.BOOL]) -> Series[bool]: ...
    @overload
    def cast(self, dtype: Literal[DType.STRING, DType.LARGE_STRING]) -> Series[builtins.str]: ...
    @overload
    def cast(self, dtype: builtins.type[bool]) -> Series[bool]: ...
    @overload
    def cast(self, dtype: builtins.type[int]) -> Series[int]: ...
    @overload
    def cast(self, dtype: builtins.type[float]) -> Series[float]: ...
    @overload
    def cast(self, dtype: builtins.type[builtins.str]) -> Series[builtins.str]: ...
    @overload
    def cast(self, dtype: builtins.str | int | DType | builtins.type) -> Series[Scalar]: ...
    def cast(self, dtype: builtins.str | int | DType | builtins.type) -> Series[Scalar]:
        """Cast to ``dtype``: a :class:`~dftracer.utils.DType`, its
        int code, a dtype name (``"int64"``, ``"Float64"``, case-insensitive;
        matches the ``DType`` member names), a Python type (``str``, ``int``,
        ``float``, ``bool``) or a NumPy scalar type or dtype. Integers, floats
        and bools cast to string (floats as the shortest text that shows a point
        or exponent, bools as ``True`` / ``False`` as Python and pandas print
        them; the native cast under it and the C++ and C APIs spell them
        ``true`` / ``false``); integers and floats cast to
        bool (zero false, a NaN or null null). A pair the engine does not
        support raises ``TypeError`` naming both types. ``astype`` is the pandas
        name."""
        target = _resolve_dtype(dtype)
        out = super()._cast(target)
        if int(target) == int(DType.STRING) and int(self.dtype) == int(DType.BOOL):
            out = _StrAccessor(
                out
            ).capitalize()  # the engine says true/false; Python says True/False
        return out

    def compare_series(self, other: Series, op: int) -> Series[bool]:
        """Row by row against ``other`` (``op`` a ``_CMP_*`` code): a Bool
        mask, null where either side is null; the engine's
        ``dftu.series.compare_series``. ``==`` / ``<`` / ... spell it."""
        return cast("Series[bool]", _ext.op_run("dftu.series.compare_series", self, other, op))

    def compare(
        self, other: Series, keep_shape: bool = False, keep_equal: bool = False
    ) -> DataFrame:
        """The rows where this Series and ``other`` differ, as a frame with
        ``self`` and ``other`` columns under the row labels (the positions when
        there are none), as pandas ``Series.compare``. ``keep_shape`` keeps every row; ``keep_equal``
        keeps the equal values instead of nulling them. The engine's
        ``compare(op, scalar)`` kernel is ``s.ops.series.compare``."""
        from ._dataframe import DataFrame

        if not isinstance(other, Series):
            raise TypeError("compare() expects a Series; the scalar kernel is s.ops.series.compare")
        if len(self) != len(other):
            raise ValueError("compare() needs two Series of the same length")
        labelled = positional(self)
        names = names_of(labelled)
        frame = DataFrame({"self": drop_levels(self), "other": drop_levels(other)})
        for name, level in zip(names, levels_of(labelled)):
            frame = frame.with_column(name, level)
        frame = frame.set_index(names)
        mask = differs(self, other)
        if not keep_shape:
            frame = frame.filter(mask)
        left, right = frame["self"], frame["other"]
        if keep_shape and not keep_equal:
            left, right = left.where(mask), right.where(mask)
        return frame.with_column("self", left).with_column("other", right)

    def __bool__(self) -> NoReturn:
        raise ValueError(
            "The truth value of a Series is ambiguous. Use a.empty, a.item(), a.any() or a.all()."
        )

    def __abs__(self) -> Self:
        return self.abs()

    def __and__(self, other: Series) -> Series[bool]:
        return self.logical(_LOGICAL_AND, other)

    def __or__(self, other: Series) -> Series[bool]:
        return self.logical(_LOGICAL_OR, other)

    def __invert__(self) -> Series[bool]:
        return self.logical_not()

    def clear(self, n: int = 0) -> Self:
        """A Series of this name and type with no rows (``n=0``) or with
        ``n`` null rows (polars ``clear``); this Series is not changed."""
        if n < 0:
            raise ValueError("clear: n must not be negative")
        if n == 0:
            return self.head(0)
        return self.take([-1] * n)

    def radians(self) -> Series[float]:
        """Each value, in degrees, as radians (Float64; polars ``radians``)."""
        return cast("Series[float]", self.astype("float64") * (math.pi / 180.0))

    def degrees(self) -> Series[float]:
        """Each value, in radians, as degrees (Float64; polars ``degrees``)."""
        return cast("Series[float]", self.astype("float64") * (180.0 / math.pi))

    def value_counts(
        self,
        normalize: bool = False,
        sort: bool = True,
        ascending: bool = False,
        bins: int | Sequence[float] | None = None,
        dropna: bool = True,
    ) -> Series:
        """The count of each distinct value as a Series named ``count``,
        labelled by the values (the pandas ``value_counts``). ``normalize``
        gives fractions, named ``proportion``; ``sort=False`` keeps the order
        of first appearance and ``ascending`` flips the count order, ties
        keeping that order. ``dropna=False`` counts the nulls and NaNs as one
        missing value. ``bins`` counts the values per interval of
        ``pandas.cut`` (an integer count of equal-width bins, or ascending
        edges), every interval listed, labelled with its text such as
        ``"(0.999, 2.0]"``."""
        return series_value_counts(self, normalize, sort, ascending, bins, dropna)

    def clip(
        self,
        lower: int | float | None = None,
        upper: int | float | None = None,
        *,
        lower_bound: int | float | None = None,
        upper_bound: int | float | None = None,
    ) -> Self:
        """Clamp to ``[lower, upper]`` (``lower_bound`` / ``upper_bound``, the
        polars keywords, name the same two bounds). One bound is enough:
        ``clip(lower=0)`` raises values below 0 to 0 and leaves the rest,
        ``clip(upper=b)`` is the mirror. A null stays null. With no bound it
        raises ``TypeError``."""
        lo = _first_set(lower, lower_bound)
        hi = _first_set(upper, upper_bound)
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
        if decimals == 0 or (decimals > 0 and self.dtype.is_integer):
            return super().round()
        scale = 10.0**decimals if decimals > 0 else 10.0 ** (-decimals)
        if decimals > 0:
            out = (self * scale).round() / scale
        else:
            out = (self / scale).round() * scale
        return cast("Self", out.astype(self.dtype) if self.dtype.is_integer else out)

    def rank(
        self,
        method: Literal["average", "min", "max", "dense", "ordinal", "first"] = "average",
        descending: bool = False,
        *,
        ascending: bool | None = None,
        pct: bool = False,
    ) -> Series[float]:
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

    def expanding(self) -> _Expanding:
        """The window from the first row to each row (pandas ``expanding``):
        ``.sum() .mean() .min() .max() .count() .var() .std()``, each from
        the cumulative kernels."""
        return _Expanding(self)

    @overload
    def cut(
        self,
        breaks: Series,
        right: bool = False,
        outer: bool = True,
        *,
        labels: Literal[False] | None = None,
        include_lowest: bool = False,
    ) -> Series[int]: ...
    @overload
    def cut(
        self,
        breaks: Series,
        right: bool = False,
        outer: bool = True,
        *,
        labels: Sequence[builtins.str],
        include_lowest: bool = False,
    ) -> Series[builtins.str]: ...
    def cut(
        self,
        breaks: Series,
        right: bool = False,
        outer: bool = True,
        *,
        labels: Sequence[builtins.str] | Literal[False] | None = None,
        include_lowest: bool = False,
    ) -> Series[int] | Series[builtins.str]:
        """Bin each value by the ascending ``breaks``. By default the intervals are
        ``[a, b)`` and the bin is the count of breaks at or below the value
        (0 to ``len(breaks)``). ``right=True`` closes each interval on the right,
        ``(a, b]``. ``outer=False`` gives null for a value outside the interior
        intervals and numbers them from 0; ``cut(breaks, right=True, outer=False)``
        equals ``pandas.cut(labels=False)``. A null value gives a null bin. Integer,
        Bool and temporal values with integer ``breaks`` compare exactly as 64-bit
        integers; with float values or float breaks the comparison is in doubles,
        as pandas does.

        ``labels`` names the interior intervals, one string each, as
        ``pandas.cut(labels=[...])``; with them (or ``labels=False``) ``outer`` is
        off. ``include_lowest`` puts a value equal to the first break in the
        first interval when ``right`` closes the intervals on the right."""
        if labels is not None:
            outer = False
            if isinstance(labels, bool) and labels:
                raise ValueError("cut: labels must be False, None or a sequence of names")
            if labels is not False and len(labels) != len(breaks) - 1:
                raise ValueError("Bin labels must be one fewer than the number of bin edges")
        flags = (1 if right else 0) | (0 if outer else 2)
        bins: Series[int] = super().cut(breaks, flags)
        if include_lowest and right and len(breaks):
            lowest = self.eq(cast("_Scalar", breaks.item(0))).fill_null(False)
            bins = cast("Series[int]", bins.mask(lowest, bins.full_like(0 if not outer else 1)))
        if labels is None or labels is False:
            return bins
        return bins.map(dict(enumerate(labels)))

    def where(self, cond: Series, other: Series | int | float | None = None) -> Series[Scalar]:
        """This value where ``cond`` is true, else ``other`` (pandas
        ``Series.where``; a scalar ``other`` broadcasts, a float one widens
        the result to Float64; a null ``other`` (``None`` or ``pd.NA``, the
        default) makes every row where ``cond`` is false null, in this
        column's own type; a null in ``cond`` takes ``other``)."""
        a, b = self._arms(other)
        return _ext.Series.where(a, cond, b)

    def mask(self, cond: Series, other: Series | int | float | None = None) -> Series[Scalar]:
        """``other`` where ``cond`` is true (null by default), else this value
        (pandas ``Series.mask``)."""
        a, b = self._arms(other)
        return _ext.Series.where(b, cond, a)

    def _arms(self, other: Series | int | float | None) -> tuple[Series[Scalar], Series[Scalar]]:
        if isinstance(other, Series):
            return self, other
        if is_na(other):
            return self, _ext._series_nulls(int(self.dtype), len(self))
        if isinstance(other, bool) or not isinstance(other, (int, float)):
            raise TypeError("other must be a Series, a number or null")
        me: Series[Scalar] = self
        if isinstance(other, float) and self.dtype not in (DType.FLOAT32, DType.FLOAT64):
            me = self.astype(DType.FLOAT64)
        return me, me.full_like(other)

    def full_like(self, value: int | float) -> Series[Scalar]:
        """A Series of this length holding ``value`` on every row, in this
        Series' numeric type (Float64 for a float value, Int64 when this
        Series is not numeric). The constant rides the null mask (0 / 1,
        never NaN or null), so no cell of this Series leaks into it."""
        target = self.dtype
        if isinstance(value, float) or not target.is_numeric:
            target = DType.FLOAT64 if isinstance(value, float) else DType.INT64
        return self.is_null().cast(target).mul_scalar(0).add_scalar(value)

    def mode(self) -> Series[Scalar]:
        """The most frequent value as a one-element Series (pandas returns every
        mode; the engine keeps one)."""
        return Series([super().mode()])

    def gather(self, indices: Series | Sequence[int]) -> Self:
        """The rows at ``indices`` (a Series or a list; any order, repeats
        allowed); ``take`` is the pandas name. An index past the last row is an
        ``IndexError``. A negative index gives a null, it does not count from
        the end as in pandas."""
        return super().take(_row_indices(indices))

    @property
    def is_unique(self) -> bool:
        """Whether every value occurs once (the pandas property). The per-row
        mask is :meth:`unique_mask`."""
        return not self.is_duplicated().any()

    unique_mask = _ext.Series.is_unique

    def has_nulls(self) -> bool:
        """Whether any value is null (polars ``has_nulls``; ``hasnans`` is the
        pandas property)."""
        return self.null_count > 0

    def is_empty(self) -> bool:
        """Whether the Series has no elements (polars ``is_empty``)."""
        return len(self) == 0

    def first(self) -> T_co | None:
        """The first element, ``None`` when the Series is empty (polars
        ``first``)."""
        return self.item(0) if len(self) else None

    def last(self) -> T_co | None:
        """The last element, ``None`` when the Series is empty (polars
        ``last``)."""
        return self[len(self) - 1] if len(self) else None

    def is_not_nan(self) -> Series[bool]:
        """Bool mask of the values that are not NaN (a null stays null; polars
        ``is_not_nan``)."""
        return self.is_nan().logical_not()

    def is_in(self, values: Series | Sequence[object]) -> Series[bool]:
        """Bool mask: the value is one of ``values`` (a Series or a list; an
        empty list matches nothing)."""
        if not isinstance(values, Series):
            items = list(values)
            if not items:
                # Present and null at once: false on every row.
                return self.is_null().logical(0, self.is_not_null())
            values = Series.from_list(items)
        return super().is_in(values)

    def shift(self, n: int = 1, *, periods: int | None = None) -> Self:
        """Shift by ``n`` rows (``periods`` is the pandas spelling)."""
        if periods is not None:
            n = periods
        return super().shift(n)

    def diff(self, n: int = 1, *, periods: int | None = None) -> Self:
        """Each value minus the one ``n`` rows before it (after it for a
        negative ``n``); the first ``n`` rows are null. ``periods`` is the
        pandas spelling."""
        if periods is not None:
            n = periods
        if n == 1:
            return super().diff()
        return cast("Self", self - self.shift(n))

    def sample(self, n: int, seed: int = 0, *, random_state: int | None = None) -> Self:
        """A deterministic ``n``-row sample (``random_state`` is the pandas
        spelling of ``seed``)."""
        if random_state is not None:
            seed = random_state
        return super().sample(n, seed)

    @property
    def ops(self: Series[Scalar]) -> _OpsAccessor:
        """Call any registered op (built-in or @jit.series user op) as a method,
        e.g. ``s.ops.series.add(other)``, ``s.ops.series.count()``, ``s.ops.stats.zscore()``."""
        return _OpsAccessor(self)

    # -- pandas / polars spellings ---------------------------------------------
    # Thin aliases over the native methods above, so either library's muscle
    # memory works on the same object. pandas names are primary; a polars name
    # that differs stays alongside.
    @property
    def dtype(self) -> DType:
        """The element type as a :class:`~dftracer.utils.DType`."""
        return DType(self.type)

    @property
    def size(self) -> int:
        return self.length

    @property
    def shape(self) -> tuple[int]:
        return (self.length,)

    def len(self) -> int:
        return self.length

    def to_list(self) -> builtins.list[T_co | None]:
        values = self._to_pylist()
        if self.is_json:
            return [None if v is None else json.loads(cast(str, v)) for v in values]
        return values

    def var(self, ddof: int = 1) -> float | None:
        """Variance with ``ddof`` degrees of freedom (0 = population, 1 =
        sample, the pandas default); the engine has kernels for those two.
        None for a column with no present value; NaN below ``ddof + 1``
        present values, as pandas."""
        return self.variance(_ddof_sample(ddof))

    def std(self, ddof: int = 1) -> float | None:
        return self.stddev(_ddof_sample(ddof))

    def skew(self, bias: bool = False) -> float | None:
        """The pandas ``skew`` (unbiased; NaN below three values and for a
        constant column, as pandas 3) by default; ``bias=True`` is the polars population
        skewness (NaN for a constant column, null for no values)."""
        if bias:
            return _population(self, self.count(), _ext.Series.skew(self))
        return _ext.Series.skew_unbiased(self)

    def kurtosis(self, fisher: bool = True, bias: bool = True) -> float | None:
        """The polars ``kurtosis``, with the polars defaults: the population
        excess kurtosis (NaN for one value or a constant column, null for no
        values); ``fisher=False`` adds 3, ``bias=False`` is the unbiased
        estimator (null below four values). ``kurt`` is the pandas name and
        gives the pandas estimator."""
        n = self.count()
        pop = _ext.Series.kurtosis(self)
        if bias:
            value = _population(self, n, pop)
        elif n < 4:
            return None
        else:
            value = _ext.Series.kurt_unbiased(self)
        if value is not None and not fisher:
            value += 3.0
        return value

    def kurt(self) -> float:
        """The pandas ``kurt``: unbiased excess kurtosis, NaN below four
        values and for a constant column (pandas 3)."""
        return _ext.Series.kurt_unbiased(self)

    def sort_values(self, ascending: bool = True, na_position: builtins.str = "last") -> Self:
        """The pandas ``sort_values``: :meth:`sort` with ``ascending`` and
        ``na_position`` (nulls last by default, unlike :meth:`sort`). The rows
        keep their labels; a positional Series is labelled by the positions, as
        pandas' index shows where each row came from."""
        if na_position not in ("first", "last"):
            raise ValueError("na_position must be 'first' or 'last'")
        return positional(self).sort(descending=not ascending, nulls_last=na_position == "last")

    def nlargest(self, n: int = 5, keep: builtins.str = "first") -> Self:
        """The pandas ``nlargest``: :meth:`top_k` with ``n``, labelled as
        :meth:`sort_values` is. ``keep`` settles a tie at the ``n``-th value:
        ``"first"`` keeps the earlier rows, ``"last"`` the later ones and
        ``"all"`` every row that ties (more than ``n`` rows)."""
        return self._extreme(n, keep, True)

    def nsmallest(self, n: int = 5, keep: builtins.str = "first") -> Self:
        """The pandas ``nsmallest``: :meth:`bottom_k` with ``n``, labelled as
        :meth:`sort_values` is, and ``keep`` as :meth:`nlargest`."""
        return self._extreme(n, keep, False)

    def _extreme(self, n: int, keep: builtins.str, largest: bool) -> Self:
        if keep not in ("first", "last", "all"):
            raise ValueError('keep must be either "first", "last" or "all"')
        s = positional(self)
        if keep == "last":
            s = s.reverse()
        best = s.top_k(n) if largest else s.bottom_k(n)
        if keep != "all" or n <= 0 or len(best) < n:
            return best
        edge = cast("float", best.item(len(best) - 1))
        tied = s.filter(s.ge(edge) if largest else s.le(edge))
        return tied.sort(descending=largest, nulls_last=True)

    def tail(self, n: int = 5) -> Self:
        """The last ``n`` rows (all but the first ``-n`` for a negative ``n``).
        The rows keep their labels; a positional Series is labelled by the
        positions it kept, as pandas."""
        out = super().tail(n if n >= 0 else max(len(self) + n, 0))
        if names_of(self):
            return out
        return with_levels(out, [default_name(0)], [positions(len(out), len(self) - len(out))])

    def dropna(self) -> Self:
        """The pandas ``dropna``: :meth:`drop_nulls` then :meth:`drop_nans`,
        so a NaN of a float column goes with the nulls. The rows keep their
        labels; a positional Series that loses a row is labelled by the
        positions it kept, as pandas."""
        if self.null_count == 0 and not (self.dtype.is_float and self.is_nan().any()):
            return self
        return positional(self).drop_nulls().drop_nans()

    def fillna(self, value: bool | int | float | builtins.str) -> Self:
        """The pandas ``fillna``: a null and, in a float column, a NaN become
        ``value`` (:meth:`fill_null` fills only the nulls, :meth:`fill_nan`
        only the NaNs)."""
        if isinstance(value, builtins.str):
            return self.fill_null(value)
        return self.fill_nan(value).fill_null(value)

    def take(self, indices: Sequence[int] | Series) -> Self:
        """The pandas ``take``: :meth:`gather`. The rows keep their labels; a
        positional Series is labelled by the positions taken."""
        if names_of(self):
            return self.gather(indices)
        out = self.gather(indices)
        rows = _row_indices(indices)
        label = Series.from_list(rows) if isinstance(rows, list) else drop_levels(rows)
        return with_levels(out, [default_name(0)], [label])

    # ``s.str``: string methods under both spellings, pandas (``s.str.lower()``,
    # ``s.str.contains("x")``, ``s.str.len()``) and polars
    # (``s.str.to_lowercase()``, ``s.str.len_bytes()``). The class-scope names
    # ``str`` and ``list`` shadow the builtins in this class's annotations, so
    # they read ``builtins.str`` and ``builtins.list`` here.
    @property
    def str(self: Series[builtins.str]) -> _StrAccessor:
        return _StrAccessor(self)

    @property
    def list(self: Series[Scalar]) -> _ListAccessor:
        return _ListAccessor(self)

    @property
    def dt(self: Series[Scalar]) -> _DtAccessor:
        return _DtAccessor(self)

    def __reduce__(self) -> tuple[Callable[..., Series], tuple[object, ...]]:
        names = names_of(self)
        if not names:
            return (_series_from_bytes, (self.to_bytes(),))
        levels = [level.to_bytes() for level in levels_of(self)]
        return (_series_from_bytes, (drop_levels(self).to_bytes(), names, levels))

    # BEGIN GENERATED aliases from _aliases.py by scripts/gen_pyi.py
    if TYPE_CHECKING:
        aggregate = SeriesPandasMixin.agg
        astype = cast
        isnull = _ext.Series.isna
        notnull = _ext.Series.notna
        isin = is_in
        nunique = _ext.Series.n_unique
        prod = _ext.Series.product
        argmax = _ext.Series.arg_max
        idxmax = _ext.Series.arg_max
        argmin = _ext.Series.arg_min
        idxmin = _ext.Series.arg_min
        cumsum = _ext.Series.cum_sum
        cumprod = _ext.Series.cum_prod
        cummax = _ext.Series.cum_max
        cummin = _ext.Series.cum_min
        searchsorted = _ext.Series.search_sorted
        tolist = to_list
        ffill = _ext.Series.forward_fill
        pad = _ext.Series.forward_fill
        bfill = _ext.Series.backward_fill
        backfill = _ext.Series.backward_fill
        iat = SeriesPandasMixin.iloc
        rtruediv = SeriesPandasMixin.rdiv
        divide = truediv
        div = truediv
        multiply = mul
        subtract = sub
        alias = _ext.Series.rename
        clone = SeriesPandasMixin.copy
        limit = _ext.Series.head
        not_ = __invert__
        pass
    # END GENERATED aliases


def _is_constant(s: Series) -> bool:
    return s.min() == s.max()


def _population(s: Series, n: int, value: float) -> float | None:
    """The polars population moment: null with no values, NaN for one value or
    a constant column."""
    if n == 0:
        return None
    if n == 1 or _is_constant(s):
        return float("nan")
    return value


def _first_set(*values: int | float | None) -> int | float | None:
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

    def count(self) -> Series[int]:
        return self._series.is_not_null().astype(DType.INT64).cumsum()

    def mean(self) -> Series[float]:
        return self.sum().astype("float64") / self.count()

    def var(self) -> Series[float]:
        s = self._series.astype("float64")
        n = self.count()
        total = s.cumsum().ffill()
        squares = (s * s).cumsum().ffill()
        # (S2 - S^2 / n) / (n - 1); a null divisor where n < 2.
        divisor = n.sub_scalar(1).where(n.ge(2), n.shift(len(n)))
        return (squares - total * total / n) / divisor

    def std(self) -> Series[float]:
        return self.var().sqrt()


_Temporal = Union[datetime.datetime, datetime.date, datetime.time, datetime.timedelta]


_dt_accessor_new = _ext.DtAccessor.__new__


class _DtAccessor(_ext.DtAccessor):
    """``s.dt``: the calendar parts of a Timestamp / Date / Duration Series
    in its own unit, or of an Int64 Series read as microseconds
    (``s.dt("ns")`` picks another unit). A Timestamp column with a zone gives
    the parts of its wall time in that zone; a naive column, an Int64 column
    and a UTC one give the UTC parts."""

    __slots__ = ("_unit", "_given")
    _unit: str
    _given: Optional[str]

    def __new__(cls, series: Series, unit: str | None = None) -> _DtAccessor:
        self = _dt_accessor_new(cls, series)
        self._given = unit
        unit = dt_unit(series.type, series.time_unit, unit)
        if unit not in DT_UNIT_CODES:
            raise ValueError("dt: unit must be s, ms, us or ns")
        self._unit = unit
        return self

    def __call__(self, unit: str) -> _DtAccessor:
        return _DtAccessor(self._s, unit)

    def _part(self, code: int) -> Series:
        return self._s.dt_part(code, DT_UNIT_CODES[self._unit])

    def strftime(self, fmt: str) -> Series:
        """Format each instant (pandas ``strftime``) for the directives %Y %y
        %m %d %H %I %M %S %f %j %a %A %b %B %p %F %T %s %z %Z %%; another
        directive raises ValueError. A column with a zone prints its wall
        time, %z as the offset and %Z as the abbreviation. Unlike pandas on a
        naive column, %z prints ``+0000`` and %Z prints ``UTC`` rather than
        empty."""
        return self._s.dt_format(fmt, DT_UNIT_CODES[self._unit])

    def day_name(self) -> Series:
        """The English weekday name of each instant (pandas ``day_name``)."""
        return self.strftime("%A")

    def month_name(self) -> Series:
        """The English month name of each instant (pandas ``month_name``)."""
        return self.strftime("%B")

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

    def ordinal_day(self) -> Series:
        """The day of the year, from 1 (the polars name; ``dayofyear`` is the
        pandas property)."""
        return self._part(10)

    def iso_year(self) -> Series:
        """The ISO 8601 year of the week the instant falls in."""
        return self._part(15)

    def week(self) -> Series:
        """The ISO 8601 week number, from 1."""
        return self._part(14)

    def epoch(self, time_unit: str = "us") -> Series[int]:
        """Each instant as an integer count of ``time_unit`` (``"ns"``,
        ``"us"``, ``"ms"``, ``"s"`` or ``"d"``) since the epoch, from the
        column's own unit; a coarser unit floors."""
        from ._expr import ColumnOp
        from ._expr_ops import run_column_op

        if time_unit not in EPOCH_NS:
            raise ValueError("epoch: time_unit must be ns, us, ms, s or d")
        spec = ColumnOp(
            "dftu.series.cast", None, 0, 0, time_unit, None, "epoch", 0, None, self._given
        )
        return cast("Series[int]", run_column_op(self._s, spec, None))

    def isocalendar(self) -> DataFrame:
        """A frame of ISO ``year``, ``week`` and ``day`` (Monday = 1)."""
        from ._dataframe import DataFrame

        return DataFrame.from_dict(
            {
                "year": self._part(15),
                "week": self._part(14),
                "day": self.dayofweek + 1,
            }
        )

    def _every(self, freq: int | str) -> int:
        return rule_to_units(freq, self._unit)

    def floor(self, freq: int | str) -> Series:
        """Each instant floored to a multiple of ``freq`` (``"5min"``, ``"1h"``,
        ``"D"``, or a count of the column's units); ``ceil`` and ``round``
        (half to even) likewise."""
        return self._s.dt_round(self._every(freq), 0)

    def truncate(self, every: int | str) -> Series:
        """As :meth:`floor` (the polars name)."""
        return self.floor(every)

    def ceil(self, freq: int | str) -> Series:
        return self._s.dt_round(self._every(freq), 1)

    def round(self, freq: int | str) -> Series:
        return self._s.dt_round(self._every(freq), 2)

    def normalize(self) -> Series:
        """Each instant at midnight of its day."""
        return self.floor("1d")

    @staticmethod
    def _unit_code(name: str, unit: object) -> int:
        code = UNIT_CODES.get(unit) if isinstance(unit, str) else None
        if code is None:
            raise ValueError(f"{name}: unit must be one of s, ms, us or ns, not {unit!r}")
        return code

    def as_unit(self, unit: str) -> Series:
        """The column in ``unit`` (``"s"``, ``"ms"``, ``"us"`` or ``"ns"``),
        the instant kept: a coarser unit floors toward negative infinity, as
        pandas ``as_unit``. A Timestamp keeps its zone; a Duration works too.
        A value that does not fit 64 bits in a finer unit raises
        ``DFTUtilsValueError`` (pandas ``OutOfBoundsDatetime``), never wraps."""
        return self._s.dt_as_unit(self._unit_code("as_unit", unit), 0)

    def cast_time_unit(self, time_unit: str) -> Series:
        """:meth:`as_unit` under the polars name and rule: a Timestamp floors
        and a Duration truncates toward zero, where ``as_unit`` floors both.
        Polars has no second unit, so only ``"ns"``, ``"us"`` and ``"ms"`` are
        accepted."""
        if time_unit not in ("ns", "us", "ms"):
            raise ValueError(f"`time_unit` must be one of {{'ns', 'us', 'ms'}}, got {time_unit}")
        return self._s.dt_as_unit(self._unit_code("cast_time_unit", time_unit), 1)

    @property
    def date(self) -> Series:
        """The calendar day of each Timestamp as a Date32 column (pandas
        ``dt.date``); the day of the wall time in the column's zone."""
        return self._s.dt_date()

    @property
    def time(self) -> Series:
        """The time of day of each Timestamp, in the column's unit: a Time32
        column for seconds and milliseconds, a Time64 column for microseconds
        and nanoseconds (pandas ``dt.time``); the wall time in the column's
        zone."""
        return self._s.dt_time()

    @property
    def timetz(self) -> Series:
        """As :meth:`time`. The column types carry no zone for a time of day,
        so the result is the wall time of day without the tzinfo pandas
        attaches."""
        return self._s.dt_time()

    def total_seconds(self) -> Series:
        """Each value as seconds (Float64), for a Duration or an Int64 read
        in the accessor's unit."""
        per_second = 1_000_000_000 / FREQ_NS[self._unit]
        return self._s.astype("float64") / per_second

    def _total(self, ns_per_result: int) -> Series[int]:
        # Whole units, truncated toward zero as polars does, in integer
        # arithmetic from the column's ticks.
        ticks = self._s.astype("int64")
        unit_ns = FREQ_NS[self._unit]
        if ns_per_result <= unit_ns:
            return cast("Series[int]", ticks * (unit_ns // ns_per_result))
        step = ns_per_result // unit_ns
        floored = ticks.floordiv(step)
        return cast(
            "Series[int]", floored + ((ticks < 0) & (floored * step != ticks)).astype("int64")
        )

    def total_days(self) -> Series[int]:
        """Whole days of each Duration (truncated toward zero, Int64)."""
        return self._total(86_400_000_000_000)

    def total_hours(self) -> Series[int]:
        return self._total(3_600_000_000_000)

    def total_minutes(self) -> Series[int]:
        return self._total(60_000_000_000)

    def total_milliseconds(self) -> Series[int]:
        return self._total(1_000_000)

    def total_microseconds(self) -> Series[int]:
        return self._total(1_000)

    def total_nanoseconds(self) -> Series[int]:
        return self._total(1)

    def timestamp(self, time_unit: str = "us") -> Series[int]:
        """The polars name of :meth:`epoch`."""
        return self.epoch(time_unit)

    def to_string(self, format: str) -> Series:
        """The polars name of :meth:`strftime`."""
        return self.strftime(format)

    def min(self) -> _Temporal | None:
        """The earliest instant as a ``datetime`` (or the value ``to_list``
        gives for the column's type), None when no value is valid."""
        at = self._s.arg_min()
        return None if at < 0 else cast("_Temporal", self._s.gather([at]).to_list()[0])

    def max(self) -> _Temporal | None:
        """The latest instant, as :meth:`min`."""
        at = self._s.arg_max()
        return None if at < 0 else cast("_Temporal", self._s.gather([at]).to_list()[0])

    @property
    def tz(self) -> str | None:
        """The zone name of a Timestamp column, or None when naive."""
        return self._s.timezone or None

    def tz_localize(
        self,
        tz: str | None,
        ambiguous: str | Sequence[bool] = "raise",
        nonexistent: str | datetime.timedelta = "raise",
    ) -> Series:
        """A naive Timestamp column read as wall times in ``tz`` (pandas
        ``tz_localize``); ``None`` turns an aware column into its wall times.
        ``ambiguous`` is ``"raise"``, ``"infer"``, ``"NaT"`` or a boolean
        array (``True`` takes the first of the two instants); ``nonexistent``
        is ``"raise"``, ``"shift_forward"``, ``"shift_backward"``, ``"NaT"``
        or a ``timedelta``. ``shift_forward`` and ``shift_backward`` use the
        instant of the change, not pandas' whole-hour rounding."""
        from ._time_index import localize

        return localize(self._s, tz, ambiguous, nonexistent)

    def tz_convert(self, tz: str | None) -> Series:
        """The same instants shown in zone ``tz`` (pandas ``tz_convert``):
        the type's zone changes, no tick moves. Needs a tz-aware column;
        ``None`` gives the naive UTC column."""
        from ._time_index import convert

        return convert(self._s, tz)

    def replace_time_zone(
        self,
        time_zone: str | None,
        *,
        use_earliest: bool | None = None,
        ambiguous: str | Series = "raise",
        non_existent: str = "raise",
    ) -> Series:
        """The polars name of :meth:`tz_localize`: the wall times kept and the
        zone replaced (``None`` removes it). ``ambiguous`` is ``"raise"``,
        ``"earliest"``, ``"latest"`` or ``"null"``; ``non_existent`` is
        ``"raise"`` or ``"null"``."""
        from ._time_index import replace_zone

        return replace_zone(self._s, time_zone, use_earliest, ambiguous, non_existent)

    def convert_time_zone(self, time_zone: str) -> Series:
        """The polars name of :meth:`tz_convert`: the same instants shown in
        ``time_zone``."""
        from ._time_index import convert_time_zone

        return convert_time_zone(self._s, time_zone)


class _ListAccessor(_ext.ListAccessor):
    """``s.list``: the List column accessors (polars ``s.list.len()`` /
    ``s.list.get(i)``). ``len`` and ``get`` are native, from api.def."""

    __slots__ = ()

    def first(self) -> Series:
        return self._s.list_get(0)

    def last(self) -> Series:
        return self._s.list_get(-1)


class _StrAccessor(_ext.StrAccessor):
    """``s.str``: the string kernels under pandas and polars names. The
    methods that are exactly one kernel are native, from api.def; the ones
    with Python logic around the kernels are here."""

    __slots__ = ()

    # pandas
    def contains(
        self,
        pat: str,
        case: bool = True,
        flags: int = 0,
        na: bool | None = None,
        regex: bool = True,
        *,
        literal: bool | None = None,
    ) -> Series[bool]:
        """Whether each value contains ``pat``: a regex search by default (the
        pandas and Polars default), the substring kernel with ``regex=False``
        or ``literal=True``. ``na`` replaces a null result. ``flags`` has no
        kernel."""
        if literal is not None:
            regex = not literal
        if flags:
            raise NotImplementedError("contains(flags=...) has no kernel")
        if regex:
            out = self._s.str_search(pat if case else "(?i)" + pat)
        elif case:
            out = self._s.str_contains(pat)
        else:
            out = self._s.to_lowercase().str_contains(pat.lower())
        return out if na is None else out.fill_null(na)

    def match(self, pat: str) -> Series[bool]:
        """Regex anchored at the start, as pandas ``str.match``."""
        return self._s.str_search("^(?:" + pat + ")")

    def replace(
        self,
        pat: str,
        repl: str,
        n: int = -1,
        regex: bool = False,
        *,
        literal: bool | None = None,
    ) -> Series[builtins.str]:
        """Replace every occurrence (``n=-1``) or the first (``n=1``) of the
        literal ``pat``; with ``regex=True`` (or ``literal=False``) ``pat`` is
        a regex and ``repl`` may use ``$n`` / ``${name}`` group references, for
        every match (``n=-1``) only."""
        if literal is not None:
            regex = not literal
        if regex:
            if n != -1:
                raise NotImplementedError("a regex replace supports n=-1 (all)")
            return self._s.str_regex_replace(pat, repl)
        if n == -1:
            return self._s.str_replace_all(pat, repl)
        if n == 1:
            return self._s.str_replace(pat, repl)
        raise NotImplementedError("replace() supports n=-1 (all) or n=1 (first)")

    def replace_all(self, pat: str, repl: str, *, literal: bool = False) -> Series[builtins.str]:
        """Replace every match of the regex ``pat`` (the Polars default; ``repl``
        may use ``$n`` / ``${name}``), or every occurrence of the literal ``pat``
        with ``literal=True``."""
        if literal:
            return self._s.str_replace_all(pat, repl)
        return self._s.str_regex_replace(pat, repl)

    def head(self, n: int) -> Series[builtins.str]:
        """The first ``n`` bytes of each value (polars ``head``)."""
        return self._s.str_slice(0, n)

    def tail(self, n: int) -> Series[builtins.str]:
        """The last ``n`` bytes of each value (polars ``tail``)."""
        return self._s.str_slice(-n, n)

    def slice(self, start: int = 0, stop: int | None = None) -> Series[builtins.str]:
        """Byte substring ``[start, stop)``, as pandas ``str.slice``."""
        length = -1 if stop is None else max(stop - start, 0)
        return self._s.str_slice(start, length)

    def pad(
        self, width: int, side: Literal["left", "right"] = "left", fillchar: str = " "
    ) -> Series[builtins.str]:
        if side == "left":
            return self._s.str_pad_start(width, fillchar)
        if side == "right":
            return self._s.str_pad_end(width, fillchar)
        raise ValueError("side must be 'left' or 'right'")

    def split(self, pat: str, expand: bool = False) -> Series | DataFrame:
        """Split on the literal ``pat``: a ``list<string>`` Series, or with
        ``expand=True`` one String column per piece named ``"0"``, ``"1"``,
        ... (a short row's missing pieces are null), as pandas."""
        parts = self._s.str_split(pat)
        if not expand:
            return parts
        from ._dataframe import DataFrame

        width = parts.list_len().max()
        n = int(width) if width is not None else 0
        return DataFrame.from_dict({str(i): parts.list_get(i) for i in range(n)})

    def regex_replace(self, pat: str, to: str) -> Series[builtins.str]:
        """:meth:`Series.str_regex_replace`: ``$n``/``${name}`` group references."""
        return self._s.str_regex_replace(pat, to)

    def extract(self, pat: str, group: int = 1) -> Series[builtins.str]:
        """The capture ``group`` of the first regex ``pat`` match per row, null
        where there is no match (pandas ``str.extract`` for one group)."""
        return self._s.str_extract(pat, group)

    # polars
    def pad_start(self, width: int, fill: str = " ") -> Series[builtins.str]:
        return self._s.str_pad_start(width, fill)

    def pad_end(self, width: int, fill: str = " ") -> Series[builtins.str]:
        return self._s.str_pad_end(width, fill)

    # pandas, the rest of the surface (ASCII; a byte outside ASCII is left as
    # it is by the case forms and fails the character classes).
    def capitalize(self) -> Series[builtins.str]:
        return self._s.str_case(0)

    def to_titlecase(self) -> Series[builtins.str]:
        """As :meth:`title` (the polars name)."""
        return self._s.str_case(1)

    def title(self) -> Series[builtins.str]:
        return self._s.str_case(1)

    def swapcase(self) -> Series[builtins.str]:
        return self._s.str_case(2)

    def isalnum(self) -> Series[bool]:
        return self._s.str_is(0)

    def encode(self, encoding: str = "utf-8", errors: str = "strict") -> Series:
        """The UTF-8 bytes of each string, as a binary column (pandas
        ``str.encode``). Only UTF-8, the engine's text encoding."""
        require_utf8("encode", encoding)
        return self._s.astype("binary")

    def decode(self, encoding: str = "utf-8", errors: str = "strict") -> Series:
        """Each binary value read as UTF-8 text (pandas ``str.decode``). With
        ``errors="strict"`` a value that is not valid UTF-8 raises
        ``ValueError``; with ``errors="null"`` it becomes null."""
        require_utf8("decode", encoding)
        if errors not in ("strict", "null"):
            raise ValueError(f"decode: errors must be 'strict' or 'null', not {errors!r}")
        out = self._s.astype("string")
        if errors == "strict" and out.null_count > self._s.null_count:
            raise ValueError("decode: a value is not valid utf-8")
        return out

    def isascii(self) -> Series[bool]:
        """True where every character is ASCII, as ``str.isascii`` (an empty
        string too)."""
        return self.fullmatch("[\\x00-\\x7f]*")

    def isalpha(self) -> Series[bool]:
        return self._s.str_is(1)

    def isdigit(self) -> Series[bool]:
        return self._s.str_is(2)

    def isdecimal(self) -> Series[bool]:
        return self._s.str_is(3)

    def isnumeric(self) -> Series[bool]:
        return self._s.str_is(4)

    def isspace(self) -> Series[bool]:
        return self._s.str_is(5)

    def islower(self) -> Series[bool]:
        return self._s.str_is(6)

    def isupper(self) -> Series[bool]:
        return self._s.str_is(7)

    def istitle(self) -> Series[bool]:
        return self._s.str_is(8)

    def index(self, sub: str) -> Series[int]:
        """As :meth:`find`, but a row without ``sub`` raises (``ValueError``),
        as Python's ``str.index``."""
        out = self._s.str_find(sub)
        if out.eq(-1).any():
            raise ValueError(f"index: substring {sub!r} not found in every row")
        return out

    def rindex(self, sub: str) -> Series[int]:
        out = self._s.str_rfind(sub)
        if out.eq(-1).any():
            raise ValueError(f"rindex: substring {sub!r} not found in every row")
        return out

    def repeat(self, repeats: int) -> Series[builtins.str]:
        return self._s.str_repeat(repeats)

    def center(self, width: int, fillchar: str = " ") -> Series[builtins.str]:
        return self._s.str_center(width, fillchar)

    def ljust(self, width: int, fillchar: str = " ") -> Series[builtins.str]:
        return self._s.str_pad_end(width, fillchar)

    def rjust(self, width: int, fillchar: str = " ") -> Series[builtins.str]:
        return self._s.str_pad_start(width, fillchar)

    def cat(self, others: Series | str | None = None, sep: str = "") -> Series | str:
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

    def normalize(self, form: NormForm) -> Series[builtins.str]:
        """Each string in the Unicode normalization ``form`` (``"NFC"``,
        ``"NFKC"``, ``"NFD"`` or ``"NFKD"``); a value that is not valid UTF-8
        is null."""
        return self._s.str_normalize(norm_form_code(form))

    def translate(self, table: Mapping[int | str, str | int | None]) -> Series[builtins.str]:
        """Each codepoint replaced by ``table`` (as ``str.translate``): a key is
        a codepoint or a one-character string, a value a string, a codepoint or
        ``None`` to delete it. A codepoint not in the table is kept."""
        keys, values = translate_pairs(table)
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
        flags = wrap_flag_bits(
            expand_tabs, replace_whitespace, drop_whitespace, break_long_words, break_on_hyphens
        )
        return self._s.str_wrap(width, flags)

    def extractall(self, pat: str) -> DataFrame:
        """Every regex ``pat`` match per row, one output row per match: the
        columns ``row`` (input position) and ``match`` (0-based order within
        the row), then one per capture group, named as in ``pat`` or
        ``"0"``, ``"1"``, ...  Rows without a match are left out (pandas
        ``str.extractall`` with its index as columns)."""
        import re

        from ._dataframe import DataFrame

        rx = re.compile(pat)
        if rx.groups == 0:
            raise ValueError("extractall: the pattern has no capture group")
        names = {i: n for n, i in rx.groupindex.items()}
        labels = [names.get(g, str(g - 1)) for g in range(1, rx.groups + 1)]
        found = DataFrame.from_dict({"m": self._s.str_findall(pat)})
        rows = found.with_row_index("row").explode("m")
        rows = rows[rows["m"].is_not_null()]
        if len(rows) == 0:
            ints = {c: Series.from_list([], dtype=DType.INT64) for c in ["row", "match"]}
            text = {c: Series.from_list([], dtype=DType.STRING) for c in labels}
            return DataFrame.from_dict({**ints, **text})
        rows = rows.assign(match=rows.groupby("row", as_index=False).cumcount()["cumcount"])
        m = rows["m"]
        groups = {c: _StrAccessor(m).extract(pat, g) for g, c in enumerate(labels, 1)}
        return DataFrame.from_dict({"row": rows["row"], "match": rows["match"], **groups})

    def partition(self, sep: str = " ", expand: bool = True) -> Series | DataFrame:
        """Split at the first ``sep`` into (head, sep, tail): a frame of
        columns ``"0"``, ``"1"``, ``"2"`` (or with ``expand=False`` the
        List<String>); ``rpartition`` splits at the last."""
        return self._expand3(self._s.str_partition(sep, False), expand)

    def rpartition(self, sep: str = " ", expand: bool = True) -> Series | DataFrame:
        return self._expand3(self._s.str_partition(sep, True), expand)

    def _expand3(self, parts: Series, expand: bool) -> Series | DataFrame:
        if not expand:
            return parts
        from ._dataframe import DataFrame

        return DataFrame.from_dict({str(i): parts.list_get(i) for i in range(3)})

    def rsplit(self, pat: str, expand: bool = False) -> Series | DataFrame:
        """As :meth:`split` (every occurrence, so the same parts)."""
        return self.split(pat, expand)

    def get(self, i: int) -> Series:
        """The element at ``i`` of each row: a list's element (negative
        counts from the end), or a string's byte ``i`` (non-negative; empty
        past the end)."""
        from ._enums import DType

        s = self._s
        if s.dtype == DType.LIST:
            return s.list_get(i)
        if i < 0:
            raise ValueError("get: a negative position needs a List column; use str.slice")
        nulls = Series.from_list([None] * len(s), dtype=DType.STRING)
        return s.str_slice(i, 1).where(s.str_len_bytes().gt(i), nulls)

    def slice_replace(
        self, start: int = 0, stop: int | None = None, repl: str = ""
    ) -> Series[builtins.str]:
        """Bytes ``[start, stop)`` of each row replaced with ``repl``."""
        s = self._s
        head = s.str_slice(0, start)
        tail = s.str_slice(stop, 1 << 62) if stop is not None else s.str_slice(0, 0)
        return head.str_cat(Series.from_list([repl] * len(s))).str_cat(tail)

    def get_dummies(self, sep: str = "|") -> DataFrame:
        """One column per distinct ``sep``-separated token, 1 where a row
        holds it (pandas ``str.get_dummies``)."""
        from ._dataframe import DataFrame

        parts = self._s.str_split(sep)
        exploded = DataFrame.from_dict({"v": parts}).with_row_index("r").explode("v")
        wide = exploded.pivot(index="r", on="v", values="v", agg="count").drop("r").fill_null(0)
        # Presence (1), not a count; an empty token is no category.
        names = [c for c in wide.columns if c != ""]
        return DataFrame.from_dict({c: wide[c].gt(0).astype("int64") for c in names})


def _series_from_bytes(
    data: bytes, names: list[str] | None = None, levels: list[bytes] | None = None
) -> Series:
    """The Series a pickle holds (``Series.to_bytes`` of the native frame format),
    under the row labels it was pickled with."""
    out = _ext._series_from_bytes(data)
    if names:
        out = with_levels(out, names, [_ext._series_from_bytes(b) for b in levels or ()])
    return out


def _list_dtype(
    dtype: DType | int | builtins.str | builtins.type | pa.DataType,
) -> tuple[DType, str, str]:
    """The native type, time unit and zone a ``dtype`` argument names: a pyarrow
    type, or any spelling :meth:`Series.cast` takes. The unit is empty for a
    type that has none."""
    if type(dtype).__module__.startswith("pyarrow"):
        kind, unit, zone = arrow_native_type(cast("pa.DataType", dtype))
        return kind, unit if kind in (DType.TIMESTAMP, DType.DURATION) else "", zone
    return DType(_resolve_dtype(cast("builtins.str | int | DType | builtins.type", dtype))), "", ""


def _numpy_spans(values: list[object]) -> Series | None:
    """``values`` as a column when every value is a NumPy ``datetime64`` (or
    every one a ``timedelta64``) or ``None``, in the unit NumPy holds them;
    ``None`` for any other list."""
    import numpy as np

    present = [v for v in values if v is not None]
    for kind in (np.datetime64, np.timedelta64):
        if present and all(isinstance(v, kind) for v in present):
            # NumPy reads None as NaT in a datetime64 or timedelta64 array
            return numpy_to_native(np.array(values, dtype=np.array(present).dtype))
    return None


def _python_scalar(value: object) -> object:
    """``value`` with each NumPy scalar or array in it, at any depth, as Python
    values. A ``datetime64`` or ``timedelta64`` turns into a microsecond
    ``datetime`` or ``timedelta``, so a finer tick must be zero."""
    import numpy as np

    if isinstance(value, np.ndarray):
        return [_python_scalar(v) for v in value]
    if isinstance(value, (np.datetime64, np.timedelta64)):
        if np.isnat(value):
            return None
        span = "datetime64" if value.dtype.kind == "M" else "timedelta64"
        micro = value.astype(f"{span}[us]")
        if micro != value:
            raise ValueError(f"{value!r} is finer than a microsecond; pass a NumPy array")
        return micro.item()
    if isinstance(value, np.generic):
        return value.item()
    if isinstance(value, (list, tuple)):
        return [_python_scalar(v) for v in value]
    if isinstance(value, dict):
        return {k: _python_scalar(v) for k, v in value.items()}
    return value


def _holds_numpy(value: object) -> bool:
    if type(value).__module__ == "numpy":
        return True
    if isinstance(value, (list, tuple)):
        return any(_holds_numpy(v) for v in value)
    if isinstance(value, dict):
        return any(_holds_numpy(v) for v in value.values())
    return False


def _python_scalars(values: list[object]) -> list[object]:
    """``values`` with each NumPy scalar or array in it as Python values, or
    ``values`` itself when it holds none."""
    if not any(_holds_numpy(v) for v in values):
        return values
    return [_python_scalar(v) for v in values]


def _series_from_arrow(array: pa.Array | pa.ChunkedArray) -> Series:
    """Import a pyarrow Array into a native Series wrapper."""
    return cast("Series", _arrow_native("_series_from_arrow")(array))


_install_aliases("Series", Series)
_ext._bind_types(Series=Series)
