# SPDX-License-Identifier: MIT
"""Accumulator values of a jit map: counters, sums, extrema, sets, top-k."""

from __future__ import annotations

from typing import (
    TYPE_CHECKING,
    Any,
    Generic,
    Tuple,
    overload,
)

from .._jit_errors import JitError
from .types import DFT_DTYPES, Str, T, TypeTag, i64, str_, u64

if TYPE_CHECKING:
    pass


class Monoid:
    """One aggregate of a jit accumulator, lowered to a ``dftu_agg_col``.

    ``dft`` is the ``DFTU_AGG_*`` op code, ``param`` its scalar parameter (a
    quantile, a k, an occupancy cell tolerance), ``needs_by`` marks an op that
    reads a second (ordering / x) column, and ``elem`` is the value column's
    family: ``"none"`` (no value column), ``"i64"``, ``"f64"`` or ``"str"``."""

    __slots__ = ("dft", "param", "needs_by", "elem")

    def __init__(
        self, dft: str, param: float = 0.0, needs_by: bool = False, elem: str = "f64"
    ) -> None:
        self.dft = dft
        self.param = param
        self.needs_by = needs_by
        self.elem = elem


class Counter(Monoid):
    __slots__ = ()

    def __iadd__(self, x: int) -> "Counter":
        raise NotImplementedError

    def observe(self, x: int) -> None: ...


class Sum(Monoid):
    __slots__ = ()

    def __iadd__(self, x: float) -> "Sum":
        raise NotImplementedError

    def observe(self, x: float) -> None: ...


class Min(Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, x: T) -> None: ...


class Max(Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, x: T) -> None: ...


class Distinct(Monoid):
    __slots__ = ()

    def observe(self, x: object) -> None: ...


class SetV(Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, x: T) -> None: ...
    def add(self, x: T) -> None: ...


class ListV(Monoid, Generic[T]):
    __slots__ = ()

    def append(self, x: T, *, order_by: int) -> None: ...


class Mean(Monoid):
    __slots__ = ()

    def observe(self, v: float) -> None: ...


class Variance(Monoid):
    __slots__ = ()

    def observe(self, v: float) -> None: ...


class Stddev(Monoid):
    __slots__ = ()

    def observe(self, v: float) -> None: ...


class AggReduce(Monoid):
    """A reduction with no ``+=`` / ``.observe`` shorthand of its own; it is
    declared as a map value and folded by the engine like any other aggregate."""

    __slots__ = ()


class Quantiles(Monoid):
    """A DDSketch quantile aggregate; ``qs`` materializes one ``p<q*100>``
    column per quantile alongside a ``count`` column."""

    __slots__ = ("qs",)

    def __init__(self, qs: "Tuple[float, ...]") -> None:
        super().__init__("DFTU_AGG_PCT", param=qs[0], elem="f64")
        self.qs = qs

    def observe(self, v: float) -> None: ...


class ArgMin(Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, payload: T, *, by: float) -> None: ...


class ArgMax(Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, payload: T, *, by: float) -> None: ...


class TopK(Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, payload: T, *, by: float) -> None: ...


class BottomK(Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, payload: T, *, by: float) -> None: ...


class ApproxTopK(Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, x: T) -> None: ...


class Sample(Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, x: T) -> None: ...


class _Accum(Monoid):
    """A :class:`Product` component: the permissive union of accumulator ops
    (``+= n`` and ``.observe(...)``), since dict/tuple products do not carry
    per-component monoid types."""

    __slots__ = ()

    def __iadd__(self, x: "int | float") -> "_Accum":
        raise NotImplementedError

    def observe(self, *args: object, **kwargs: object) -> None: ...


class Product(Monoid):
    __slots__ = ()

    # Type-only (runtime access is AST-compiled; a real __setattr__ would break
    # Monoid.__init__). __setattr__ lets `product.cnt += 1` type-check.
    if TYPE_CHECKING:

        def __getitem__(self, i: int) -> _Accum: ...
        def __getattr__(self, name: str) -> _Accum: ...
        def __setattr__(self, name: str, value: _Accum) -> None: ...


