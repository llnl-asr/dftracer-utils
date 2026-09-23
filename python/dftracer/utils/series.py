"""The ``Series`` Python wrapper over the native columnar engine.

The C extension exposes a low-level ``_Series`` handle carrying the SIMD ops and
the Arrow C Data Interface capsule, but no Arrow/pandas/polars conversion. That
conversion lives here: ``Series`` wraps the handle, forwards every native op
(re-wrapping the handles it hands back), and adds ``to_arrow`` / ``to_pandas`` /
``to_numpy`` / ``to_polars``, NumPy-style ``+ - * /`` and ``s[i]`` indexing, the
``np.asarray`` protocol, and pickling. ``dataframe.DataFrame`` is the frame
counterpart.
"""

from __future__ import annotations

import builtins
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Generic,
    List,
    Literal,
    Optional,
    Sequence,
    Tuple,
    Type,
    TypeVar,
    Union,
    cast,
)

from . import dftracer_utils_ext as _ext
from ._pandas_series import _SeriesPandasMixin
from .enums import _DTYPE_BY_NAME, DType

if TYPE_CHECKING:
    from types import ModuleType

    import numpy as np  # ty: ignore[unresolved-import]
    import pandas as pd  # ty: ignore[unresolved-import]
    import polars as pl  # ty: ignore[unresolved-import]
    import pyarrow as pa  # ty: ignore[unresolved-import]

    from .dataframe import DataFrame

# (native handle type, Python wrapper) pairs, filled as each wrapper module
# imports. _wrap consults it so the base forwarder can wrap any handle a native
# op returns without this module importing the frame/viewer wrappers.
_WRAP: List[Tuple[type, Type["_Wrapper"]]] = []


def _register(native_type: type, wrapper: Type["_Wrapper"]) -> None:
    _WRAP.append((native_type, wrapper))


# Any: precise overloads break self-typed builder methods that return _ViewerT.
def _wrap(obj: Any) -> Any:
    for native_type, wrapper in _WRAP:
        if isinstance(obj, native_type):
            return wrapper(obj)
    return obj


def _resolve_dtype(dtype: "Union[str, int, DType]") -> int:
    """A dtype spelled as a name, a DType member, or a raw int code -> int
    code. Raises ValueError on an unrecognized name."""
    if isinstance(dtype, str):
        try:
            return int(_DTYPE_BY_NAME[dtype.lower()])
        except KeyError:
            raise ValueError(f"unknown dtype name: {dtype!r}") from None
    return int(dtype)


_N = TypeVar("_N")


# Any: a `_Wrapper[_N] -> _N` overload mis-resolves on union-typed arguments.
def _unwrap(obj: Any) -> Any:
    return obj._native if isinstance(obj, _Wrapper) else obj


class _Wrapper(Generic[_N]):
    """Holds a native handle and forwards unknown attributes to it, wrapping the
    handles it hands back and unwrapping the ones passed in. Parameterized by the
    concrete native handle type."""

    __slots__ = ("_native",)

    _native: _N

    def __init__(self, native: _N) -> None:
        self._native = native

    # Runtime-only forwarder for native methods not explicitly wrapped; hidden
    # from the checker so unknown attributes are type errors, not Any.
    if not TYPE_CHECKING:

        def __getattr__(self, name):
            attr = getattr(self._native, name)
            if not callable(attr):
                return _wrap(attr)

            def forward(*args, **kwargs):
                uargs = [_unwrap(a) for a in args]
                ukwargs = {k: _unwrap(v) for k, v in kwargs.items()}
                return _wrap(attr(*uargs, **ukwargs))

            return forward

    # Opaque Arrow C Data Interface capsules; Python has no capsule type.
    def __arrow_c_array__(
        self, requested_schema: Optional[object] = None
    ) -> "Tuple[object, object]":
        return self._native.__arrow_c_array__(requested_schema)


def _to_pandas(arrow_obj: Any, arrow: bool) -> Any:
    """``pyarrow`` to pandas: NumPy dtypes (a copy) or, with ``arrow``, the
    ``pd.ArrowDtype`` columns that share the Arrow buffers."""
    if not arrow:
        return arrow_obj.to_pandas()
    import pandas as pd  # ty: ignore[unresolved-import]

    return arrow_obj.to_pandas(types_mapper=pd.ArrowDtype)


def _require_pyarrow() -> "ModuleType":
    try:
        import pyarrow as pa  # ty: ignore[unresolved-import]
    except ImportError:
        raise ImportError(
            "pyarrow is required for Arrow conversion. Install with: pip install pyarrow"
        ) from None
    return pa


# Comparison op codes, mirroring abi.h DFTU_CMP_* (passed to native compare()).
_CMP_GT = 0
_CMP_GE = 1
_CMP_LT = 2
_CMP_LE = 3
_CMP_EQ = 4
_CMP_NE = 5
_LOGICAL_AND = 0
_LOGICAL_OR = 1

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