def count() -> Counter:
    """An integer counting value: ``+= 1`` counts, ``+= <expr>`` sums.

    Materializes to an int64 column."""
    return Counter("DFTU_AGG_SUM", elem="i64")


def sum() -> Sum:
    """A double-summing value; materializes to an f64 column."""
    return Sum("DFTU_AGG_SUM", elem="f64")


def _minmax_elem(op: str, of: "TypeTag[object]") -> str:
    dtype = DFT_DTYPES.get(of.dft)
    if dtype is None or not dtype.is_numeric:
        raise JitError(f"jit.{op}(of=...) must be a fixed-width int or float type")
    return "f64" if dtype.is_float else "i64"


@overload
def min() -> "Min[int]": ...
@overload
def min(of: "TypeTag[T]") -> "Min[T]": ...
def min(of: "TypeTag[Any]" = u64) -> "Min[Any]":
    """A typed min value (default u64); ``of=`` picks the int or float element.

    ``.observe(v)`` contributes."""
    return Min("DFTU_AGG_MIN", elem=_minmax_elem("min", of))


@overload
def max() -> "Max[int]": ...
@overload
def max(of: "TypeTag[T]") -> "Max[T]": ...
def max(of: "TypeTag[Any]" = u64) -> "Max[Any]":
    """A typed max value (default u64); ``of=`` picks the int or float element.

    ``.observe(v)`` contributes."""
    return Max("DFTU_AGG_MAX", elem=_minmax_elem("max", of))


def minf() -> "Min[float]":
    """A double min value; contribute with ``.observe(v)``."""
    return Min("DFTU_AGG_MIN", elem="f64")


def maxf() -> "Max[float]":
    """A double max value; contribute with ``.observe(v)``."""
    return Max("DFTU_AGG_MAX", elem="f64")


def distinct() -> Distinct:
    """An approximate distinct count (u64); contribute with ``.observe(v)``."""
    return Distinct("DFTU_AGG_DISTINCT", elem="i64")


def bitset() -> Monoid:
    """The bitwise OR of the observed integers; contribute with ``.observe(v)``."""
    return Monoid("DFTU_AGG_BIT_OR", elem="i64")


def prod() -> Monoid:
    """The product of the observed values (f64, 1 when none); contribute with
    ``.observe(v)``."""
    return Monoid("DFTU_AGG_PROD", elem="f64")


def _elem_of(fn: str, of: "TypeTag[object]") -> str:
    if of is i64:
        return "i64"
    if of is str_:
        return "str"
    raise JitError(f"jit.{fn}(of=...) must be jit.str_ or jit.i64")


@overload
def set() -> "SetV[Str]": ...
@overload
def set(of: "TypeTag[T]") -> "SetV[T]": ...
def set(of: "TypeTag[Any]" = str_) -> "SetV[Any]":
    """A set value collecting distinct elements added with ``.observe``.

    ``of=jit.str_`` (default) observes strings, ``of=jit.i64`` int64 values.
    The result is one String cell per group: the distinct values as reprs,
    sorted and joined by ``\\x1e``. Use ``jit.list`` for a list column."""
    return SetV("DFTU_AGG_SET_UNION", elem=_elem_of("set", of))


@overload
def list() -> "ListV[Str]": ...
@overload
def list(of: "TypeTag[T]") -> "ListV[T]": ...
def list(of: "TypeTag[Any]" = str_) -> "ListV[Any]":
    """An ordered-list value collecting elements added with ``.append``.

    ``of=jit.str_`` (default) collects strings, ``of=jit.i64`` int64 values;
    both materialize as a list<string> of reprs sorted by ``order_by``."""
    return ListV("DFTU_AGG_LIST_SORTED", needs_by=True, elem=_elem_of("list", of))


def mean() -> Mean:
    """A mean value; contribute one value with ``.observe(v)``."""
    return Mean("DFTU_AGG_MEAN", elem="f64")


def variance() -> Variance:
    """A sample (n-1) variance value; contribute with ``.observe(v)``."""
    return Variance("DFTU_AGG_VAR", elem="f64")


var = variance