class Series(_SeriesPandasMixin, _Wrapper["_ext._Series"]):
    """A typed column: SIMD ops from the native engine plus Arrow/NumPy conversion.

    Supports NumPy-style ``+ - * /`` against another Series (elementwise) or a
    scalar, ``s[i]`` / ``s[a:b]`` indexing, and ``np.asarray(s)``. Reflected
    scalar division (``2 / s``) is not provided; use ``2 / s.to_numpy()``.
    Comparisons (``<``, ``<=``, ``>``, ``>=``, ``gt``/``ge``/``lt``/``le``/``eq``/
    ``ne``) return a boolean mask Series.

    ``Series([1, 2, 3])`` builds one from a Python sequence, the pandas / polars
    constructor; an Arrow array (anything with ``__arrow_c_array__``) imports
    zero-copy with its own type, so an empty or all-null one keeps it;
    ``Series(native)`` wraps a native handle."""

    def __init__(self, data: "Union[_ext._Series, Sequence[object]]") -> None:
        if isinstance(data, Series):
            data = data._native
        elif not isinstance(data, _ext._Series):
            if hasattr(data, "__arrow_c_array__"):
                data = _unwrap(_series_from_arrow(data))
            else:
                data = _unwrap(Series.from_list(list(data)))
        super().__init__(data)

    @classmethod
    def from_arrow(cls, arr: "pa.Array") -> "Series":
        """Import a ``pyarrow.Array`` (or any ``__arrow_c_array__`` provider),
        zero-copy via the Arrow C Data Interface."""
        return _series_from_arrow(arr)

    @classmethod
    def from_pandas(cls, s: "pd.Series") -> "Series":
        """Import a ``pandas.Series`` (zero-copy for numeric, via pyarrow)."""
        pa = _require_pyarrow()
        return _series_from_arrow(pa.Array.from_pandas(s))

    @classmethod
    def from_polars(cls, s: "pl.Series") -> "Series":
        """Import a ``polars.Series`` (zero-copy via its Arrow buffers)."""
        return _series_from_arrow(s.to_arrow())

    @classmethod
    def from_numpy(cls, a: "np.ndarray") -> "Series":
        """Import a NumPy array.

        A 1-D C-contiguous fixed-width numeric array takes the native
        pyarrow-free path (zero-copy borrow of the array's buffer); any other
        case (non-contiguous, unsupported dtype, non-1-D) falls back to the
        Arrow path, which needs pyarrow."""
        try:
            return Series(_ext._series_from_numpy(a))
        except (TypeError, BufferError, RuntimeError):
            pa = _require_pyarrow()
            return _series_from_arrow(pa.array(a))

    @classmethod
    def from_list(cls, values: Sequence[object], dtype: "Optional[pa.DataType]" = None) -> "Series":
        """Import a Python sequence, optionally typed by a pyarrow ``dtype``."""
        pa = _require_pyarrow()
        return _series_from_arrow(pa.array(values, type=dtype))

    def to_arrow(self) -> "pa.Array":
        """This column as a ``pyarrow.Array`` (zero-copy via the C Data
        Interface)."""
        return _require_pyarrow().array(self._native)

    def to_pandas(self, *, arrow: bool = False) -> "pd.Series":
        """This column as a pandas Series: a NumPy dtype by default (a copy),
        or with ``arrow=True`` Arrow-backed, sharing this column's buffers."""
        return _to_pandas(self.to_arrow(), arrow)

    def to_numpy(self) -> "np.ndarray":
        """This column as a NumPy array.

        A flat, non-null, fixed-width numeric column is read directly from the
        native buffer (no pyarrow, zero-copy view); Bool, strings, nulls, and
        non-flat encodings fall back to the Arrow path (which needs pyarrow)."""
        import numpy as np  # ty: ignore[unresolved-import]

        try:
            # memoryview forces the native buffer protocol (a plain
            # np.asarray(handle) can fall back to a 0-d object array).
            # _Series's C buffer support is not in the stub (PEP 688 is 3.12+).
            return np.asarray(memoryview(self._native))  # ty: ignore[invalid-argument-type]
        except (BufferError, TypeError):
            return self.to_arrow().to_numpy(zero_copy_only=False)

    def to_polars(self) -> "pl.Series":
        """This column as a polars Series."""
        try:
            import polars as pl  # ty: ignore[unresolved-import]
        except ImportError:
            raise ImportError(
                "polars is required for to_polars(). Install with: pip install polars"
            ) from None
        return pl.Series(pl.from_arrow(self.to_arrow()))

    def __array__(
        self, dtype: "Optional[np.dtype]" = None, copy: Optional[bool] = None
    ) -> "np.ndarray":
        """NumPy array protocol, so ``np.asarray(series)`` works."""
        import numpy as np  # ty: ignore[unresolved-import]

        return np.asarray(self.to_numpy(), dtype=dtype)

    def __len__(self) -> int:
        return self._native.length

    # builtins.slice (not bare `slice`): ty otherwise binds it to Series.slice.
    def __getitem__(self, key: "Union[int, builtins.slice]") -> "Union[Series, object]":
        """``s[i]`` returns the element as a Python scalar; ``s[a:b]`` returns a
        Series (step-1 slices only)."""
        arr = self.to_arrow()
        if isinstance(key, slice):
            return _series_from_arrow(arr[key])
        idx = key + len(self) if key < 0 else key
        return arr[idx].as_py()

    # Returns a Series, or NotImplemented for an unsupported right operand.
    def _elementwise(self, other: _SeriesOrScalar, series_op: str, scalar_op: str) -> "Series":
        if isinstance(other, Series):
            return getattr(self, series_op)(other)
        if isinstance(other, (int, float)) and not isinstance(other, bool):
            return getattr(self, scalar_op)(other)
        return NotImplemented

    def __add__(self, other: _SeriesOrScalar) -> "Series":
        return self._elementwise(other, "add", "add_scalar")

    def __radd__(self, other: _Scalar) -> "Series":
        return self._elementwise(other, "add", "add_scalar")

    def __sub__(self, other: _SeriesOrScalar) -> "Series":
        return self._elementwise(other, "sub", "sub_scalar")

    def __rsub__(self, other: _Scalar) -> "Series":
        # other - self; a Series left operand would take __sub__, so other is a scalar.
        if isinstance(other, (int, float)) and not isinstance(other, bool):
            return self.mul_scalar(-1).add_scalar(other)
        return NotImplemented

    def __mul__(self, other: _SeriesOrScalar) -> "Series":
        return self._elementwise(other, "mul", "mul_scalar")

    def __rmul__(self, other: _Scalar) -> "Series":
        return self._elementwise(other, "mul", "mul_scalar")

    def __truediv__(self, other: _SeriesOrScalar) -> "Series":
        # True division, as pandas / polars `/`: an integer operand is widened
        # to Float64 first (the engine's div over two integers truncates; that
        # form is `ops.series.div`; `//` is floordiv).
        me = self.astype(DType.FLOAT64) if self.dtype in _INTEGRAL else self
        if isinstance(other, Series) and other.dtype in _INTEGRAL:
            other = other.astype(DType.FLOAT64)
        elif isinstance(other, int) and not isinstance(other, bool):
            other = float(other)
        if isinstance(other, Series):
            return _wrap(me._native.div(_unwrap(other)))
        if isinstance(other, float):
            return me.div_scalar(other)
        return NotImplemented

    def _extra(self, name: str, other: _SeriesOrScalar, reverse: bool = False) -> "Series":
        if isinstance(other, Series):
            return _wrap(getattr(self._native, name)(_unwrap(other)))
        if isinstance(other, (int, float)) and not isinstance(other, bool):
            return _wrap(getattr(self._native, name)(other, reverse))
        return NotImplemented

    def __floordiv__(self, other: _SeriesOrScalar) -> "Series":
        """Floor division (Python's rule; a zero divisor is null)."""
        return self._extra("floordiv", other)

    def __rfloordiv__(self, other: _Scalar) -> "Series":
        return self._extra("floordiv", other, reverse=True)

    def __mod__(self, other: _SeriesOrScalar) -> "Series":
        """Remainder with the divisor's sign (Python's rule)."""
        return self._extra("mod", other)

    def __rmod__(self, other: _Scalar) -> "Series":
        return self._extra("mod", other, reverse=True)

    def __pow__(self, other: _SeriesOrScalar) -> "Series":
        """Power: Int64 for two integers (a negative exponent is null), else
        Float64."""
        return self._extra("pow", other)

    def __rpow__(self, other: _Scalar) -> "Series":
        return self._extra("pow", other, reverse=True)

    def floordiv(self, other: _SeriesOrScalar) -> "Series":
        return self._extra("floordiv", other)

    def rfloordiv(self, other: _Scalar) -> "Series":
        return self._extra("floordiv", other, reverse=True)

    def mod(self, other: _SeriesOrScalar) -> "Series":
        return self._extra("mod", other)

    def rmod(self, other: _Scalar) -> "Series":
        return self._extra("mod", other, reverse=True)

    def pow(self, other: _SeriesOrScalar) -> "Series":
        return self._extra("pow", other)

    def rpow(self, other: _Scalar) -> "Series":
        return self._extra("pow", other, reverse=True)

    def divmod(self, other: _SeriesOrScalar) -> "Tuple[Series, Series]":
        return self // other, self % other

    def rdivmod(self, other: _Scalar) -> "Tuple[Series, Series]":
        return other // self, other % self

    def ffill(self) -> "Series":
        """Nulls filled from the nearest present value before them (``pad``)."""
        return _wrap(self._native.ffill())

    def bfill(self) -> "Series":
        """Nulls filled from the nearest present value after them (``backfill``)."""
        return _wrap(self._native.bfill())

    pad = ffill
    backfill = bfill

    # Comparisons return a boolean mask Series, ``==`` and ``!=`` included,
    # as pandas and polars; a Series is therefore unhashable, as theirs are.
    # A string scalar compares the text of a String column.
    __hash__ = None  # type: ignore[assignment]

    def __eq__(self, other: object) -> "Series":  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._cmp(_CMP_EQ, other)

    def __ne__(self, other: object) -> "Series":  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._cmp(_CMP_NE, other)

    def __lt__(self, other: _Scalar) -> "Series":
        return self._cmp(_CMP_LT, other)

    def __le__(self, other: _Scalar) -> "Series":
        return self._cmp(_CMP_LE, other)

    def __gt__(self, other: _Scalar) -> "Series":
        return self._cmp(_CMP_GT, other)

    def __ge__(self, other: _Scalar) -> "Series":
        return self._cmp(_CMP_GE, other)

    def gt(self, value: _Scalar) -> "Series":
        """Boolean mask where the value is greater than ``value``."""
        return self._cmp(_CMP_GT, value)

    def ge(self, value: _Scalar) -> "Series":
        """Boolean mask where the value is greater than or equal to ``value``."""
        return self._cmp(_CMP_GE, value)

    def lt(self, value: _Scalar) -> "Series":
        """Boolean mask where the value is less than ``value``."""
        return self._cmp(_CMP_LT, value)

    def le(self, value: _Scalar) -> "Series":
        """Boolean mask where the value is less than or equal to ``value``."""
        return self._cmp(_CMP_LE, value)

    def eq(self, value: _Scalar) -> "Series":
        """Boolean mask where the value equals ``value``."""
        return self._cmp(_CMP_EQ, value)

    def ne(self, value: _Scalar) -> "Series":
        """Boolean mask where the value does not equal ``value``."""
        return self._cmp(_CMP_NE, value)

    # -- native engine ops (SIMD kernels, wrapped from the C extension) --------
    def add(self, other: "Series") -> "Series":
        """Element-wise addition; mixed dtypes promote to a common type
        (numpy rules, e.g. uint64 + float64 -> float64)."""
        return _wrap(self._native.add(_unwrap(other)))

    def sub(self, other: "Series") -> "Series":
        """Element-wise subtraction; mixed dtypes promote to a common type
        (numpy rules). Note: uint64 - uint64 wraps on underflow (like numpy);
        cast to a signed or float dtype first for interval/signed math."""
        return _wrap(self._native.sub(_unwrap(other)))

    def mul(self, other: "Series") -> "Series":
        return _wrap(self._native.mul(_unwrap(other)))

    def div(self, other: "Union[Series, int, float]") -> "Series":
        """True division (pandas ``div``): integer operands widen to Float64
        first. The engine's integer-truncating kernel is ``ops.series.div``."""
        return self / other

    def add_scalar(self, v: Union[int, float]) -> "Series":
        return _wrap(self._native.add_scalar(_unwrap(v)))

    def sub_scalar(self, v: Union[int, float]) -> "Series":
        """Subtract a scalar; a float ``v`` promotes an integer column to
        Float64 (numpy's weak-scalar rule). Note: uint64 - v wraps on
        underflow (like numpy); cast to a signed or float dtype first for
        interval/signed math."""
        return _wrap(self._native.sub_scalar(_unwrap(v)))

    def mul_scalar(self, v: Union[int, float]) -> "Series":
        return _wrap(self._native.mul_scalar(_unwrap(v)))

    def div_scalar(self, v: Union[int, float]) -> "Series":
        return _wrap(self._native.div_scalar(_unwrap(v)))

    def apply(self, func: Callable[..., object]) -> "Series":
        """``func`` over each element (pandas ``Series.apply``), engine-first:
        the function is traced once with a symbolic column and, when it builds
        a column expression, runs fused in the engine; a ``@jit.series`` or
        ``@jit.op`` function runs through its own path; anything else runs in
        Python per element with a warning naming why. See ``_apply``."""
        from ._apply import apply_series

        return apply_series(self, func, "Series.apply")

    def map(self, func: Callable[..., object]) -> "Series":
        """``func`` over each element; the pandas ``Series.map`` spelling of
        :meth:`apply`."""
        from ._apply import apply_series

        return apply_series(self, func, "Series.map")

    def astype(self, dtype: "Union[str, int, DType]") -> "Series":
        """Cast to ``dtype``: a :class:`~dftracer.utils.enums.DType`, its
        int code, or a dtype name (``"int64"``, ``"Float64"``, case-insensitive;
        matches the ``DType`` member names). The pandas-style primary spelling
        of :meth:`cast`."""
        return _wrap(self._native.cast(_unwrap(_resolve_dtype(dtype))))

    def cast(self, type_id: "Union[str, int, DType]") -> "Series":
        """Alias of :meth:`astype`."""
        return self.astype(type_id)

    def prim(self, op: int) -> "Series":
        return _wrap(self._native.prim(_unwrap(op)))

    def _cmp(self, op: int, scalar: object) -> "Series":
        if isinstance(scalar, Series):
            return self.compare_series(scalar, op)
        return _wrap(self._native.compare(_unwrap(op), _unwrap(scalar)))

    def compare_series(self, other: "Series", op: int) -> "Series":
        """Row by row against ``other`` (``op`` a ``_CMP_*`` code): a Bool
        mask, null where either side is null; the engine's
        ``dftu.series.compare_series``. ``==`` / ``<`` / ... spell it."""
        from . import dftracer_utils_ext as _ext

        return _wrap(_ext.op_run("dftu.series.compare_series", self._native, other._native, op))

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
        left: List[object] = []
        right: List[object] = []
        for i, (x, y) in enumerate(zip(a, b)):
            same = x == y or (x is None and y is None)
            if same and not keep_shape:
                continue
            idx.append(i)
            left.append(x if (not same or keep_equal) else None)
            right.append(y if (not same or keep_equal) else None)
        # Typed columns, so no difference at all is still a well-typed frame.
        pa = _require_pyarrow()
        arrow_type = self.to_arrow().type
        return DataFrame(
            {
                "index": pa.array(idx, pa.int64()),
                "self": pa.array(left, arrow_type),
                "other": pa.array(right, arrow_type),
            }
        )

    def logical(self, op: int, other: "Series") -> "Series":
        return _wrap(self._native.logical(_unwrap(op), _unwrap(other)))

    def logical_not(self) -> "Series":
        return _wrap(self._native.logical_not())

    def __and__(self, other: "Series") -> "Series":
        return self.logical(_LOGICAL_AND, other)

    def __or__(self, other: "Series") -> "Series":
        return self.logical(_LOGICAL_OR, other)

    def __invert__(self) -> "Series":
        return self.logical_not()

    def quantile(self, q: float) -> float:
        return self._native.quantile(_unwrap(q))

    def median(self) -> float:
        return self._native.median()

    def variance(self, sample: bool = True) -> float:
        return self._native.variance(_unwrap(sample))

    def stddev(self, sample: bool = True) -> float:
        return self._native.stddev(_unwrap(sample))

    def skewness(self) -> float:
        return self._native.skewness()

    def kurtosis(self) -> float:
        return self._native.kurtosis()

    def nunique(self) -> int:
        return self._native.nunique()

    def unique(self) -> "Series":
        return _wrap(self._native.unique())

    def value_counts(
        self, normalize: bool = False, sort: bool = True, ascending: bool = False
    ) -> "DataFrame":
        """A ``value`` / ``count`` frame of the distinct values, most frequent
        first (the pandas arguments: ``normalize`` reports a ``proportion``
        column instead, ``ascending`` flips the order, ``sort=False`` keeps
        first-seen order). A frame rather than an indexed Series: this engine
        has no index."""
        vc = _wrap(self._native.value_counts())
        if normalize:
            total = float(self._native.length)
            vc = vc.with_column("proportion", vc["count"].astype("float64") / total).select(
                "value", "proportion"
            )
        if sort and ascending:
            vc = vc.reverse()
        return vc

    def abs(self) -> "Series":
        return _wrap(self._native.abs())

    def clip(
        self,
        lo: Union[int, float, None] = None,
        hi: Union[int, float, None] = None,
        *,
        lower: Union[int, float, None] = None,
        upper: Union[int, float, None] = None,
        lower_bound: Union[int, float, None] = None,
        upper_bound: Union[int, float, None] = None,
    ) -> "Series":
        """Clamp to ``[lo, hi]``. ``lower`` / ``upper`` (pandas) and
        ``lower_bound`` / ``upper_bound`` (polars) name the same two bounds;
        both are required."""
        lo = _first_set(lo, lower, lower_bound)
        hi = _first_set(hi, upper, upper_bound)
        if lo is None or hi is None:
            raise TypeError("clip() needs both a lower and an upper bound")
        return _wrap(self._native.clip(_unwrap(lo), _unwrap(hi)))

    def round(self, decimals: int = 0) -> "Series":
        """Round to the nearest integer. Only ``decimals=0`` has a kernel."""
        if decimals != 0:
            raise NotImplementedError("round() supports decimals=0 only")
        return _wrap(self._native.round())

    def fillna(self, value: Union[int, float]) -> "Series":
        return _wrap(self._native.fillna(_unwrap(value)))

    def cumsum(self) -> "Series":
        return _wrap(self._native.cumsum())

    def cummax(self) -> "Series":
        return _wrap(self._native.cummax())

    def cummin(self) -> "Series":
        return _wrap(self._native.cummin())

    def cum_prod(self) -> "Series":
        return _wrap(self._native.cum_prod())

    def cum_count(self) -> "Series":
        return _wrap(self._native.cum_count())

    def ceil(self) -> "Series":
        return _wrap(self._native.ceil())

    def floor(self) -> "Series":
        return _wrap(self._native.floor())

    def trunc(self) -> "Series":
        return _wrap(self._native.trunc())

    def sign(self) -> "Series":
        return _wrap(self._native.sign())

    def negate(self) -> "Series":
        return _wrap(self._native.negate())

    def diff(self) -> "Series":
        return _wrap(self._native.diff())

    def pct_change(self) -> "Series":
        return _wrap(self._native.pct_change())

    def sqrt(self) -> "Series":
        return _wrap(self._native.sqrt())

    def exp(self) -> "Series":
        return _wrap(self._native.exp())

    def log(self) -> "Series":
        return _wrap(self._native.log())

    def rank(
        self,
        method: Literal["average", "min", "max", "dense", "ordinal", "first"] = "average",
        descending: bool = False,
        *,
        ascending: Optional[bool] = None,
    ) -> "Series":
        """Rank each element (1-based); ties per ``method``. Always returns a
        Float64 Series, matching pandas ``rank()`` (even ``method="dense"``,
        which pandas also returns as float). Call ``.astype("int64")`` on the
        result for integer ranks. ``ascending`` is the pandas spelling of
        ``not descending``."""
        if ascending is not None:
            descending = not ascending
        return _wrap(self._native.rank(_unwrap(method), _unwrap(descending)))

    def rolling(
        self,
        window: int,
        op: Optional[Literal["sum", "mean", "min", "max"]] = None,
    ) -> "Union[Series, _Rolling]":
        """A trailing window of ``window`` rows. With ``op`` the reduced Series;
        without it a window object with ``.sum() .mean() .min() .max() .var()
        .std() .median() .quantile(q)``, the pandas ``s.rolling(n).mean()``
        spelling."""
        if op is None:
            return _Rolling(self, window)
        return _wrap(self._native.rolling(_unwrap(window), _unwrap(op)))

    def rolling_sum(self, window: int) -> "Series":
        return _wrap(self._native.rolling(_unwrap(window), "sum"))

    def rolling_mean(self, window: int) -> "Series":
        return _wrap(self._native.rolling(_unwrap(window), "mean"))

    def rolling_min(self, window: int) -> "Series":
        return _wrap(self._native.rolling(_unwrap(window), "min"))

    def rolling_max(self, window: int) -> "Series":
        return _wrap(self._native.rolling(_unwrap(window), "max"))

    def rolling_var(self, window: int) -> "Series":
        return _wrap(self._native.rolling_var(_unwrap(window)))

    def rolling_std(self, window: int) -> "Series":
        return _wrap(self._native.rolling_std(_unwrap(window)))

    def rolling_median(self, window: int) -> "Series":
        return _wrap(self._native.rolling_median(_unwrap(window)))

    def rolling_quantile(self, window: int, q: float) -> "Series":
        return _wrap(self._native.rolling_quantile(_unwrap(window), _unwrap(q)))

    def ewm_mean(self, alpha: float) -> "Series":
        return _wrap(self._native.ewm_mean(_unwrap(alpha)))

    def ewm_std(self, alpha: float) -> "Series":
        return _wrap(self._native.ewm_std(_unwrap(alpha)))

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

    def cut(self, breaks: "Series") -> "Series":
        return _wrap(self._native.cut(_unwrap(breaks)))

    def qcut(self, q: int) -> "Series":
        return _wrap(self._native.qcut(_unwrap(q)))

    def search_sorted(self, values: "Series") -> "Series":
        return _wrap(self._native.search_sorted(_unwrap(values)))

    def interpolate(self) -> "Series":
        return _wrap(self._native.interpolate())

    def where(self, cond: "Series", other: "Union[Series, int, float]") -> "Series":
        """This value where ``cond`` is true, else ``other`` (pandas
        ``Series.where``; a scalar ``other`` broadcasts, a float one widens
        the result to Float64; a null in ``cond`` takes ``other``)."""
        a, b = self._arms(other)
        return _wrap(a._native.where(_unwrap(cond), _unwrap(b)))

    def mask(self, cond: "Series", other: "Union[Series, int, float]") -> "Series":
        """``other`` where ``cond`` is true, else this value (pandas
        ``Series.mask``)."""
        a, b = self._arms(other)
        return _wrap(b._native.where(_unwrap(cond), _unwrap(a)))

    def _arms(self, other: "Union[Series, int, float]") -> "Tuple[Series, Series]":
        if isinstance(other, Series):
            return self, other
        if isinstance(other, bool) or not isinstance(other, (int, float)):
            raise TypeError("other must be a Series or a number")
        me = self
        if isinstance(other, float) and self.dtype not in (DType.FLOAT32, DType.FLOAT64):
            me = self.astype(DType.FLOAT64)
        return me, me.full_like(other)

    def full_like(self, value: Union[int, float]) -> "Series":
        """A Series of this length holding ``value`` on every row, in this
        Series' numeric type (Float64 for a float value, Int64 when this
        Series is not numeric). The constant rides the null mask (0 / 1,
        never NaN or null), so no cell of this Series leaks into it."""
        target = self.dtype
        if isinstance(value, float) or target not in _NUMERIC_DTYPES:
            target = DType.FLOAT64 if isinstance(value, float) else DType.INT64
        return self.isna().astype(target).mul_scalar(0).add_scalar(value)

    def is_between(self, lo: Union[int, float], hi: Union[int, float]) -> "Series":
        return _wrap(self._native.is_between(_unwrap(lo), _unwrap(hi)))

    def dot(self, other: "Series") -> Union[int, float]:
        return self._native.dot(_unwrap(other))

    def sum(self) -> Union[int, float]:
        return self._native.sum()

    def min(self) -> Union[int, float]:
        return self._native.min()

    def max(self) -> Union[int, float]:
        return self._native.max()

    def mean(self) -> float:
        return self._native.mean()

    def count(self) -> int:
        return self._native.count()

    def product(self) -> Union[int, float]:
        return self._native.product()

    def mode(self) -> "Series":
        """The most frequent value as a one-element Series (pandas returns every
        mode; the engine keeps one)."""
        return Series([self._native.mode()])

    def all(self) -> bool:
        return self._native.all()

    def any(self) -> bool:
        return self._native.any()

    def arg_min(self) -> int:
        return self._native.arg_min()

    def arg_max(self) -> int:
        return self._native.arg_max()

    def take(self, indices: "Union[Series, Sequence[int]]") -> "Series":
        """The rows at ``indices`` (a Series or a list; any order, repeats
        allowed)."""
        return _wrap(self._native.take(_indices(indices)))

    def filter(self, mask: "Series") -> "Series":
        return _wrap(self._native.filter(_unwrap(mask)))

    def argsort(self, descending: bool = False) -> "Series":
        return _wrap(self._native.argsort(_unwrap(descending)))

    def dictionary_encode(self) -> "Series":
        return _wrap(self._native.dictionary_encode())

    def materialize(self) -> "Series":
        return _wrap(self._native.materialize())

    def share(self) -> "Series":
        return _wrap(self._native.share())

    def slice(self, offset: int, length: int) -> "Series":
        return _wrap(self._native.slice(_unwrap(offset), _unwrap(length)))

    def is_null(self, i: Optional[int] = None) -> "Union[bool, Series]":
        """With ``i``, whether row ``i`` is null; without, the Bool mask of
        null rows (polars ``is_null()``, pandas ``isna()``)."""
        if i is None:
            return _wrap(self._native.null_mask())
        return self._native.is_null(_unwrap(i))

    def is_not_null(self) -> "Series":
        """Bool mask of present rows."""
        return _wrap(self._native.valid_mask())

    def num_children(self) -> int:
        return self._native.num_children()

    def child(self, i: int) -> "Series":
        return _wrap(self._native.child(_unwrap(i)))

    def is_nan(self) -> "Series":
        return _wrap(self._native.is_nan())

    def is_finite(self) -> "Series":
        return _wrap(self._native.is_finite())

    def is_infinite(self) -> "Series":
        return _wrap(self._native.is_infinite())

    @property
    def is_unique(self) -> bool:
        """Whether every value occurs once (the pandas property). The per-row
        mask is :meth:`unique_mask`."""
        return not _wrap(self._native.is_duplicated()).any()

    def unique_mask(self) -> "Series":
        """Bool mask: the value occurs exactly once (polars ``is_unique``)."""
        return _wrap(self._native.is_unique())

    def is_duplicated(self) -> "Series":
        return _wrap(self._native.is_duplicated())

    def is_sorted(self, descending: bool = False) -> bool:
        return self._native.is_sorted(_unwrap(descending))

    def drop_nulls(self) -> "Series":
        return _wrap(self._native.drop_nulls())

    def is_in(self, values: "Union[Series, Sequence[object]]") -> "Series":
        """Bool mask: the value is one of ``values`` (a Series or a list; an
        empty list matches nothing)."""
        if not isinstance(values, Series):
            items = list(values)
            if not items:
                # Present and null at once: false on every row.
                return self.isna().logical(0, self.notna())
            values = Series.from_list(items)
        return _wrap(self._native.is_in(_unwrap(values)))

    def sort(self, descending: bool = False) -> "Series":
        return _wrap(self._native.sort(_unwrap(descending)))

    def head(self, n: int = 5) -> "Series":
        return _wrap(self._native.head(_unwrap(n)))

    def tail(self, n: int = 5) -> "Series":
        return _wrap(self._native.tail(_unwrap(n)))

    def reverse(self) -> "Series":
        return _wrap(self._native.reverse())

    def shift(self, n: int = 1, *, periods: Optional[int] = None) -> "Series":
        """Shift by ``n`` rows (``periods`` is the pandas spelling)."""
        if periods is not None:
            n = periods
        return _wrap(self._native.shift(_unwrap(n)))

    def top_k(self, k: int) -> "Series":
        return _wrap(self._native.top_k(_unwrap(k)))

    def bottom_k(self, k: int) -> "Series":
        return _wrap(self._native.bottom_k(_unwrap(k)))

    def sample(self, n: int, seed: int = 0, *, random_state: Optional[int] = None) -> "Series":
        """A deterministic ``n``-row sample (``random_state`` is the pandas
        spelling of ``seed``)."""
        if random_state is not None:
            seed = random_state
        return _wrap(self._native.sample(_unwrap(n), _unwrap(seed)))

    def str_eq(self, rhs: str) -> "Series":
        return _wrap(self._native.str_eq(_unwrap(rhs)))

    def str_contains(self, needle: str) -> "Series":
        return _wrap(self._native.str_contains(_unwrap(needle)))

    def str_starts_with(self, prefix: str) -> "Series":
        return _wrap(self._native.str_starts_with(_unwrap(prefix)))

    def str_ends_with(self, suffix: str) -> "Series":
        return _wrap(self._native.str_ends_with(_unwrap(suffix)))

    def str_matches(self, pattern: str) -> "Series":
        return _wrap(self._native.str_matches(_unwrap(pattern)))

    def str_search(self, pattern: str) -> "Series":
        return _wrap(self._native.str_search(_unwrap(pattern)))

    def str_like(self, pattern: str) -> "Series":
        return _wrap(self._native.str_like(_unwrap(pattern)))

    def str_len_bytes(self) -> "Series":
        return _wrap(self._native.str_len_bytes())

    def str_len_chars(self) -> "Series":
        return _wrap(self._native.str_len_chars())

    def str_find(self, needle: str) -> "Series":
        return _wrap(self._native.str_find(_unwrap(needle)))

    def fnv1a(self) -> "Series":
        return _wrap(self._native.fnv1a())

    def hex64_parse(self) -> "Series":
        return _wrap(self._native.hex64_parse())

    def hex64_format(self) -> "Series":
        return _wrap(self._native.hex64_format())

    def to_lowercase(self) -> "Series":
        return _wrap(self._native.to_lowercase())

    def to_uppercase(self) -> "Series":
        return _wrap(self._native.to_uppercase())

    def str_strip(self) -> "Series":
        return _wrap(self._native.str_strip())

    def str_lstrip(self) -> "Series":
        return _wrap(self._native.str_lstrip())

    def str_rstrip(self) -> "Series":
        return _wrap(self._native.str_rstrip())

    def str_replace(self, pat: str, repl: str) -> "Series":
        return _wrap(self._native.str_replace(_unwrap(pat), _unwrap(repl)))

    def str_replace_all(self, pat: str, repl: str) -> "Series":
        return _wrap(self._native.str_replace_all(_unwrap(pat), _unwrap(repl)))

    def str_slice(self, start: int, length: int = -1) -> "Series":
        return _wrap(self._native.str_slice(_unwrap(start), _unwrap(length)))

    def str_pad_start(self, width: int, fill: str = " ") -> "Series":
        return _wrap(self._native.str_pad_start(_unwrap(width), _unwrap(fill)))

    def str_pad_end(self, width: int, fill: str = " ") -> "Series":
        return _wrap(self._native.str_pad_end(_unwrap(width), _unwrap(fill)))

    def str_zfill(self, width: int) -> "Series":
        return _wrap(self._native.str_zfill(_unwrap(width)))

    def str_split(self, sep: str) -> "Series":
        return _wrap(self._native.str_split(_unwrap(sep)))

    def str_extract(self, pattern: str, group: int = 1) -> "Series":
        return _wrap(self._native.str_extract(pattern, group))

    def list_len(self) -> "Series":
        return _wrap(self._native.list_len())

    def list_get(self, index: int) -> "Series":
        return _wrap(self._native.list_get(index))

    def list_join(self, sep: str) -> "Series":
        return _wrap(self._native.list_join(sep))

    def str_case(self, code: int) -> "Series":
        return _wrap(self._native.str_case(code))

    def str_is(self, code: int) -> "Series":
        return _wrap(self._native.str_is(code))

    def str_count(self, pat: str) -> "Series":
        return _wrap(self._native.str_count(pat))

    def str_rfind(self, needle: str) -> "Series":
        return _wrap(self._native.str_rfind(needle))

    def str_remove_prefix(self, prefix: str) -> "Series":
        return _wrap(self._native.str_remove_prefix(prefix))

    def str_remove_suffix(self, suffix: str) -> "Series":
        return _wrap(self._native.str_remove_suffix(suffix))

    def str_repeat(self, n: int) -> "Series":
        return _wrap(self._native.str_repeat(n))

    def str_center(self, width: int, fill: str = " ") -> "Series":
        return _wrap(self._native.str_center(width, fill))

    def str_cat(self, other: "Series") -> "Series":
        return _wrap(self._native.str_cat(_unwrap(other)))

    def str_findall(self, pattern: str) -> "Series":
        return _wrap(self._native.str_findall(pattern))

    def str_partition(self, sep: str, from_right: bool = False) -> "Series":
        return _wrap(self._native.str_partition(sep, from_right))

    def dt_part(self, code: int, unit: int = 2) -> "Series":
        return _wrap(self._native.dt_part(code, unit))

    def dt_round(self, every: int, mode: int) -> "Series":
        return _wrap(self._native.dt_round(every, mode))

    def with_timezone(self, tz: str) -> "Series":
        """This Timestamp column under zone ``tz`` (``""`` for naive), the
        same instants and buffers."""
        return _wrap(self._native.with_timezone(tz))

    # -- native property accessors --------------------------------------------
    # These are attributes, not methods: read as ``s.type``, never ``s.type()``.
    @property
    def type(self) -> int:
        """(property, not a method) The native element type of the column, a
        :class:`~dftracer.utils.enums.DType` value. Read as ``s.type``."""
        return self._native.type

    @property
    def encoding(self) -> int:
        """(property, not a method) The native storage encoding (flat,
        dictionary, selection). Read as ``s.encoding``."""
        return self._native.encoding

    @property
    def length(self) -> int:
        """(property, not a method) Number of elements in the column. Read as
        ``s.length``."""
        return self._native.length

    @property
    def null_count(self) -> int:
        """(property, not a method) Number of null elements. Read as
        ``s.null_count``."""
        return self._native.null_count

    @property
    def ops(self) -> "_OpsAccessor":
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
        return DType(self._native.type)

    @property
    def size(self) -> int:
        return self._native.length

    @property
    def shape(self) -> Tuple[int]:
        return (self._native.length,)

    def len(self) -> int:
        return self._native.length

    def to_list(self) -> List[object]:
        return self.to_arrow().to_pylist()

    def tolist(self) -> List[object]:
        return self.to_list()

    def isna(self) -> "Series":
        return _wrap(self._native.null_mask())

    def isnull(self) -> "Series":
        return _wrap(self._native.null_mask())

    def notna(self) -> "Series":
        return _wrap(self._native.valid_mask())

    def notnull(self) -> "Series":
        return _wrap(self._native.valid_mask())

    def isin(self, values: "Union[Series, Sequence[object]]") -> "Series":
        return self.is_in(values)

    def dropna(self) -> "Series":
        return _wrap(self._native.drop_nulls())

    def fill_null(self, value: Union[int, float]) -> "Series":
        return _wrap(self._native.fillna(_unwrap(value)))

    def var(self, ddof: int = 1) -> float:
        """Variance with ``ddof`` degrees of freedom (0 = population, 1 =
        sample, the pandas default); the engine has kernels for those two.
        NaN below ``ddof + 1`` present values, as pandas (the kernel's
        readout is 0)."""
        if self.count() < ddof + 1:
            return float("nan")
        return self._native.variance(_ddof_sample(ddof))

    def std(self, ddof: int = 1) -> float:
        if self.count() < ddof + 1:
            return float("nan")
        return self._native.stddev(_ddof_sample(ddof))

    def skew(self) -> float:
        return self._native.skewness()

    def kurt(self) -> float:
        return self._native.kurtosis()

    def n_unique(self) -> int:
        return self._native.nunique()

    def prod(self) -> Union[int, float]:
        return self._native.product()

    def argmin(self) -> int:
        return self._native.arg_min()

    def argmax(self) -> int:
        return self._native.arg_max()

    def idxmin(self) -> int:
        return self._native.arg_min()

    def idxmax(self) -> int:
        return self._native.arg_max()

    def arg_sort(self, descending: bool = False) -> "Series":
        return _wrap(self._native.argsort(_unwrap(descending)))

    def cumprod(self) -> "Series":
        return _wrap(self._native.cum_prod())

    def cum_sum(self) -> "Series":
        return _wrap(self._native.cumsum())

    def cum_max(self) -> "Series":
        return _wrap(self._native.cummax())

    def cum_min(self) -> "Series":
        return _wrap(self._native.cummin())

    def sort_values(self, ascending: bool = True) -> "Series":
        return _wrap(self._native.sort(not ascending))

    def nlargest(self, n: int = 5) -> "Series":
        return _wrap(self._native.top_k(_unwrap(n)))

    def nsmallest(self, n: int = 5) -> "Series":
        return _wrap(self._native.bottom_k(_unwrap(n)))

    def searchsorted(self, values: "Series") -> "Series":
        return _wrap(self._native.search_sorted(_unwrap(values)))

    def gather(self, indices: "Union[Series, Sequence[int]]") -> "Series":
        return self.take(indices)

    # ``s.str``: string methods under both spellings, pandas (``s.str.lower()``,
    # ``s.str.contains("x")``, ``s.str.len()``) and polars
    # (``s.str.to_lowercase()``, ``s.str.len_bytes()``). Served from the
    # forwarder rather than a property: a class-scope name ``str`` would shadow
    # the builtin in every annotation of this class.
    if not TYPE_CHECKING:

        def __getattr__(self, name):
            if name == "str":
                return _StrAccessor(self)
            if name == "list":
                return _ListAccessor(self)
            if name == "dt":
                return _DtAccessor(self)
            return super().__getattr__(name)

    def __reduce__(self) -> "Tuple[Callable[[object], Series], Tuple[object, ...]]":
        return (_series_from_arrow, (self.to_arrow(),))


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

    def sum(self) -> Series:
        return self._series.rolling_sum(self._window)

    def mean(self) -> Series:
        return self._series.rolling_mean(self._window)

    def min(self) -> Series:
        return self._series.rolling_min(self._window)

    def max(self) -> Series:
        return self._series.rolling_max(self._window)

    def var(self) -> Series:
        return self._series.rolling_var(self._window)

    def std(self) -> Series:
        return self._series.rolling_std(self._window)

    def median(self) -> Series:
        return self._series.rolling_median(self._window)

    def quantile(self, q: float) -> Series:
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

    def count(self) -> Series:
        return self._series.notna().astype("int64").cumsum()

    def mean(self) -> Series:
        return self.sum().astype("float64") / self.count()

    def var(self) -> Series:
        s = self._series.astype("float64")
        n = self.count()
        total = s.cumsum().ffill()
        squares = (s * s).cumsum().ffill()
        # (S2 - S^2 / n) / (n - 1); a null divisor where n < 2.
        divisor = n.sub_scalar(1).where(n.ge(2), n.shift(len(n)))
        return (squares - total * total / n) / divisor

    def std(self) -> Series:
        return self.var().sqrt()