def stddev() -> Stddev:
    """A sample (n-1) standard deviation value; contribute with ``.observe(v)``."""
    return Stddev("DFTU_AGG_STD", elem="f64")


std = stddev


def quantiles(qs: "Tuple[float, ...]" = (0.5, 0.9, 0.95, 0.99)) -> Quantiles:
    """A DDSketch quantile value; contribute with ``.observe(v)``.

    Materializes a ``count`` int64 column plus one f64 column per quantile in
    ``qs`` (each in [0, 1]), named ``p<q*100>`` (``p50``, ``p90``, ``p99``, ..).
    Quantiles are approximate (DDSketch, ~1% relative error). Top-level only:
    it cannot be a product/record component."""
    qs = tuple(float(q) for q in qs)
    if not qs:
        raise JitError("jit.quantiles needs at least one quantile")
    for q in qs:
        if not 0.0 <= q <= 1.0:
            raise JitError(f"jit.quantiles value {q} is outside [0, 1]")
    return Quantiles(qs)


def skew() -> AggReduce:
    """Population skewness of the observed values."""
    return AggReduce("DFTU_AGG_SKEW", elem="f64")


def kurt() -> AggReduce:
    """Excess (population) kurtosis of the observed values."""
    return AggReduce("DFTU_AGG_KURT", elem="f64")


def sumsq() -> AggReduce:
    """Sum of squares (Float64)."""
    return AggReduce("DFTU_AGG_SUMSQ", elem="f64")


def first() -> AggReduce:
    """First non-null value in row order."""
    return AggReduce("DFTU_AGG_FIRST", elem="f64")


def last() -> AggReduce:
    """Last non-null value in row order."""
    return AggReduce("DFTU_AGG_LAST", elem="f64")


def count_valid() -> AggReduce:
    """Count of non-null observed values."""
    return AggReduce("DFTU_AGG_COUNT_VALID", elem="f64")


def hist() -> AggReduce:
    """The DDSketch histogram of the observed values, a
    list<struct{lo, hi, count}> column (mergeable, relative-error buckets)."""
    return AggReduce("DFTU_AGG_HIST", elem="f64")


def busy(cell: float = 0.0) -> AggReduce:
    """Occupancy: interval-union length (us) where overlap depth > 0, over a
    (ts, dur) pair. ``cell`` is the endpoint-snap tolerance in us (0 = exact)."""
    return AggReduce("DFTU_AGG_BUSY", param=float(cell), needs_by=True, elem="f64")


def concurrency(cell: float = 0.0) -> AggReduce:
    """Occupancy: sum(dur) / busy over a (ts, dur) pair."""
    return AggReduce("DFTU_AGG_CONCURRENCY", param=float(cell), needs_by=True, elem="f64")


def utilization(cell: float = 0.0) -> AggReduce:
    """Occupancy: busy / (max_end - min_ts) over a (ts, dur) pair."""
    return AggReduce("DFTU_AGG_UTILIZATION", param=float(cell), needs_by=True, elem="f64")


def active(cell: float = 0.0) -> AggReduce:
    """Occupancy: peak overlap depth over a (ts, dur) pair."""
    return AggReduce("DFTU_AGG_ACTIVE", param=float(cell), needs_by=True, elem="f64")


def corr() -> AggReduce:
    """Pearson correlation of the (y, x) pair; y is the value, x the ``by``."""
    return AggReduce("DFTU_AGG_CORR", needs_by=True, elem="f64")


def covar_pop() -> AggReduce:
    """Population covariance of the (y, x) pair."""
    return AggReduce("DFTU_AGG_COVAR_POP", needs_by=True, elem="f64")


def covar_samp() -> AggReduce:
    """Sample covariance of the (y, x) pair."""
    return AggReduce("DFTU_AGG_COVAR_SAMP", needs_by=True, elem="f64")


def regr_slope() -> AggReduce:
    """Least-squares slope of y on x."""
    return AggReduce("DFTU_AGG_REGR_SLOPE", needs_by=True, elem="f64")


def regr_intercept() -> AggReduce:
    """Least-squares intercept of y on x."""
    return AggReduce("DFTU_AGG_REGR_INTERCEPT", needs_by=True, elem="f64")