class _Ewm:
    """``s.ewm(alpha)``: the pandas exponentially weighted window object."""

    __slots__ = ("_series", "_alpha")

    def __init__(self, series: Series, alpha: float) -> None:
        self._series = series
        self._alpha = alpha

    def mean(self) -> Series:
        return self._series.ewm_mean(self._alpha)

    def std(self) -> Series:
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
                unit = ("s", "ms", "us", "ns")[series._native.time_unit]
            elif series.dtype == DType.DATE64:
                unit = "ms"
        if unit not in _TIME_UNITS:
            raise ValueError("dt: unit must be s, ms, us or ns")
        self._unit = unit

    def __call__(self, unit: str) -> "_DtAccessor":
        return _DtAccessor(self._s, unit)

    def _part(self, code: int) -> Series:
        return self._s.dt_part(code, _TIME_UNITS[self._unit])

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
                "year": self._part(15).to_arrow(),
                "week": self._part(14).to_arrow(),
                "day": (self.dayofweek + 1).to_arrow(),
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
        return self._s._native.timezone or None

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
    def lower(self) -> Series:
        return self._s.to_lowercase()

    def upper(self) -> Series:
        return self._s.to_uppercase()

    def strip(self) -> Series:
        return self._s.str_strip()

    def lstrip(self) -> Series:
        return self._s.str_lstrip()

    def rstrip(self) -> Series:
        return self._s.str_rstrip()

    def len(self) -> Series:
        """Codepoint count, as pandas ``str.len()``."""
        return self._s.str_len_chars()

    def contains(self, pat: str, case: bool = True, regex: bool = False) -> Series:
        """Substring (or with ``regex=True`` a regex search) predicate."""
        if regex:
            if not case:
                raise NotImplementedError("case-insensitive regex has no kernel; lower() first")
            return self._s.str_search(pat)
        if case:
            return self._s.str_contains(pat)
        return self._s.to_lowercase().str_contains(pat.lower())

    def startswith(self, pat: str) -> Series:
        return self._s.str_starts_with(pat)

    def endswith(self, pat: str) -> Series:
        return self._s.str_ends_with(pat)

    def match(self, pat: str) -> Series:
        """Regex anchored at the start, as pandas ``str.match``."""
        return self._s.str_search("^(?:" + pat + ")")

    def fullmatch(self, pat: str) -> Series:
        return self._s.str_matches(pat)

    def find(self, sub: str) -> Series:
        return self._s.str_find(sub)

    def replace(self, pat: str, repl: str, n: int = -1) -> Series:
        """Literal replace: every occurrence (``n=-1``) or the first (``n=1``)."""
        if n == -1:
            return self._s.str_replace_all(pat, repl)
        if n == 1:
            return self._s.str_replace(pat, repl)
        raise NotImplementedError("replace() supports n=-1 (all) or n=1 (first)")

    def slice(self, start: int = 0, stop: Optional[int] = None) -> Series:
        """Byte substring ``[start, stop)``, as pandas ``str.slice``."""
        length = -1 if stop is None else max(stop - start, 0)
        return self._s.str_slice(start, length)

    def pad(
        self, width: int, side: Literal["left", "right"] = "left", fillchar: str = " "
    ) -> Series:
        if side == "left":
            return self._s.str_pad_start(width, fillchar)
        if side == "right":
            return self._s.str_pad_end(width, fillchar)
        raise ValueError("side must be 'left' or 'right'")

    def zfill(self, width: int) -> Series:
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

    def extract(self, pat: str, group: int = 1) -> Series:
        """The capture ``group`` of the first regex ``pat`` match per row, null
        where there is no match (pandas ``str.extract`` for one group)."""
        return self._s.str_extract(pat, group)

    # polars
    def to_lowercase(self) -> Series:
        return self._s.to_lowercase()

    def to_uppercase(self) -> Series:
        return self._s.to_uppercase()

    def strip_chars(self) -> Series:
        return self._s.str_strip()

    def len_bytes(self) -> Series:
        return self._s.str_len_bytes()

    def len_chars(self) -> Series:
        return self._s.str_len_chars()

    def starts_with(self, prefix: str) -> Series:
        return self._s.str_starts_with(prefix)

    def ends_with(self, suffix: str) -> Series:
        return self._s.str_ends_with(suffix)

    def replace_all(self, pat: str, repl: str) -> Series:
        return self._s.str_replace_all(pat, repl)

    def pad_start(self, width: int, fill: str = " ") -> Series:
        return self._s.str_pad_start(width, fill)

    def pad_end(self, width: int, fill: str = " ") -> Series:
        return self._s.str_pad_end(width, fill)

    # pandas, the rest of the surface (ASCII; a byte outside ASCII is left as
    # it is by the case forms and fails the character classes).
    def capitalize(self) -> Series:
        return self._s.str_case(0)

    def title(self) -> Series:
        return self._s.str_case(1)

    def swapcase(self) -> Series:
        return self._s.str_case(2)

    def casefold(self) -> Series:
        return self._s.to_lowercase()

    def isalnum(self) -> Series:
        return self._s.str_is(0)

    def isalpha(self) -> Series:
        return self._s.str_is(1)

    def isdigit(self) -> Series:
        return self._s.str_is(2)

    def isdecimal(self) -> Series:
        return self._s.str_is(3)

    def isnumeric(self) -> Series:
        return self._s.str_is(4)

    def isspace(self) -> Series:
        return self._s.str_is(5)

    def islower(self) -> Series:
        return self._s.str_is(6)

    def isupper(self) -> Series:
        return self._s.str_is(7)

    def istitle(self) -> Series:
        return self._s.str_is(8)

    def count(self, pat: str) -> Series:
        """Non-overlapping occurrences of the literal ``pat`` per row."""
        return self._s.str_count(pat)

    def rfind(self, sub: str) -> Series:
        """Byte index of the last ``sub`` per row, or -1."""
        return self._s.str_rfind(sub)

    def index(self, sub: str) -> Series:
        """As :meth:`find`, but a row without ``sub`` raises (``ValueError``),
        as Python's ``str.index``."""
        out = self._s.str_find(sub)
        if out.eq(-1).any():
            raise ValueError(f"index: substring {sub!r} not found in every row")
        return out

    def rindex(self, sub: str) -> Series:
        out = self._s.str_rfind(sub)
        if out.eq(-1).any():
            raise ValueError(f"rindex: substring {sub!r} not found in every row")
        return out

    def removeprefix(self, prefix: str) -> Series:
        return self._s.str_remove_prefix(prefix)

    def removesuffix(self, suffix: str) -> Series:
        return self._s.str_remove_suffix(suffix)

    def repeat(self, repeats: int) -> Series:
        return self._s.str_repeat(repeats)

    def center(self, width: int, fillchar: str = " ") -> Series:
        return self._s.str_center(width, fillchar)

    def ljust(self, width: int, fillchar: str = " ") -> Series:
        return self._s.str_pad_end(width, fillchar)

    def rjust(self, width: int, fillchar: str = " ") -> Series:
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
        nulls = Series.from_list([None] * len(s), dtype=_require_pyarrow().string())
        return s.str_slice(i, 1).where(s.str_len_bytes().gt(i), nulls)

    def slice_replace(self, start: int = 0, stop: Optional[int] = None, repl: str = "") -> Series:
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
        return DataFrame(
            {c: Series(wide._native[c]).gt(0).astype("int64").to_arrow() for c in names}
        )


_register(_ext._Series, Series)


def _series_from_arrow(array: "pa.Array") -> Series:
    """Import a pyarrow Array into a native Series wrapper."""
    return Series(_ext._series_from_arrow(array))