def regr_r2() -> AggReduce:
    """Coefficient of determination of the least-squares fit of y on x."""
    return AggReduce("DFTU_AGG_REGR_R2", needs_by=True, elem="f64")


@overload
def argmin() -> "ArgMin[Str]": ...
@overload
def argmin(of: "TypeTag[T]") -> "ArgMin[T]": ...
def argmin(of: "TypeTag[Any]" = str_) -> "ArgMin[Any]":
    """An argmin value: keep the payload whose ``by=`` key is smallest.

    ``of=jit.str_`` (default) observes a string payload, ``of=jit.i64`` an
    int64 one; the result is the payload's String repr. Contribute with
    ``.observe(payload, by=<expr>)``."""
    return ArgMin("DFTU_AGG_ARGMIN", needs_by=True, elem=_elem_of("argmin", of))


@overload
def argmax() -> "ArgMax[Str]": ...
@overload
def argmax(of: "TypeTag[T]") -> "ArgMax[T]": ...
def argmax(of: "TypeTag[Any]" = str_) -> "ArgMax[Any]":
    """An argmax value: keep the payload whose ``by=`` key is largest.

    ``of=jit.str_`` (default) observes a string payload, ``of=jit.i64`` an
    int64 one; the result is the payload's String repr. Contribute with
    ``.observe(payload, by=<expr>)``."""
    return ArgMax("DFTU_AGG_ARGMAX", needs_by=True, elem=_elem_of("argmax", of))


def _check_k(op: str, k: int) -> float:
    if not isinstance(k, int) or isinstance(k, bool) or k <= 0:
        raise JitError(f"jit.{op}(k, ...) k must be a positive integer")
    return float(k)


@overload
def topk(k: int) -> "TopK[Str]": ...
@overload
def topk(k: int, of: "TypeTag[T]") -> "TopK[T]": ...
def topk(k: int, of: "TypeTag[Any]" = str_) -> "TopK[Any]":
    """A bounded top-k value: keep the k payloads at the k largest ``by=`` keys.

    ``of=jit.str_`` (default) observes string payloads, ``of=jit.i64`` int64
    ones; the result is a list<string> of their reprs. Contribute with
    ``.observe(payload, by=<expr>)``."""
    return TopK(
        "DFTU_AGG_TOPK", param=_check_k("topk", k), needs_by=True, elem=_elem_of("topk", of)
    )


@overload
def bottomk(k: int) -> "BottomK[Str]": ...
@overload
def bottomk(k: int, of: "TypeTag[T]") -> "BottomK[T]": ...
def bottomk(k: int, of: "TypeTag[Any]" = str_) -> "BottomK[Any]":
    """A bounded bottom-k value: keep the k payloads at the k smallest ``by=``
    keys, mirroring :func:`topk`."""
    return BottomK(
        "DFTU_AGG_BOTTOMK",
        param=_check_k("bottomk", k),
        needs_by=True,
        elem=_elem_of("bottomk", of),
    )


@overload
def approx_topk(k: int) -> "ApproxTopK[Str]": ...
@overload
def approx_topk(k: int, of: "TypeTag[T]") -> "ApproxTopK[T]": ...
def approx_topk(k: int, of: "TypeTag[Any]" = str_) -> "ApproxTopK[Any]":
    """An approximate heavy-hitters value: the k most FREQUENT observed values
    (SpaceSaving), in bounded memory. Contribute with ``.observe(value)``."""
    return ApproxTopK(
        "DFTU_AGG_APPROX_TOPK",
        param=_check_k("approx_topk", k),
        elem=_elem_of("approx_topk", of),
    )


@overload
def sample(k: int) -> "Sample[Str]": ...
@overload
def sample(k: int, of: "TypeTag[T]") -> "Sample[T]": ...
def sample(k: int, of: "TypeTag[Any]" = str_) -> "Sample[Any]":
    """A deterministic mergeable sample of k items, a list<string> of their
    reprs; contribute with ``.observe(item)``."""
    return Sample("DFTU_AGG_SAMPLE", param=_check_k("sample", k), elem=_elem_of("sample", of))
