# SPDX-License-Identifier: MIT
"""Author a DFTracer plugin in Python and get a native one.

Decorate a class with :func:`plugin`; its scan state and its one
:func:`each_event` or :func:`each_batch` method are AST-compiled to a C plugin
against the stable ABI, built to a cached ``.so``, and loaded through
:class:`dftracer.utils.plugins.Plugins` exactly like a hand-written plugin.

A body is a deliberately small subset: conditions, locals, ``raise`` and ``try``,
files and JSON values, then state updates. Anything outside it raises
:class:`JitError` at decoration time, with the line and column, never a silent
miscompile. The guide is ``docs/source/jit.rst``.

Scan state is a :func:`map` descriptor or, with the merge inferred from the update
code, an annotated plain field (``counts: Dict[int, int] = defaultdict(int)``,
``names: Set[str] = set()``, ``total: int = 0``); a persistent key-value field is
``seen: Store[int, int]``, and the settings of a load are ``config: Cfg`` for a
dataclass ``Cfg``. :func:`finalize` and :func:`task` methods run after the scan, and
:func:`sender`, :func:`receiver`, :func:`latest` and :func:`phase` connect plugins.

Every :func:`map` is one named accumulator on the dataframe engine's ``AggState``,
reached through the host's ``DFTU_SVC_AGG`` service: the key tuple names its
grouping columns (an empty tuple makes it a whole-scan scalar) and each value
reduction is one aggregate. The host merges same-named accumulators across worker
slices and finalizes each to a frame.

Example::

    @jit.plugin
    class NameEdges:
        edges = jit.map(key=(jit.i64, jit.str_), value=jit.count())

        @jit.each_event
        def step(self, e):
            if e.fhash != jit.NONE:
                self.edges[(e.pid, e.name)] += 1
"""

from __future__ import annotations

import ast
import builtins
import collections
import dataclasses
import inspect
import math
import subprocess
import sys
import textwrap
import types
from pathlib import Path
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    ClassVar,
    Dict,
    Generic,
    Iterator,
    List,
    Literal,
    Mapping,
    NamedTuple,
    NoReturn,
    Optional,
    Protocol,
    Tuple,
    TypeVar,
    Union,
    cast,
    get_args,
    get_origin,
    get_type_hints,
    overload,
)

from .. import _plugin_build, jit_op
from .._c_text import c_str_literal as _c_str_literal
from .._jit_errors import Cancelled as Cancelled
from .._jit_errors import Closed as Closed
from .._jit_errors import Error as Error
from .._jit_errors import Fatal as Fatal
from .._jit_errors import IoError as IoError
from .._jit_errors import JitError as JitError
from .._jit_errors import NotFound as NotFound
from .._jit_errors import ParseError as ParseError
from .._jit_errors import SkipRest as SkipRest
from .._jit_errors import _class_error, _Source
from ..expr import Expr, col
from ..jit_op import Op, compile_op, op, run_op
from . import _channels, _flow
from . import ops as ops
from ._channels import Channel as Channel
from ._channels import Latest as Latest
from ._channels import Phase as Phase
from ._channels import PortDecl as _PortDecl
from ._channels import Receiver as Receiver
from ._channels import Sender as Sender
from ._channels import channel as channel
from ._channels import latest as latest
from ._channels import phase as phase
from ._channels import receiver as receiver
from ._channels import sender as sender
from ._flow import File as File
from ._flow import FlowFn as _FlowFn
from ._flow import TaskSig as _TaskSig
from ._flow import Value as Value
from ._flow import all as all
from ._flow import any as any
from ._flow import emit as emit
from ._flow import finalize as finalize
from ._flow import json as json
from ._flow import open as open
from ._flow import sleep as sleep
from ._flow import task as task
from ._flow import yaml as yaml

if TYPE_CHECKING:
    from ..expr import Expr
    from ..plugins import JSONValue
    from ._batch import BatchHook
    from .ops import Result

# Python 3.8 wraps a subscript's slice in ast.Index; 3.9+ stores the value
# directly, and 3.14 may drop the class entirely, so resolve it defensively.
_AST_INDEX = getattr(ast, "Index", None)


def _unwrap_index(node: ast.expr) -> ast.expr:
    # _AST_INDEX is resolved at runtime, so the type checker cannot narrow node
    # to ast.Index here; read .value dynamically.
    if _AST_INDEX is not None and isinstance(node, _AST_INDEX):
        return cast(ast.expr, getattr(node, "value", node))
    return node


__all__ = [
    "JitError",
    "Error",
    "NotFound",
    "IoError",
    "ParseError",
    "Fatal",
    "SkipRest",
    "Cancelled",
    "task",
    "finalize",
    "emit",
    "sleep",
    "all",
    "any",
    "open",
    "json",
    "yaml",
    "File",
    "Value",
    "Op",
    "op",
    "ops",
    "compile_op",
    "run_op",
    "plugin",
    "JitPackage",
    "build",
    "map",
    "publish",
    "consume",
    "Port",
    "Sender",
    "Receiver",
    "Channel",
    "Latest",
    "Phase",
    "sender",
    "receiver",
    "channel",
    "latest",
    "phase",
    "Closed",
    "count",
    "sum",
    "min",
    "max",
    "minf",
    "maxf",
    "distinct",
    "set",
    "list",
    "mean",
    "variance",
    "var",
    "stddev",
    "std",
    "quantiles",
    "skew",
    "kurt",
    "sumsq",
    "hist",
    "busy",
    "concurrency",
    "utilization",
    "active",
    "first",
    "last",
    "count_valid",
    "argmin",
    "argmax",
    "topk",
    "bottomk",
    "approx_topk",
    "sample",
    "bitset",
    "prod",
    "corr",
    "covar_pop",
    "covar_samp",
    "regr_slope",
    "regr_intercept",
    "regr_r2",
    "record",
    "each_event",
    "vfold",
    "each_batch",
    "series",
    # numeric primitives
    "ilog2",
    "bit_width",
    "clz",
    "ctz",
    "popcount",
    "ceil_pow2",
    "floor_pow2",
    "rotl",
    "rotr",
    "abs",
    "clamp",
    "div_ceil",
    "div_round",
    "align_up",
    "align_down",
    "isqrt",
    "gcd",
    "mul_hi",
    "mix64",
    "fastrange",
    "fmin",
    "fmax",
    "clampf",
    "sqrt",
    "log2",
    "log",
    "exp",
    "fma",
    "lerp",
    "copysign",
    "Map",
    "Store",
    "Event",
    "Frame",
    "Column",
    "Mask",
    "Str",
    "Counter",
    "Sum",
    "Min",
    "Max",
    "Distinct",
    "SetV",
    "ListV",
    "Mean",
    "Variance",
    "Stddev",
    "Quantiles",
    "ArgMin",
    "ArgMax",
    "TopK",
    "BottomK",
    "ApproxTopK",
    "Sample",
    "Product",
    "i64",
    "str_",
    "bytes",
    "i8",
    "i16",
    "i32",
    "u8",
    "u16",
    "u32",
    "u64",
    "f32",
    "f64",
    "NONE",
]


JitError.__module__ = __name__
for _runtime_exc in (Error, NotFound, Closed, IoError, ParseError, Fatal, SkipRest, Cancelled):
    _runtime_exc.__module__ = __name__

T = TypeVar("T")
T_co = TypeVar("T_co", covariant=True)
K = TypeVar("K")
N = TypeVar("N", int, float)
V = TypeVar("V", bound="_Monoid | type[object]")
K1 = TypeVar("K1")
K2 = TypeVar("K2")
K3 = TypeVar("K3")
K4 = TypeVar("K4")
_C = TypeVar("_C")
SK = TypeVar("SK")
SV = TypeVar("SV")
_MISSING: Any = object()


class Str(str):
    """An interned-string id: usable as a STR key / SET_STR|LIST_STR element and
    comparable to a ``str`` literal. Authoring-only, never instantiated; it is a
    ``str`` to the type checker so a plain ``Set[str]`` or ``Dict[Tuple[int, str], int]``
    state field takes ``e.name``."""

    __slots__ = ()


class _Type(Generic[T_co]):
    """Phantom type marker: the parameter tracks the Python type a jit column
    maps to (u64 -> int, str_ -> str) and is never consumed, so it is
    covariant - _Type[int] is a _Type[object]."""

    __slots__ = ("dft",)

    def __init__(self, dft: str) -> None:
        self.dft = dft


i64: _Type[int] = _Type("DFTU_T_I64")
str_: _Type[Str] = _Type("DFTU_T_STR")
bytes: "_Type[builtins.bytes]" = _Type("DFTU_T_BYTES")

i8: _Type[int] = _Type("DFTU_T_I8")
i16: _Type[int] = _Type("DFTU_T_I16")
i32: _Type[int] = _Type("DFTU_T_I32")
u8: _Type[int] = _Type("DFTU_T_U8")
u16: _Type[int] = _Type("DFTU_T_U16")
u32: _Type[int] = _Type("DFTU_T_U32")
u64: _Type[int] = _Type("DFTU_T_U64")
f32: _Type[float] = _Type("DFTU_T_F32")
f64: _Type[float] = _Type("DFTU_T_F64")


class _Sentinel:
    __slots__ = ()

    # Custom equality keeps ``e.fhash != jit.NONE`` valid for a ``str``-typed Str
    # under mypy --strict (strict equality skips an operand that defines __eq__).
    def __eq__(self, other: object) -> bool:
        return self is other

    def __hash__(self) -> int:
        return id(self)


NONE = _Sentinel()


class _Monoid:
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


class Counter(_Monoid):
    __slots__ = ()

    def __iadd__(self, x: int) -> "Counter":
        raise NotImplementedError

    def observe(self, x: int) -> None: ...


class Sum(_Monoid):
    __slots__ = ()

    def __iadd__(self, x: float) -> "Sum":
        raise NotImplementedError

    def observe(self, x: float) -> None: ...


class Min(_Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, x: T) -> None: ...


class Max(_Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, x: T) -> None: ...


class Distinct(_Monoid):
    __slots__ = ()

    def observe(self, x: object) -> None: ...


class SetV(_Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, x: T) -> None: ...
    def add(self, x: T) -> None: ...


class ListV(_Monoid, Generic[T]):
    __slots__ = ()

    def append(self, x: T, *, order_by: int) -> None: ...


class Mean(_Monoid):
    __slots__ = ()

    def observe(self, v: float) -> None: ...


class Variance(_Monoid):
    __slots__ = ()

    def observe(self, v: float) -> None: ...


class Stddev(_Monoid):
    __slots__ = ()

    def observe(self, v: float) -> None: ...


class AggReduce(_Monoid):
    """A reduction with no ``+=`` / ``.observe`` shorthand of its own; it is
    declared as a map value and folded by the engine like any other aggregate."""

    __slots__ = ()


class Quantiles(_Monoid):
    """A DDSketch quantile aggregate; ``qs`` materializes one ``p<q*100>``
    column per quantile alongside a ``count`` column."""

    __slots__ = ("qs",)

    def __init__(self, qs: "Tuple[float, ...]") -> None:
        super().__init__("DFTU_AGG_PCT", param=qs[0], elem="f64")
        self.qs = qs

    def observe(self, v: float) -> None: ...


class ArgMin(_Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, payload: T, *, by: float) -> None: ...


class ArgMax(_Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, payload: T, *, by: float) -> None: ...


class TopK(_Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, payload: T, *, by: float) -> None: ...


class BottomK(_Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, payload: T, *, by: float) -> None: ...


class ApproxTopK(_Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, x: T) -> None: ...


class Sample(_Monoid, Generic[T]):
    __slots__ = ()

    def observe(self, x: T) -> None: ...


class _Accum(_Monoid):
    """A :class:`Product` component: the permissive union of accumulator ops
    (``+= n`` and ``.observe(...)``), since dict/tuple products do not carry
    per-component monoid types."""

    __slots__ = ()

    def __iadd__(self, x: "int | float") -> "_Accum":
        raise NotImplementedError

    def observe(self, *args: object, **kwargs: object) -> None: ...


class Product(_Monoid):
    __slots__ = ()

    # Type-only (runtime access is AST-compiled; a real __setattr__ would break
    # _Monoid.__init__). __setattr__ lets `product.cnt += 1` type-check.
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


# ---- numeric primitives -----------------------------------------------------
# Authoring markers, not implementations. Inside @jit.each_event each call is
# AST-lowered to the matching prims.h helper (see _PRIM_C) - the C in prims.h is
# the single source of truth, so there is no Python reimplementation to drift
# from it. Calling one directly in Python raises, just like a monoid factory's
# value cannot be read at Python runtime.


def _prim(name: str) -> Callable[..., int]:
    def marker(*args: int) -> int:
        raise JitError(
            f"jit.{name} is a plugin primitive; use it inside a @jit.each_event "
            f"body, where it lowers to dftu_{name}_* in C."
        )

    marker.__name__ = name
    return marker


ilog2 = _prim("ilog2")
"""Floor of log2(x): the highest set-bit index - the natural log-scale histogram
bucket. Lowers to ``dftu_ilog2_u64``."""

bit_width = _prim("bit_width")
"""Bits needed to represent x (``ilog2 + 1``); 0 for 0. ``dftu_bit_width_u64``."""

clz = _prim("clz")
"""Leading zero bits of a 64-bit x (64 for 0). ``dftu_clz_u64``."""

ctz = _prim("ctz")
"""Trailing zero bits of a 64-bit x (64 for 0). ``dftu_ctz_u64``."""

popcount = _prim("popcount")
"""Number of set bits in a 64-bit x. ``dftu_popcount_u64``."""

ceil_pow2 = _prim("ceil_pow2")
"""Smallest power of two >= x (1 for 0). ``dftu_ceil_pow2_u64``."""

floor_pow2 = _prim("floor_pow2")
"""Largest power of two <= x; 0 for 0. ``dftu_floor_pow2_u64``."""

rotl = _prim("rotl")
"""Rotate a 64-bit x left by r bits (mod 64). ``dftu_rotl_u64``."""

rotr = _prim("rotr")
"""Rotate a 64-bit x right by r bits (mod 64). ``dftu_rotr_u64``."""

abs = _prim("abs")
"""Absolute value. ``dftu_abs_i64``."""

clamp = _prim("clamp")
"""Clamp x to the inclusive range [lo, hi]. ``dftu_clamp_i64``."""

div_ceil = _prim("div_ceil")
"""Ceiling of a / b; 0 when b is 0. ``dftu_div_ceil_i64``."""

div_round = _prim("div_round")
"""a / b rounded to nearest, half away from zero; 0 when b is 0. ``dftu_div_round_i64``."""

align_up = _prim("align_up")
"""Round x up to the next multiple of a; x when a is 0. ``dftu_align_up_u64``."""

align_down = _prim("align_down")
"""Round x down to the previous multiple of a; x when a is 0. ``dftu_align_down_u64``."""

isqrt = _prim("isqrt")
"""Integer square root (largest r with r*r <= x). ``dftu_isqrt_u64``."""

gcd = _prim("gcd")
"""Greatest common divisor. ``dftu_gcd_u64``."""

mul_hi = _prim("mul_hi")
"""High 64 bits of the 128-bit product a*b. ``dftu_mul_hi_u64``."""

mix64 = _prim("mix64")
"""SplitMix64 finalizer: a fast, well-avalanched hash of x - good for
hash-bucketing or deterministic sampling. ``dftu_mix64``."""

fastrange = _prim("fastrange")
"""Map x uniformly into [0, n) without a modulo (Lemire's reduction); pair with
:func:`mix64` for hash bucketing. 0 when n is 0. ``dftu_fastrange_u64``."""


def _primf(name: str) -> Callable[..., float]:
    def marker(*args: float) -> float:
        raise JitError(
            f"jit.{name} is a plugin primitive; use it inside a @jit.each_event "
            f"body, where it lowers to dftu_{name}_* in C."
        )

    marker.__name__ = name
    return marker


fmin = _primf("fmin")
"""Lesser of two doubles. ``dftu_min_f64``."""

fmax = _primf("fmax")
"""Greater of two doubles. ``dftu_max_f64``."""

clampf = _primf("clampf")
"""Clamp a double x to the inclusive range [lo, hi]. ``dftu_clamp_f64``."""

sqrt = _primf("sqrt")
"""Square root. ``dftu_sqrt_f64``."""

log2 = _primf("log2")
"""Base-2 logarithm (contrast :func:`ilog2`, the integer bit index). ``dftu_log2_f64``."""

log = _primf("log")
"""Natural logarithm. ``dftu_log_f64``."""

exp = _primf("exp")
"""Natural exponential. ``dftu_exp_f64``."""

fma = _primf("fma")
"""Fused multiply-add ``a*b + c`` with a single rounding. ``dftu_fma_f64``."""

lerp = _primf("lerp")
"""Linear interpolation from a to b by t (unclamped). ``dftu_lerp_f64``."""

copysign = _primf("copysign")
"""Magnitude of x with the sign of y. ``dftu_copysign_f64``."""


_MINMAX_WIDTHS = frozenset(
    {
        "DFTU_T_I8",
        "DFTU_T_I16",
        "DFTU_T_I32",
        "DFTU_T_I64",
        "DFTU_T_U8",
        "DFTU_T_U16",
        "DFTU_T_U32",
        "DFTU_T_U64",
        "DFTU_T_F32",
        "DFTU_T_F64",
    }
)

_FLOAT_TYPES = frozenset({"DFTU_T_F32", "DFTU_T_F64"})


def _minmax_elem(op: str, of: "_Type[object]") -> str:
    if of.dft not in _MINMAX_WIDTHS:
        raise JitError(f"jit.{op}(of=...) must be a fixed-width int or float type")
    return "f64" if of.dft in _FLOAT_TYPES else "i64"


@overload
def min() -> "Min[int]": ...
@overload
def min(of: "_Type[T]") -> "Min[T]": ...
def min(of: "_Type[Any]" = u64) -> "Min[Any]":
    """A typed min value (default u64); ``of=`` picks the int or float element.

    ``.observe(v)`` contributes."""
    return Min("DFTU_AGG_MIN", elem=_minmax_elem("min", of))


@overload
def max() -> "Max[int]": ...
@overload
def max(of: "_Type[T]") -> "Max[T]": ...
def max(of: "_Type[Any]" = u64) -> "Max[Any]":
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


def bitset() -> _Monoid:
    """The bitwise OR of the observed integers; contribute with ``.observe(v)``."""
    return _Monoid("DFTU_AGG_BIT_OR", elem="i64")


def prod() -> _Monoid:
    """The product of the observed values (f64, 1 when none); contribute with
    ``.observe(v)``."""
    return _Monoid("DFTU_AGG_PROD", elem="f64")


def _elem_of(fn: str, of: "_Type[object]") -> str:
    if of is i64:
        return "i64"
    if of is str_:
        return "str"
    raise JitError(f"jit.{fn}(of=...) must be jit.str_ or jit.i64")


@overload
def set() -> "SetV[Str]": ...
@overload
def set(of: "_Type[T]") -> "SetV[T]": ...
def set(of: "_Type[Any]" = str_) -> "SetV[Any]":
    """A set value collecting distinct elements added with ``.observe``.

    ``of=jit.str_`` (default) observes strings, ``of=jit.i64`` int64 values.
    The result is one String cell per group: the distinct values as reprs,
    sorted and joined by ``\\x1e``. Use ``jit.list`` for a list column."""
    return SetV("DFTU_AGG_SET_UNION", elem=_elem_of("set", of))


@overload
def list() -> "ListV[Str]": ...
@overload
def list(of: "_Type[T]") -> "ListV[T]": ...
def list(of: "_Type[Any]" = str_) -> "ListV[Any]":
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
def argmin(of: "_Type[T]") -> "ArgMin[T]": ...
def argmin(of: "_Type[Any]" = str_) -> "ArgMin[Any]":
    """An argmin value: keep the payload whose ``by=`` key is smallest.

    ``of=jit.str_`` (default) observes a string payload, ``of=jit.i64`` an
    int64 one; the result is the payload's String repr. Contribute with
    ``.observe(payload, by=<expr>)``."""
    return ArgMin("DFTU_AGG_ARGMIN", needs_by=True, elem=_elem_of("argmin", of))


@overload
def argmax() -> "ArgMax[Str]": ...
@overload
def argmax(of: "_Type[T]") -> "ArgMax[T]": ...
def argmax(of: "_Type[Any]" = str_) -> "ArgMax[Any]":
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
def topk(k: int, of: "_Type[T]") -> "TopK[T]": ...
def topk(k: int, of: "_Type[Any]" = str_) -> "TopK[Any]":
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
def bottomk(k: int, of: "_Type[T]") -> "BottomK[T]": ...
def bottomk(k: int, of: "_Type[Any]" = str_) -> "BottomK[Any]":
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
def approx_topk(k: int, of: "_Type[T]") -> "ApproxTopK[T]": ...
def approx_topk(k: int, of: "_Type[Any]" = str_) -> "ApproxTopK[Any]":
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
def sample(k: int, of: "_Type[T]") -> "Sample[T]": ...
def sample(k: int, of: "_Type[Any]" = str_) -> "Sample[Any]":
    """A deterministic mergeable sample of k items, a list<string> of their
    reprs; contribute with ``.observe(item)``."""
    return Sample("DFTU_AGG_SAMPLE", param=_check_k("sample", k), elem=_elem_of("sample", of))


class Map(Generic[K, V]):
    __slots__ = ("key_types", "values", "is_product", "value_names")

    def __init__(
        self,
        key_types: "Tuple[_Type[object], ...]",
        values: Tuple[_Monoid, ...],
        is_product: bool,
        value_names: Tuple[str, ...] | None = None,
    ) -> None:
        self.key_types = key_types
        self.values = values
        self.is_product = is_product
        self.value_names = value_names

    @overload
    def __getitem__(self, key: K) -> V: ...
    @overload
    def __getitem__(self, key: object) -> V: ...
    def __getitem__(self, key: object) -> V:
        raise NotImplementedError

    @overload
    def __setitem__(self, key: K, value: V) -> None: ...
    @overload
    def __setitem__(self, key: object, value: V) -> None: ...
    def __setitem__(self, key: object, value: V) -> None: ...


_MapDecl = Map
_AnyMap = Map[Any, Any]


class Event(Protocol):
    """Typed per-event surface for a ``@jit.each_event`` method. Authoring-only:
    the method body is AST-compiled, never executed, so nothing is instantiated."""

    pid: int
    tid: int
    ts: int
    dur: int
    phase: int
    has_dur: bool
    cat: Str
    name: Str
    fhash: Str
    hhash: Str

    def arg_i64(self, name: str) -> int: ...
    def arg_f64(self, name: str) -> float: ...
    def arg_str(self, name: str) -> Str: ...


class Mask(Protocol):
    """A Bool column built from comparisons in an ``@jit.each_batch`` body.
    Authoring-only: the body is AST-compiled, never executed."""

    def __and__(self, other: "Mask") -> "Mask": ...
    def __or__(self, other: "Mask") -> "Mask": ...
    def __invert__(self) -> "Mask": ...
    def sum(self) -> int: ...
    def count(self) -> int: ...


class Column(Protocol[N]):
    """A numeric column of an ``@jit.each_batch`` body: ``Column[int]`` or
    ``Column[float]``. Arithmetic and comparisons build lazy column
    expressions; ``s[mask]`` filters; a reduction returns a scalar to fold into
    scan state. Authoring-only: the body is AST-compiled, never executed."""

    @overload
    def __add__(self, other: "int | Column[int]") -> "Column[N]": ...
    @overload
    def __add__(self, other: "float | Column[float]") -> "Column[float]": ...
    @overload
    def __sub__(self, other: "int | Column[int]") -> "Column[N]": ...
    @overload
    def __sub__(self, other: "float | Column[float]") -> "Column[float]": ...
    @overload
    def __mul__(self, other: "int | Column[int]") -> "Column[N]": ...
    @overload
    def __mul__(self, other: "float | Column[float]") -> "Column[float]": ...
    @overload
    def __radd__(self, other: int) -> "Column[N]": ...
    @overload
    def __radd__(self, other: float) -> "Column[float]": ...
    @overload
    def __rsub__(self, other: int) -> "Column[N]": ...
    @overload
    def __rsub__(self, other: float) -> "Column[float]": ...
    @overload
    def __rmul__(self, other: int) -> "Column[N]": ...
    @overload
    def __rmul__(self, other: float) -> "Column[float]": ...
    def __truediv__(self, other: "float | Column[int] | Column[float]") -> "Column[float]": ...
    def __rtruediv__(self, other: float) -> "Column[float]": ...
    def __neg__(self) -> "Column[N]": ...
    def __abs__(self) -> "Column[N]": ...
    def __lt__(self, other: "float | Column[int] | Column[float]") -> Mask: ...
    def __le__(self, other: "float | Column[int] | Column[float]") -> Mask: ...
    def __gt__(self, other: "float | Column[int] | Column[float]") -> Mask: ...
    def __ge__(self, other: "float | Column[int] | Column[float]") -> Mask: ...
    def __eq__(  # type: ignore[override]  # ty: ignore[incompatible-method-override]
        self, other: "float | Column[int] | Column[float]"
    ) -> Mask: ...
    def __ne__(  # type: ignore[override]  # ty: ignore[incompatible-method-override]
        self, other: "float | Column[int] | Column[float]"
    ) -> Mask: ...
    def __getitem__(self, mask: Mask) -> "Column[N]": ...
    def abs(self) -> "Column[N]": ...
    def clip(self, lo: N, hi: N) -> "Column[N]": ...
    def isnull(self) -> Mask: ...
    def notnull(self) -> Mask: ...
    def fill_null(self, value: N) -> "Column[N]": ...
    def where(self, mask: Mask, other: "N | Column[N]") -> "Column[N]": ...
    def sum(self) -> N: ...
    def min(self) -> N: ...
    def max(self) -> N: ...
    def count(self) -> int: ...
    def mean(self) -> float: ...


class Frame(Protocol):
    """The scan batch an ``@jit.each_batch`` method receives: ``df["dur"]`` is
    a numeric column. The scan columns ``pid``, ``tid``, ``ts``, ``dur`` and
    ``ph`` are ``Column[int]``; an ``args.<key>`` column is typed
    ``Column[float]``, and a sum of it folds into an int field only when the
    column holds integers at run time."""

    @overload
    def __getitem__(  # type: ignore[overload-overlap]
        self, name: Literal["pid", "tid", "ts", "dur", "ph"]
    ) -> Column[int]: ...
    @overload
    def __getitem__(self, name: str) -> Column[float]: ...


class _Record:
    __slots__ = ("names", "monoids")

    def __init__(self, names: Tuple[str, ...], monoids: Tuple[_Monoid, ...]) -> None:
        self.names = names
        self.monoids = monoids


def record(cls: type[_C]) -> type[_C]:
    """Class decorator: turn monoid-valued attributes into a named product value.

    Each attribute (``count = jit.count()``) declares one value component in
    declaration order; pass the class as a :func:`map` ``value``."""
    fields = {k: v for k, v in vars(cls).items() if isinstance(v, _Monoid)}
    if not fields:
        raise JitError("@jit.record needs at least one monoid attribute such as jit.count()")
    setattr(cls, "_jit_record", _Record(tuple(fields), tuple(fields.values())))
    return cls


def _value_spec(value: object) -> Tuple[Tuple[_Monoid, ...], bool, Tuple[str, ...] | None]:
    if isinstance(value, _Monoid):
        return (value,), False, None
    rec = getattr(value, "_jit_record", None)
    if isinstance(rec, _Record):
        return rec.monoids, True, rec.names
    if isinstance(value, dict):
        if not value or not builtins.all(isinstance(v, _Monoid) for v in value.values()):
            raise JitError("map value dict must map names to monoids such as jit.count()")
        return tuple(value.values()), True, tuple(value.keys())
    if isinstance(value, tuple) and value and builtins.all(isinstance(v, _Monoid) for v in value):
        return value, True, None
    raise JitError("map value must be a monoid, a tuple/dict of monoids, or a @jit.record")


# A dict/tuple of monoids is a product (value type Product); these precede the
# generic `value: V` forms so it is not typed as a bare dict/tuple.
@overload
def map(*, key: "_Type[K1]", value: "Dict[str, _Monoid]") -> "Map[Tuple[K1], Product]": ...
@overload
def map(*, key: "_Type[K1]", value: "Tuple[_Monoid, ...]") -> "Map[Tuple[K1], Product]": ...
@overload
def map(*, key: "Tuple[_Type[K1]]", value: "Dict[str, _Monoid]") -> "Map[Tuple[K1], Product]": ...
@overload
def map(*, key: "Tuple[_Type[K1]]", value: "Tuple[_Monoid, ...]") -> "Map[Tuple[K1], Product]": ...
@overload
def map(
    *, key: "Tuple[_Type[K1], _Type[K2]]", value: "Dict[str, _Monoid]"
) -> "Map[Tuple[K1, K2], Product]": ...
@overload
def map(
    *, key: "Tuple[_Type[K1], _Type[K2]]", value: "Tuple[_Monoid, ...]"
) -> "Map[Tuple[K1, K2], Product]": ...
@overload
def map(
    *, key: "Tuple[_Type[K1], _Type[K2], _Type[K3]]", value: "Dict[str, _Monoid]"
) -> "Map[Tuple[K1, K2, K3], Product]": ...
@overload
def map(
    *, key: "Tuple[_Type[K1], _Type[K2], _Type[K3]]", value: "Tuple[_Monoid, ...]"
) -> "Map[Tuple[K1, K2, K3], Product]": ...
@overload
def map(
    *, key: "Tuple[_Type[K1], _Type[K2], _Type[K3], _Type[K4]]", value: "Dict[str, _Monoid]"
) -> "Map[Tuple[K1, K2, K3, K4], Product]": ...
@overload
def map(
    *, key: "Tuple[_Type[K1], _Type[K2], _Type[K3], _Type[K4]]", value: "Tuple[_Monoid, ...]"
) -> "Map[Tuple[K1, K2, K3, K4], Product]": ...
@overload
def map(*, key: "_Type[K1]", value: V) -> "Map[Tuple[K1], V]": ...
@overload
def map(*, key: "Tuple[_Type[K1]]", value: V) -> "Map[Tuple[K1], V]": ...
@overload
def map(*, key: "Tuple[_Type[K1], _Type[K2]]", value: V) -> "Map[Tuple[K1, K2], V]": ...
@overload
def map(
    *, key: "Tuple[_Type[K1], _Type[K2], _Type[K3]]", value: V
) -> "Map[Tuple[K1, K2, K3], V]": ...
@overload
def map(
    *, key: "Tuple[_Type[K1], _Type[K2], _Type[K3], _Type[K4]]", value: V
) -> "Map[Tuple[K1, K2, K3, K4], V]": ...
@overload
def map(
    *, key: "Tuple[_Type[object], ...]", value: "Dict[str, _Monoid]"
) -> "Map[Tuple[object, ...], Product]": ...
@overload
def map(
    *, key: "Tuple[_Type[object], ...]", value: "Tuple[_Monoid, ...]"
) -> "Map[Tuple[object, ...], Product]": ...
@overload
def map(*, key: "Tuple[_Type[object], ...]", value: V) -> "Map[Tuple[object, ...], V]": ...
def map(key: "Tuple[_Type[Any], ...] | _Type[Any]", value: object) -> "Map[Any, Any]":
    """Declare a keyed accumulator: a typed-tuple key, and either one value
    reduction, a tuple of reductions (a positional product), a dict of
    name->reduction, or a :func:`record` class (both named products).

    It lowers to the engine's ``AggState`` grouped by the key columns, one
    aggregate per value reduction; the host merges same-named accumulators
    across worker slices and finalizes each to a native DataFrame.

    A single-component key may be passed bare (``key=jit.i64``); it normalizes to
    the 1-tuple ``(jit.i64,)`` and the body may then use a bare subscript
    ``self.m[k]`` as well as ``self.m[(k,)]``. Multi-key maps stay tuple-only.
    An empty key tuple declares a scalar accumulator: one whole-scan row."""
    if isinstance(key, _Type):
        key = (key,)
    if not isinstance(key, tuple):
        raise JitError("map key must be a tuple of jit.i64 / jit.str_")
    for k in key:
        if not isinstance(k, _Type):
            raise JitError("map key components must be jit.i64 or jit.str_")
        if k.dft == "DFTU_T_BYTES":
            raise JitError(
                "a bytes map key has no column form; group by jit.str_ or jit.i64 instead"
            )
    values, is_product, value_names = _value_spec(value)
    if is_product and builtins.any(isinstance(v, Quantiles) for v in values):
        raise JitError("jit.quantiles is top-level only; it cannot be a product/record component")
    return _MapDecl(key, values, is_product, value_names)


class Port(Protocol):
    """Authoring-only handle for a batch-scoped inter-plugin port.

    A :func:`publish` port is written per event with ``self.<port> += <expr>`` and
    the host publishes the per-batch total for a later plugin's consume port wired
    to it; a :func:`consume` port reads that value as a plain scalar
    (``self.<port>``, 0 when no producer published this batch). Never instantiated."""

    def __iadd__(self, x: float) -> "Port": ...


class Store(Generic[SK, SV]):
    """A persistent key-value field of a plugin, lowered to the plugin store ABI.

    Declare it by annotation only, ``seen: jit.Store[int, int]``; the field name
    is the namespace name and the host registers it as
    ``plugin.<plugin name>.<field name>``, so the same plugin keeps one
    namespace across runs. The plugin name is the stem of its ``.so``: the class
    identity for a plugin built from the cache, the file name for ``build(out=)``.
    Keys and values go through the :mod:`dftracer.utils.store_codec` encoding, so
    ``Store.namespace(name, 1, durable=True, shared=True)`` of the same index
    root reads and writes it from Python as a ``Namespace[K, V]``.

    ``K`` is an ``int``, a ``str`` or a ``Tuple`` of those; ``V`` is an ``int``
    or a ``float``. Authoring-only, never instantiated: the body of
    ``@jit.each_event`` is compiled, and each use below is an explicit key.

    ``get`` reads from a snapshot taken when the batch starts, so it sees what
    earlier folds committed and not what this fold's ``put`` calls wrote, which
    land when the batch ends. A ``put`` of a key that other workers or later runs
    wrote also replaces it (last writer wins), so keep per-scan totals in plain
    state and use a ``Store`` to carry a value to the next run. The store is
    lent only inside a fold, to the one index root of the scan. A
    :class:`~dftracer.utils.Series` or any other batch value cannot be a
    stored value."""

    def get(self, key: SK, default: SV) -> SV:
        """The value of ``key`` as of this batch's snapshot, or ``default``."""
        raise NotImplementedError

    def put(self, key: SK, value: SV) -> None:
        """Write ``value`` under ``key`` when the batch ends."""
        raise NotImplementedError


class _StoreField:
    """A :class:`Store` field: key components (``"i64"`` or ``"str"``), the value
    family (``"i64"`` or ``"f64"``) and its index among the plugin's stores."""

    __slots__ = ("name", "key_kinds", "value_kind", "index", "is_tuple")

    def __init__(
        self,
        name: str,
        key_kinds: Tuple[str, ...],
        value_kind: str,
        index: int,
        is_tuple: bool,
    ) -> None:
        self.name = name
        self.key_kinds = key_kinds
        self.value_kind = value_kind
        self.index = index
        self.is_tuple = is_tuple


class _PlainField:
    """A plain-container scan-state field: ``kind`` is ``dict``, ``set``, ``list``
    or ``scalar``; ``elem`` is the value (dict, scalar) or element (set, list)
    family; ``factory`` is whether a dict is a ``defaultdict``."""

    __slots__ = ("name", "kind", "key_types", "elem", "factory")

    def __init__(
        self,
        name: str,
        kind: str,
        key_types: "Tuple[_Type[object], ...]",
        elem: str,
        factory: bool,
    ) -> None:
        self.name = name
        self.kind = kind
        self.key_types = key_types
        self.elem = elem
        self.factory = factory


_STATE_SKIP = frozenset({"config", "plan_duql"})


def _state_elem(tp: object) -> "str | None":
    if tp is int:
        return "i64"
    if tp is float:
        return "f64"
    if tp is str or tp is Str:
        return "str"
    return None


def _state_key(tp: object) -> "Tuple[str, ...] | None":
    """The key components (``"i64"`` or ``"str"``) of a state key annotation: an
    ``int``, a ``str`` or a non-empty ``Tuple`` of those."""
    parts: Tuple[object, ...] = (tp,)
    if get_origin(tp) is tuple:
        parts = get_args(tp)
        if not parts or parts == ((),) or parts[-1] is Ellipsis:
            return None
    kinds: List[str] = []
    for part in parts:
        kind = _state_elem(part)
        if kind is None or kind == "f64":
            return None
        kinds.append(kind)
    return tuple(kinds)


def _resolve_hint(cls: type, attr: str, tp: object) -> object:
    """``tp`` with a string annotation evaluated in the class's module."""
    if not isinstance(tp, str):
        return tp
    module = sys.modules.get(getattr(cls, "__module__", ""))
    scope = dict(vars(module)) if module is not None else {}
    try:
        return eval(tp, scope, dict(vars(cls)))  # noqa: S307 - the author's own annotation
    except Exception as exc:
        raise _class_error(
            cls,
            f"cannot resolve the annotation '{tp}' of '{attr}' ({exc}); "
            "declare the types it names at module scope",
            attr,
        ) from None


def _state_default(cls: type, field: _PlainField, declared_default: bool) -> None:
    """Refuse a default that is not the empty state a worker starts from."""
    val = cls.__dict__.get(field.name, _MISSING)
    if field.kind == "dict":
        if declared_default and not isinstance(val, collections.defaultdict):
            raise _class_error(
                cls,
                f"'{field.name}' is a DefaultDict; its default must be defaultdict(int) "
                "or defaultdict(float)",
                field.name,
            )
        if isinstance(val, collections.defaultdict):
            want = int if field.elem == "i64" else float
            if val.default_factory is not want or val:
                raise _class_error(
                    cls,
                    f"'{field.name}' must start as defaultdict({want.__name__}) with no "
                    "items; a worker slice starts empty",
                    field.name,
                )
        elif val is not _MISSING and (type(val) is not dict or val):
            raise _class_error(
                cls, f"'{field.name}' must start as {{}} or as an empty defaultdict", field.name
            )
        return
    if val is _MISSING:
        return
    empty: object = {"set": builtins.set, "list": builtins.list}.get(field.kind)
    if empty is not None:
        if type(val) is not empty or val:
            raise _class_error(
                cls, f"'{field.name}' must start empty; a worker slice starts empty", field.name
            )
        return
    if isinstance(val, bool) or not isinstance(val, (int, float)) or val != 0:
        raise _class_error(
            cls,
            f"'{field.name}' must start at 0, because a non-zero start would be added once per "
            "worker; use ClassVar[int] for a constant, or config for a setting",
            field.name,
        )


_BATCH_TYPES = frozenset({"Series", "DataFrame", "LazyFrame", "Expr"})


def _is_batch_type(tp: object) -> bool:
    base = get_origin(tp) or tp
    return (
        isinstance(base, type)
        and base.__name__ in _BATCH_TYPES
        and base.__module__.startswith("dftracer.utils")
    )


def _escape_error(cls: type, attr: str, tp: object) -> JitError:
    return _class_error(
        cls,
        f"'{attr}: {tp!r}' keeps a Series, DataFrame or Expr on self, but a batch value is valid "
        "only inside one step call and cannot outlive it; fold it into scan state (a Dict, Set, "
        "List or number) instead",
        attr,
    )


def _plain_field(cls: type, attr: str, tp: object) -> _PlainField:
    if _is_batch_type(tp):
        raise _escape_error(cls, attr, tp)
    origin = get_origin(tp)
    args = get_args(tp)
    scalar = _state_elem(tp)
    if scalar in ("i64", "f64"):
        field = _PlainField(attr, "scalar", (), scalar, False)
        _state_default(cls, field, False)
        return field
    if origin in (dict, collections.defaultdict):
        keys = _state_key(args[0]) if len(args) == 2 else None
        elem = _state_elem(args[1]) if len(args) == 2 else None
        if keys is None or elem not in ("i64", "f64"):
            raise _class_error(
                cls,
                f"'{attr}' needs Dict[K, V] with K an int, a str or a Tuple of those and V an "
                f"int or a float, got {tp!r}",
                attr,
            )
        types = tuple(str_ if k == "str" else i64 for k in keys)
        field = _PlainField(attr, "dict", types, elem, False)
        _state_default(cls, field, origin is collections.defaultdict)
        field.factory = isinstance(cls.__dict__.get(attr), collections.defaultdict)
        return field
    if origin in (builtins.set, builtins.list):
        elem = _state_elem(args[0]) if len(args) == 1 else None
        if elem not in ("i64", "str"):
            raise _class_error(cls, f"'{attr}' needs an int or str element type, got {tp!r}", attr)
        kind = "set" if origin is builtins.set else "list"
        field = _PlainField(attr, kind, (), elem, False)
        _state_default(cls, field, False)
        return field
    raise _class_error(
        cls,
        f"'{attr}: {tp!r}' is not scan state; a state field is a Dict, DefaultDict, Set or List "
        "of int, float or str, a Tuple-keyed Dict, an int, a float, or a jit.Store[K, V]",
        attr,
    )


def _store_field(cls: type, attr: str, tp: object, index: int) -> _StoreField:
    if attr in cls.__dict__:
        raise _class_error(cls, f"'{attr}' is a jit.Store; declare it by annotation only", attr)
    if not attr or builtins.any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_" for c in attr):
        raise _class_error(
            cls,
            f"the jit.Store field '{attr}' names a store namespace, so it must be lowercase "
            "ASCII [a-z0-9_]",
            attr,
        )
    args = get_args(tp)
    if len(args) == 2 and _is_batch_type(args[1]):
        raise _escape_error(cls, attr, tp)
    keys = _state_key(args[0]) if len(args) == 2 else None
    value = _state_elem(args[1]) if len(args) == 2 else None
    if keys is None or value not in ("i64", "f64"):
        raise _class_error(
            cls,
            f"'{attr}' needs jit.Store[K, V] with K an int, a str or a Tuple of those and V "
            f"an int or a float, got {tp!r}",
            attr,
        )
    return _StoreField(attr, keys, value, index, get_origin(args[0]) is tuple)


def _own_annotations(cls: type) -> Dict[str, object]:
    get = getattr(inspect, "get_annotations", None)
    if get is not None:
        return dict(get(cls))
    return dict(cls.__dict__.get("__annotations__", {}))


def _state_fields(cls: type) -> "Tuple[Dict[str, _PlainField], Dict[str, _StoreField]]":
    """The plain-container scan-state and :class:`Store` fields ``cls`` declares
    by annotation."""
    plains: Dict[str, _PlainField] = {}
    stores: Dict[str, _StoreField] = {}
    own = _own_annotations(cls)
    hints = _type_hints(cls)
    for attr in own:
        if attr in _STATE_SKIP or isinstance(
            cls.__dict__.get(attr), (_MapDecl, _Port, _EachEvent, _PortDecl)
        ):
            continue
        tp = _resolve_hint(cls, attr, hints.get(attr, own[attr]))
        if get_origin(tp) is ClassVar or tp is ClassVar:
            continue
        if get_origin(tp) is Store:
            stores[attr] = _store_field(cls, attr, tp, len(stores))
        else:
            plains[attr] = _plain_field(cls, attr, tp)
    return plains, stores


_PORT_ID_CHARS = frozenset("abcdefghijklmnopqrstuvwxyz0123456789._/-")

# The registry namespace reserved for host ops (see _HOST_NAME_PREFIX below);
# not available as a JitPackage namespace or an explicit consume() wire id.
_RESERVED_NAMESPACE = "dftu"


class _Port:
    __slots__ = ("is_f64", "name", "role")

    def __init__(self, name: "str | None", is_f64: bool, role: str) -> None:
        self.name = name
        self.is_f64 = is_f64
        self.role = role


def _check_port_name(fn: str, name: object) -> str:
    if not isinstance(name, str) or not name:
        raise JitError(f"jit.{fn} needs a non-empty port name such as 'com.example.edges'")
    if name == _RESERVED_NAMESPACE or name.startswith(_RESERVED_NAMESPACE + "."):
        raise JitError(
            f"jit.{fn} port name '{name}' uses the reserved dftu. namespace; pick your own, "
            "e.g. 'com.example.edges'"
        )
    if builtins.any(c not in _PORT_ID_CHARS for c in name):
        raise JitError(f"jit.{fn} port name '{name}' must be lowercase ASCII [a-z0-9._/-]")
    return name


def _port_is_f64(fn: str, of: "_Type[object]") -> bool:
    if of is f64:
        return True
    if of is u64 or of is i64:
        return False
    raise JitError(f"jit.{fn}(of=...) must be jit.u64, jit.i64, or jit.f64")


def publish(of: "_Type[object]" = u64) -> Port:
    """Declare a batch-scoped publish port.

    Its wire id is derived from the attribute it is assigned to (package +
    module + class + attribute, see :class:`JitPackage`), resolved when the
    enclosing class is built by :func:`plugin`. In ``each_event`` accumulate a
    per-batch total with ``self.<port> += <expr>`` (a u64 sum by default,
    ``of=jit.f64`` for a double sum). The total is published once per batch and
    reset for the next batch; ``of`` picks the wire width (u64/i64 as an 8-byte
    int, f64 as a double)."""
    return cast(Port, _Port(None, _port_is_f64("publish", of), "publish"))


def consume(producer: "Port | str", of: "_Type[object]" = u64) -> Port:
    """Declare a batch-scoped consume port wired to ``producer``.

    ``producer`` is the other plugin's :func:`publish` attribute itself
    (``consume(Other.total)``), never a typed string - identity comes from the
    producer's object, and it must already be built (the producer class must be
    declared and decorated before this one). A string is accepted only for the
    C-interop case where the producer is a hand-written plugin with no Python
    object to reference. In ``each_event`` read the value a producer published
    for the current batch as a plain scalar ``self.<port>`` (0 when no producer
    published this batch). ``of`` must match the producer's width."""
    is_f64 = _port_is_f64("consume", of)
    if isinstance(producer, str):
        _check_port_name("consume", producer)
        return cast(Port, _Port(producer, is_f64, "consume"))
    port = cast(_Port, producer)
    if not isinstance(port, _Port) or port.role != "publish":
        raise JitError("jit.consume(...) needs a jit.publish attribute or an explicit wire id")
    if port.name is None:
        raise JitError(
            "jit.consume(...) references a jit.publish attribute whose plugin is not "
            "built yet; declare and decorate the producer class before this one"
        )
    if port.is_f64 != is_f64:
        raise JitError(f"jit.consume(of=...) does not match the producer's width for '{port.name}'")
    return cast(Port, _Port(port.name, is_f64, "consume"))


class _EachEvent:
    __slots__ = ("fn", "raw")

    def __init__(self, fn: Callable[..., object], raw: bool = False) -> None:
        self.fn = fn
        self.raw = raw


@overload
def each_event(fn: Callable[..., None]) -> _EachEvent: ...
@overload
def each_event(*, raw: Literal[False] = ...) -> Callable[[Callable[..., None]], _EachEvent]: ...
@overload
def each_event(*, raw: Literal[True]) -> Callable[[Callable[..., str]], _EachEvent]: ...
def each_event(
    fn: Callable[..., Any] | None = None, *, raw: bool = False
) -> _EachEvent | Callable[[Callable[..., Any]], _EachEvent]:
    """Mark the one per-event method to AST-compile.

    With ``raw=True`` the method instead returns a C++ string spliced verbatim
    into the per-row loop; available C names are ``host``, ``df``, ``e``, ``i``,
    and, for each declared accumulator ``m``, its row buffers ``_n_m``,
    ``_cap_m``, ``_k<i>_m``, ``_v<c>_m`` (plus ``_o<c>_m`` for a product and
    ``_b<c>_m`` for a by-reading reduction). ``e`` is a per-row shim populated
    from every fixed scan column (``e->pid`` etc), since a raw body cannot be
    re-lowered against the batch-resolved columns a compiled body uses. A raw
    body appends at most ``_RAW_ROWS_PER_EVENT`` rows per event per
    accumulator."""
    if fn is None:
        return lambda f: _EachEvent(f, raw)
    return _EachEvent(fn, raw)


class _EachBatch:
    __slots__ = ("fn",)

    def __init__(self, fn: Callable[..., object]) -> None:
        self.fn = fn


def each_batch(fn: Callable[..., None]) -> _EachBatch:
    """Mark the one per-batch method of a :func:`vfold` or a :func:`plugin`.

    It takes ``(self, df)`` where ``df`` is the current scan batch as columns.
    On a :func:`vfold` its body folds column reductions into the class's scalar
    accumulators, e.g. ``self.total += df["dur"].sum()``. On a :func:`plugin`
    the body is a sequence of column expressions (arithmetic, comparisons,
    masks, ``s[mask]``, ``where``, ``abs``, ``clip``, ``isnull``,
    ``fill_null``) whose reductions (``sum``, ``min``, ``max``, ``count``,
    ``mean``) fold into scalar state fields or key-less ``jit.map``
    descriptors; annotate ``df`` as :class:`Frame`."""
    return _EachBatch(fn)


class JitPlugin:
    __slots__ = ("name", "source", "renames", "result_names", "shippable", "lowering")

    def __init__(
        self,
        name: str,
        source: str,
        renames: Dict[str, List[str]],
        result_names: "Dict[str, str] | None" = None,
        shippable: bool = True,
        lowering: str = "branch",
    ) -> None:
        self.name = name
        self.source = source
        self.renames = renames
        # {wire id: attribute name}: a jit.map's provides/results wire id is
        # package-qualified to avoid cross-plugin collisions, but the value a
        # caller indexes results[...] with is always the attribute name, so
        # The extension rewrites native result keys through this map.
        self.result_names = result_names if result_names is not None else {}
        # False when built under an implicit __main__ identity (a script or
        # notebook with no JitPackage namespace); build() refuses those.
        self.shippable = shippable
        # How the per-event body was lowered: "branch" (a C branch per event),
        # "mask" (a guard evaluated over the batch into a selection, then the
        # updates on the selected rows only), "batch" (an @each_batch body of
        # column ops, no per-event loop), "raw" or "vfold".
        self.lowering = lowering


def _top_level_package(module: str) -> str:
    """The first dotted segment of an import path (its top-level package)."""
    return module.split(".", 1)[0]


def _module_rest(module: str) -> str:
    """``module`` with its top-level package segment stripped."""
    top = _top_level_package(module)
    rest = module[len(top) :]
    return rest[1:] if rest.startswith(".") else rest


def _check_namespace(fn: str, namespace: str) -> str:
    if not isinstance(namespace, str) or not namespace:
        raise JitError(f"{fn} needs a non-empty namespace such as 'acme'")
    if namespace == _RESERVED_NAMESPACE or namespace.startswith(_RESERVED_NAMESPACE + "."):
        raise JitError(f"{fn} namespace '{namespace}' uses the reserved dftu namespace")
    if builtins.any(c not in _PORT_ID_CHARS for c in namespace):
        raise JitError(f"{fn} namespace '{namespace}' must be lowercase ASCII [a-z0-9._/-]")
    return namespace


def _qualname_of(cls: type) -> str:
    qn = getattr(cls, "__qualname__", None) or getattr(cls, "__name__", None)
    if not isinstance(qn, str) or not qn:
        raise JitError("@jit.plugin needs a named class")
    # A class authored inside a function/notebook cell carries "<locals>" in
    # its qualname; drop it rather than emit an identity outside [a-z0-9._/-].
    return qn.replace(".<locals>.", ".")


def _module_of(cls: type) -> str:
    mod = getattr(cls, "__module__", None)
    if not isinstance(mod, str) or not mod:
        raise JitError("@jit.plugin needs a class with a __module__")
    return mod


class JitPackage:
    """The namespace and build settings a group of jit-authored plugins ship
    under.

    Identity for anything a jit plugin declares (a :func:`map` accumulator, a
    :func:`publish` port) is package + module + class + attribute, never a
    typed string: ``<namespace>/<module-without-its-top-package>.<qualname>.<attr>``,
    all lowercase. Bare :func:`plugin` / :func:`series` use a default
    ``JitPackage()``, whose namespace is the decorated class's top-level import
    package - so ``JitPackage("mypkg")`` on code already living in package
    ``mypkg`` produces the identical identity; pass an explicit namespace only
    to ship under a different name, or to name code authored in ``__main__``
    (a script or notebook has no package, and :meth:`build` refuses a
    shippable ``.so`` from one unless a namespace is supplied).

    Also carries the build settings for :meth:`build`: ``cache_dir`` (default
    the module's JIT cache), ``cflags`` (extra compiler flags), and ``out``
    (a fixed output path instead of the content-hashed cache).
    """

    __slots__ = ("namespace", "cache_dir", "cflags", "out")

    def __init__(
        self,
        namespace: "str | None" = None,
        *,
        cache_dir: "str | None" = None,
        cflags: "List[str] | None" = None,
        out: "str | None" = None,
    ) -> None:
        if namespace is not None:
            namespace = _check_namespace("JitPackage", namespace.lower())
        self.namespace = namespace
        self.cache_dir = cache_dir
        self.cflags = builtins.list(cflags) if cflags is not None else None
        self.out = out

    def _namespace_for(self, module: str) -> str:
        return self.namespace if self.namespace is not None else _top_level_package(module)

    def identity(self, cls: type, attr: "str | None" = None) -> str:
        """The qualified identity for ``attr`` on ``cls`` (or for ``cls``
        itself when ``attr`` is None), all lowercase."""
        module = _module_of(cls)
        namespace = self._namespace_for(module)
        parts = [p for p in (_module_rest(module), _qualname_of(cls), attr) if p]
        return (namespace + "/" + ".".join(parts)).lower()

    def shippable(self, cls: type) -> bool:
        """False for a class authored in ``__main__`` with no explicit
        namespace to fall back on - :meth:`build` refuses those."""
        return not (_module_of(cls) == "__main__" and self.namespace is None)

    @overload
    def plugin(self, cls: type[_C]) -> type[_C]: ...
    @overload
    def plugin(self, cls: None = None) -> Callable[[type[_C]], type[_C]]: ...
    def plugin(self, cls: "type | None" = None) -> "type | Callable[[type], type]":
        """Like :func:`plugin`, but names every map/port under this package."""
        if cls is None:
            return lambda c: _build_plugin(c, self)
        return _build_plugin(cls, self)

    @overload
    def series(self, fn: "Callable[..., Expr]") -> "Callable[..., Result]": ...
    @overload
    def series(
        self, fn: None = None, *, module: "str | None" = None
    ) -> "Callable[[Callable[..., Expr]], Callable[..., Result]]": ...
    def series(
        self, fn: "Callable[..., Expr] | None" = None, *, module: "str | None" = None
    ) -> "Callable[..., Result] | Callable[[Callable[..., Expr]], Callable[..., Result]]":
        """Like :func:`series`, but the default op name comes from this
        package instead of the raw ``__module__``."""
        if fn is None:
            return lambda f: _series_impl(f, module, self)
        return _series_impl(fn, module, self)

    def build(self, cls: type, *, out: "str | None" = None) -> str:
        """Compile ``cls`` (a :func:`plugin`-decorated class) to a shippable
        native ``.so`` and return its path.

        Raises :class:`JitError` for a class authored in ``__main__`` unless
        this package (or the one that decorated ``cls``) has an explicit
        namespace: a script or notebook has no import path to derive one
        from."""
        spec = getattr(cls, "_jit_plugin", None)
        if not isinstance(spec, JitPlugin):
            raise TypeError("expected a @jit.plugin-decorated class")
        if not spec.shippable:
            raise JitError(
                f"cannot build a shippable .so for '{cls.__name__}' from __main__; "
                'move it to a module, or pass JitPackage("yourname")'
            )
        return compile_plugin(
            spec, cache_dir=self.cache_dir, extra_cflags=self.cflags, out=out or self.out
        )


_DEFAULT_PACKAGE = JitPackage()


# Each expression reads the batch-resolved column for that field at row `i`
# (see _COLBUF_HELPERS); a column absent from the batch reads back its old
# per-field default (0 / DFTU_STR_NONE), matching dftu_jit_resolve/accessors.
_FIELD_EXPR: Dict[str, str] = {
    "pid": "dftu_jit_u64(&_col_pid, i)",
    "tid": "dftu_jit_u64(&_col_tid, i)",
    "ts": "dftu_jit_u64(&_col_ts, i)",
    "dur": "dftu_jit_u64(&_col_dur, i)",
    "cat": "dftu_jit_strid(host, &_col_cat, i)",
    "name": "dftu_jit_strid(host, &_col_name, i)",
    "fhash": "dftu_jit_strid(host, &_col_fhash, i)",
    "hhash": "dftu_jit_strid(host, &_col_hhash, i)",
    "phase": "dftu_jit_i64(&_col_ph, i)",
    # The row-fold frame has no per-row null tracking for dur; approximate
    # has_dur as phase == Complete, matching plugins::Event::has_dur().
    "has_dur": "(dftu_jit_i64(&_col_ph, i) == (int64_t)DFTU_PH_COMPLETE)",
}

# event field -> the batch column name it reads (the "_col_<name>" resolved in
# on_batch).
_FIELD_COLUMN: Dict[str, str] = {
    "pid": "pid",
    "tid": "tid",
    "ts": "ts",
    "dur": "dur",
    "cat": "cat",
    "name": "name",
    "fhash": "fhash",
    "hhash": "hhash",
    "phase": "ph",
    "has_dur": "ph",
}

# Every fixed scan column a raw each_event body might reference; raw text is
# not AST-analyzable, so a raw plugin resolves all of them unconditionally.
_ALL_FIXED_FIELDS = frozenset(_FIELD_COLUMN.values())

_ARG_METHODS: Dict[str, Tuple[str, str]] = {
    "arg_i64": ("dftu_jit_arg_i64", "0"),
    "arg_f64": ("dftu_jit_arg_f64", "0.0"),
    "arg_str": ("dftu_jit_arg_str", "DFTU_STR_NONE"),
}

_ARG_HELPER_DEFS: Dict[str, List[str]] = {
    "arg_i64": [
        "static int64_t dftu_jit_arg_i64(const dftu_jit_col* c, int64_t i) {",
        "    if (c->type != DFTU_TYPE_INT64 || !c->data || dftu_series_is_null(c->handle, i))",
        "        return 0;",
        "    return ((const int64_t*)c->data)[i];",
        "}",
        "",
    ],
    "arg_f64": [
        "static double dftu_jit_arg_f64(const dftu_jit_col* c, int64_t i) {",
        "    if (c->type != DFTU_TYPE_FLOAT64 || !c->data || dftu_series_is_null(c->handle, i))",
        "        return 0.0;",
        "    return ((const double*)c->data)[i];",
        "}",
        "",
    ],
    "arg_str": [
        "static dftu_str dftu_jit_arg_str(const dftu_plugin_host* host, const dftu_jit_col* c,",
        "                                 int64_t i) {",
        "    if (c->type != DFTU_TYPE_STRING) return DFTU_STR_NONE;",
        "    return dftu_jit_strid(host, c, i);",
        "}",
        "",
    ],
}

# A resolved batch column (see plugins::detail::ColBuf, the C++ SDK's mirror
# of this): NULL/-1 fields when the named column is absent from the batch, so
# every accessor below reads that as the field's old default rather than
# re-resolving it.
_COLBUF_HELPERS: List[str] = [
    "typedef struct {",
    "    dftu_series* handle;",
    "    int32_t type;",
    "    const void* data;",
    "    const int32_t* offsets;",
    "} dftu_jit_col;",
    "",
    "static dftu_jit_col dftu_jit_resolve(const dftu_dataframe* df, const char* name) {",
    "    dftu_jit_col c;",
    "    c.handle = dftu_dataframe_column(df, name);",
    "    c.type = c.handle ? dftu_series_type(c.handle) : -1;",
    "    c.data = c.handle ? dftu_series_data(c.handle) : NULL;",
    "    c.offsets = c.handle ? dftu_series_offsets(c.handle) : NULL;",
    "    return c;",
    "}",
    "",
    "static uint64_t dftu_jit_u64(const dftu_jit_col* c, int64_t i) {",
    "    if (!c->data || dftu_series_is_null(c->handle, i)) return 0;",
    "    return ((const uint64_t*)c->data)[i];",
    "}",
    "",
    "static int64_t dftu_jit_i64(const dftu_jit_col* c, int64_t i) {",
    "    if (!c->data || dftu_series_is_null(c->handle, i)) return 0;",
    "    return ((const int64_t*)c->data)[i];",
    "}",
    "",
    "static dftu_str dftu_jit_strid(const dftu_plugin_host* host, const dftu_jit_col* c, int64_t i) {",
    "    if (!c->data || !c->offsets) {",
    "        int64_t n = 0;",
    "        const char* p = c->handle ? dftu_series_string_at(c->handle, i, &n) : NULL;",
    "        return p ? host->intern(host->h, p, (uint32_t)n) : DFTU_STR_NONE;",
    "    }",
    "    if (dftu_series_is_null(c->handle, i)) return DFTU_STR_NONE;",
    "    const char* base = (const char*)c->data;",
    "    int32_t o0 = c->offsets[i], o1 = c->offsets[i + 1];",
    "    return host->intern(host->h, base + o0, (uint32_t)(o1 - o0));",
    "}",
    "",
]

# A raw each_event body is spliced verbatim and cannot be re-lowered against
# _col_*, so it gets a per-row shim struct populated from every fixed column
# up front, preserving the old e->field spelling.
_RAW_EVENT_SHIM_TYPE: List[str] = [
    "typedef struct {",
    "    uint64_t pid, tid, ts, dur;",
    "    int64_t phase;",
    "    int has_dur;",
    "    dftu_str cat, name, fhash, hhash;",
    "} dftu_jit_event;",
    "",
]

_RAW_EVENT_SHIM_POPULATE: List[str] = [
    "dftu_jit_event _ev;",
    "_ev.pid = dftu_jit_u64(&_col_pid, i);",
    "_ev.tid = dftu_jit_u64(&_col_tid, i);",
    "_ev.ts = dftu_jit_u64(&_col_ts, i);",
    "_ev.dur = dftu_jit_u64(&_col_dur, i);",
    "_ev.phase = dftu_jit_i64(&_col_ph, i);",
    "_ev.has_dur = (_ev.phase == (int64_t)DFTU_PH_COMPLETE);",
    "_ev.cat = dftu_jit_strid(host, &_col_cat, i);",
    "_ev.name = dftu_jit_strid(host, &_col_name, i);",
    "_ev.fhash = dftu_jit_strid(host, &_col_fhash, i);",
    "_ev.hhash = dftu_jit_strid(host, &_col_hhash, i);",
    "const dftu_jit_event* e = &_ev;",
]

_STR_FIELDS = frozenset({"cat", "name", "fhash", "hhash"})

# Reductions contributed with `+=` (everything else takes .observe / .append).
_ADDITIVE_OPS = frozenset({"DFTU_AGG_SUM"})

_ARGBY_OPS = frozenset({"DFTU_AGG_ARGMIN", "DFTU_AGG_ARGMAX"})

_TOPK_OPS = frozenset({"DFTU_AGG_TOPK", "DFTU_AGG_BOTTOMK"})

_SET_OPS = frozenset({"DFTU_AGG_SET_UNION"})

_LIST_OPS = frozenset({"DFTU_AGG_LIST_SORTED"})

# The C buffer element type and the batch column dtype for each value family.
_ELEM_CTYPE = {"i64": "int64_t", "f64": "double", "str": "dftu_str"}
_ELEM_DTYPE = {"i64": "DFTU_TYPE_INT64", "f64": "DFTU_TYPE_FLOAT64", "str": "DFTU_TYPE_STRING"}

# Key component type -> (C buffer element type, batch column dtype).
_KEY_CTYPE = {
    "DFTU_T_STR": ("dftu_str", "DFTU_TYPE_STRING"),
    "DFTU_T_F32": ("double", "DFTU_TYPE_FLOAT64"),
    "DFTU_T_F64": ("double", "DFTU_TYPE_FLOAT64"),
}

_CMP_OP: Dict[type, str] = {
    ast.Eq: "==",
    ast.NotEq: "!=",
    ast.Lt: "<",
    ast.LtE: "<=",
    ast.Gt: ">",
    ast.GtE: ">=",
}

_ARITH_OP: Dict[type, str] = {
    ast.Add: "+",
    ast.Sub: "-",
    ast.Mult: "*",
    ast.Div: "/",
}

# Numeric primitives usable in a key or value expression; each lowers to the
# matching static-inline helper in prims.h so a body reads jit.ilog2(e.dur)
# instead of a raw compiler builtin. Value is (c_function, arity, arg_cast); all
# return an integer.
_PRIM_C: Dict[str, Tuple[str, int, str]] = {
    # bit / log-scale bucketing (unsigned)
    "ilog2": ("dftu_ilog2_u64", 1, "uint64_t"),
    "clz": ("dftu_clz_u64", 1, "uint64_t"),
    "ctz": ("dftu_ctz_u64", 1, "uint64_t"),
    "popcount": ("dftu_popcount_u64", 1, "uint64_t"),
    "bit_width": ("dftu_bit_width_u64", 1, "uint64_t"),
    "ceil_pow2": ("dftu_ceil_pow2_u64", 1, "uint64_t"),
    "floor_pow2": ("dftu_floor_pow2_u64", 1, "uint64_t"),
    "rotl": ("dftu_rotl_u64", 2, "uint64_t"),
    "rotr": ("dftu_rotr_u64", 2, "uint64_t"),
    # integer math
    "abs": ("dftu_abs_i64", 1, "int64_t"),
    "clamp": ("dftu_clamp_i64", 3, "int64_t"),
    "div_ceil": ("dftu_div_ceil_i64", 2, "int64_t"),
    "div_round": ("dftu_div_round_i64", 2, "int64_t"),
    "align_up": ("dftu_align_up_u64", 2, "uint64_t"),
    "align_down": ("dftu_align_down_u64", 2, "uint64_t"),
    "isqrt": ("dftu_isqrt_u64", 1, "uint64_t"),
    "gcd": ("dftu_gcd_u64", 2, "uint64_t"),
    # fast integer hashing
    "mul_hi": ("dftu_mul_hi_u64", 2, "uint64_t"),
    "mix64": ("dftu_mix64", 1, "uint64_t"),
    "fastrange": ("dftu_fastrange_u64", 2, "uint64_t"),
    # floating point (double)
    "fmin": ("dftu_min_f64", 2, "double"),
    "fmax": ("dftu_max_f64", 2, "double"),
    "clampf": ("dftu_clamp_f64", 3, "double"),
    "sqrt": ("dftu_sqrt_f64", 1, "double"),
    "log2": ("dftu_log2_f64", 1, "double"),
    "log": ("dftu_log_f64", 1, "double"),
    "exp": ("dftu_exp_f64", 1, "double"),
    "fma": ("dftu_fma_f64", 3, "double"),
    "lerp": ("dftu_lerp_f64", 3, "double"),
    "copysign": ("dftu_copysign_f64", 2, "double"),
}

_CONSTRUCT = {
    ast.For: "for loop",
    ast.While: "while loop",
    ast.With: "with statement",
    ast.Call: "function call",
    ast.BinOp: "arithmetic expression",
    ast.BoolOp: "boolean expression",
    ast.Assign: "local assignment",
    ast.Return: "return statement",
    ast.IfExp: "conditional expression",
    ast.NamedExpr: "assignment expression (:=)",
}


def _describe(node: ast.AST) -> str:
    return _CONSTRUCT.get(type(node), type(node).__name__)


def _reject_at(reason: str, node: "ast.AST | None" = None) -> NoReturn:
    """Fail the compile at ``node``; the compiler turns it into file:line:col."""
    err = JitError(reason, locate=False)
    err.node = node
    raise err


def _reject(what: str, node: "ast.AST | None" = None) -> NoReturn:
    _reject_at(f"unsupported in @jit.each_event: {what}; use a raw C++ plugin", node)


_I64_MIN = -(1 << 63)
_I64_MAX = (1 << 63) - 1
_U64_MAX = (1 << 64) - 1


def _int_literal(v: int) -> str:
    """C text for an integer literal, rejecting one that would not fit in the
    64-bit key/value slot (Python ints are unbounded; the compiled body is not,
    so an out-of-range literal would silently wrap)."""
    if v < _I64_MIN or v > _U64_MAX:
        raise JitError(
            f"integer literal {v} does not fit in 64 bits; jit keys and values "
            "are 64-bit and it would silently wrap"
        )
    return f"{v}ULL" if v > _I64_MAX else str(v)


def _key_ctype(ktype: str) -> Tuple[str, str]:
    """(C buffer element type, batch column dtype) for a key component type."""
    return _KEY_CTYPE.get(ktype, ("int64_t", "DFTU_TYPE_INT64"))


def _out_name(decl: _AnyMap, comp: int) -> str:
    """Result column name for value component ``comp``."""
    if not decl.is_product:
        return "value"
    if decl.value_names is not None:
        return decl.value_names[comp]
    return f"v{comp}"


class _Update(NamedTuple):
    """One merge-inferable update to a plain-container field: its ``form``
    (``sum``, ``max``, ``min``, ``add`` or ``append``), the key and value
    expressions, and whether a missing key contributes the type's zero first."""

    attr: str
    form: str
    keys: List[ast.expr]
    value: ast.expr
    identity: bool


_STATE_FORMS = {
    "dict": (
        "d[k] += x on a defaultdict, d[k] = d.get(k, 0) + x, d[k] = max(d[k], x) on a "
        "defaultdict, d[k] = max(d.get(k, x), x), or the same with min"
    ),
    "set": "s.add(x)",
    "list": "lst.append(x)",
    "scalar": "n += x",
}
_STATE_PY = {"i64": "int", "f64": "float"}


def _child_blocks(stmt: ast.stmt) -> "List[List[ast.stmt]] | None":
    """The statement lists a compound statement runs, None for a simple one."""
    if isinstance(stmt, ast.If):
        return [stmt.body, stmt.orelse]
    if isinstance(stmt, ast.Try):
        return [stmt.body] + [h.body for h in stmt.handlers]
    if isinstance(stmt, (ast.With, ast.For)):
        return [stmt.body]
    return None


class _Local:
    """A local of a body: ``kind`` is ``i64``, ``f64``, ``str`` (an interned id),
    ``view`` (bytes that the host lends: a file line, a chunk or a JSON string) or
    ``val`` (a JSON / YAML value); ``cname`` the C lvalue. ``origins`` names what a
    ``view`` or ``val`` points into, and ``depth`` is the loop depth that made it."""

    __slots__ = ("kind", "cname", "origins", "depth")

    def __init__(self, kind: str, cname: str, depth: int = 0) -> None:
        self.kind = kind
        self.cname = cname
        self.origins: List[Tuple[Any, ...]] = []
        self.depth = depth


def _unconditional_nodes(root: ast.AST) -> Iterator[ast.AST]:
    """The nodes of ``root`` that always run. An ``and`` / ``or`` operand after
    the first, and a link of a chained comparison after the first, only run when
    the earlier ones allow it, so hoisting one above its guard would run it for
    a row the guard excludes."""
    stack: List[ast.AST] = [root]
    while stack:
        node = stack.pop()
        yield node
        if isinstance(node, ast.BoolOp):
            stack.append(node.values[0])
        elif isinstance(node, ast.Compare) and len(node.ops) > 1:
            stack.extend((node.left, node.comparators[0]))
        else:
            stack.extend(ast.iter_child_nodes(node))


_KIND_WORD = {
    "i64": "an int",
    "f64": "a float",
    "str": "a string",
    "view": "a string view",
    "val": "a JSON value",
}
_CTYPE = {
    "i64": "int64_t",
    "f64": "double",
    "str": "dftu_str",
    "view": "DftuView",
    "val": "const dftu_value*",
}
_CINIT = {
    "i64": "0",
    "f64": "0.0",
    "str": "DFTU_STR_NONE",
    "view": "{NULL, 0, DFTU_STR_NONE}",
    "val": "NULL",
}
_VAL_METHODS = frozenset({"find", "as_int", "as_float", "as_str", "as_bool", "is_null"})
_RAISABLE = {"Fatal": "DFTU_E_UNKNOWN", "SkipRest": "DFTU_OK", "Cancelled": "DFTU_E_CANCELLED"}
_UNCATCHABLE = {
    "Fatal": "it fails the run; raise it from the handler of a jit.Error instead",
    "SkipRest": "it is not an error",
    "Cancelled": "a cancelled body must stop",
}
_CATCH_COND = {
    "Error": "dftu_jit_is_error(_st)",
    "NotFound": "_st == DFTU_E_NOT_FOUND",
    "Closed": "_st == DFTU_E_NOT_FOUND && _pclosed",
    "IoError": "_st == DFTU_E_IO",
    "ParseError": "_st == DFTU_E_PARSE",
}
# What `except jit.Error` catches: a failed call. A jit.Fatal, a cancel, an
# internal error and an allocation failure always end the run.
_ERROR_HELPER = [
    "static int dftu_jit_is_error(dftu_status s) {",
    "    return s < 0 && s != DFTU_E_UNKNOWN && s != DFTU_E_INTERNAL && s != DFTU_E_CANCELLED &&",
    "           s != DFTU_E_NO_MEMORY;",
    "}",
    "",
]


class _Compiler:
    def __init__(
        self,
        maps: Dict[str, _AnyMap],
        ops: Dict[str, Op] | None = None,
        ports: Dict[str, _Port] | None = None,
        config_fields: "Dict[str, _CfgField] | None" = None,
        plain: "Dict[str, _PlainField] | None" = None,
        stores: "Dict[str, _StoreField] | None" = None,
        chans: "Dict[str, _PortDecl] | None" = None,
    ) -> None:
        self.maps = maps
        self.plain = plain if plain is not None else {}
        self.stores = stores if stores is not None else {}
        # Channels, latest values and phase results by attribute, and the ones
        # this body uses (their handles are opened before the body runs).
        self.chans = chans if chans is not None else {}
        self.chan_uses: builtins.set[str] = builtins.set()
        self._port_frame = False
        # Per plain field: the (form, identity) its updates agree on.
        self.plain_forms: Dict[str, Tuple[str, bool]] = {}
        self.store_gets: builtins.set[str] = builtins.set()
        self.store_puts: builtins.set[str] = builtins.set()
        self.ports = ports if ports is not None else {}
        # The plugin's config dataclass fields; each_event reads them as
        # self.config.<name>, lowering to the file-scope _cfg_<name> static set
        # at load.
        self.config_fields = config_fields if config_fields is not None else {}
        # Referenced @jit.op transforms, inlined into the plugin as static C
        # functions the moment the body calls one.
        self.ops = ops if ops is not None else {}
        self.op_defs: List[str] = []
        self.op_emitted: builtins.set[str] = builtins.set()
        # Physical batch columns ("pid", "cat", "ph", ...) the body actually
        # reads; on_batch resolves exactly these once per batch.
        self.fields_used: builtins.set[str] = builtins.set()
        self.str_literals: List[str] = []
        self.arg_keys: List[str] = []
        self.arg_helpers: builtins.set[str] = builtins.set()
        self.self_name = "self"
        self.event_name = "e"
        # Contribution statements per accumulator; the emitted row buffers hold
        # this many rows per scanned event.
        self.rows_per_event: Dict[str, int] = {}
        # CSE: ast.dump(node) -> (local name, is_float) for a hoisted expression
        # shared within the current scope.
        self._cse: Dict[str, Tuple[str, bool]] = {}
        self._cse_n = 0
        # The statement being compiled, where an error without a node points.
        self._cur: ast.AST | None = None
        # The C condition of a body that is one guard around its updates: the
        # emitted code filters the batch with it once, then runs the updates
        # on the surviving rows only. None when the body branches per event.
        self.mask_guard: str | None = None
        # raise / try: whether the body unwinds, the stack of labels a failing
        # statement jumps to (innermost last), and the label past the loop.
        self.can_raise = False
        self._targets: List[str] = []
        self._label_n = 0
        self._source: _Source | None = None
        self._store_read = False
        # Locals, files and JSON values (see _flow): a local is a C local of the
        # per-event loop, or a field of the task frame in a flow body.
        self.flow = False
        self.is_async = False
        self.in_task = False
        self.locals: Dict[str, _Local] = {}
        self.local_decls: List[str] = []
        self.res_decls: List[str] = []
        self.res_frees: List[str] = []
        self.files: Dict[str, Tuple[str, str]] = {}
        self.uses: builtins.set[str] = builtins.set()
        self.tasks: Dict[str, _TaskSig] = {}
        self.emit_names: List[str] = []
        self.ob_size = 8
        self._res_n = 0
        self._await_n = 0
        self._job_n = 0
        self._cse_ok = True
        self._loops = 0
        self._has_loops = False
        self.doc_gen: Dict[str, int] = {}
        self.file_gen: Dict[str, int] = {}
        self._active_loops: List[int] = []
        self._loop_seq = 0
        self._iter_files: List[str] = []
        self.docs: Dict[str, str] = {}
        self.file_path: Dict[str, str] = {}
        self._rhs_node: "ast.expr | None" = None

    def lower(self, fn: Callable[..., object]) -> List[str]:
        source = _Source(fn)
        self._source = source
        try:
            return self._lower(fn, source)
        except JitError as exc:
            raise source.relocate(exc, self._cur) from None

    def _lower(self, fn: Callable[..., object], source: _Source) -> List[str]:
        mod = ast.parse(source.text)
        funcs = [n for n in mod.body if isinstance(n, ast.FunctionDef)]
        if len(funcs) != 1:
            _reject("each_event must decorate a single method", mod.body[0])
        fn_ast = funcs[0]
        self._cur = fn_ast
        params = fn_ast.args.args
        if len(params) != 2:
            _reject("each_event must take exactly (self, e)", fn_ast)
        self.self_name = params[0].arg
        self.event_name = params[1].arg
        if self.plain:
            self._infer_plain(fn_ast.body)
            self._declare_plain()
        self._has_loops = builtins.any(isinstance(n, ast.For) for n in ast.walk(fn_ast))
        self.can_raise = builtins.any(
            isinstance(n, (ast.Raise, ast.Try, ast.With))
            or (isinstance(n, ast.Call) and self._jit_name(n.func) in ("json", "yaml"))
            or (isinstance(n, ast.Call) and self._chan_call(n) is not None)
            for n in ast.walk(fn_ast)
        )
        body = fn_ast.body
        if (
            len(body) == 1
            and isinstance(body[0], ast.If)
            and not body[0].orelse
            and not self.stores
        ):
            self._cur = body[0]
            self.mask_guard = self._cond(body[0].test)
            body = body[0].body
        defs, lines = self._compile_scope(body)
        return self.local_decls + defs + lines

    # ---- flow bodies: @jit.finalize and @jit.task ----------------------------

    def lower_flow(self, fn: Callable[..., object], sig: "_TaskSig | None") -> "_FlowFn":
        source = _Source(fn)
        self._source = source
        try:
            return self._lower_flow(fn, source, sig)
        except JitError as exc:
            raise source.relocate(exc, self._cur) from None

    def _lower_flow(
        self, fn: Callable[..., object], source: _Source, sig: "_TaskSig | None"
    ) -> "_FlowFn":
        mod = ast.parse(source.text)
        funcs = [n for n in mod.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
        if len(funcs) != 1:
            _reject_at("a flow body decorates a single method", mod.body[0])
        fn_ast = funcs[0]
        self._cur = fn_ast
        self.flow = True
        self.is_async = isinstance(fn_ast, ast.AsyncFunctionDef)
        self.in_task = sig is not None
        self.task_ret = sig.ret if sig is not None else None
        self._cse_ok = False
        self._exit_label = "_fx_exit"
        self.event_name = ""
        self.can_raise = True
        params = fn_ast.args.args
        want = 1 + (len(sig.params) if sig is not None else 0)
        if len(params) != want or fn_ast.args.vararg or fn_ast.args.kwonlyargs or fn_ast.args.kwarg:
            _reject_at(
                "a @jit.task takes (self, <typed parameters>)"
                if sig is not None
                else "a @jit.finalize takes (self)",
                fn_ast,
            )
        self.self_name = params[0].arg
        if sig is not None:
            for arg, (pname, pkind) in zip(params[1:], sig.params):
                self.local_decls.append(f"{_CTYPE[pkind]} v_{arg.arg};")
                self.locals[arg.arg] = _Local(pkind, f"S->v_{arg.arg}")
        defs, lines = self._compile_scope(fn_ast.body)
        name = sig.name if sig is not None else fn_ast.name
        return _FlowFn(
            name=name,
            is_async=self.is_async,
            sig=sig,
            fields=self.local_decls + self.res_decls,
            body=defs + lines,
            ob_size=self.ob_size,
            uses=builtins.set(self.uses),
            res_frees=self.res_frees,
            emit_names=self.emit_names,
            store_gets=self.store_gets,
            op_defs=self.op_defs,
        )

    # ---- locals, files, JSON values, await (the flow constructs) -------------

    def _local(self, node: ast.Name) -> _Local:
        loc = self.locals.get(node.id)
        if loc is not None:
            self._check_origins(node.id, loc, node)
            return loc
        if node.id in self.files:
            _reject_at(
                f"{node.id} is a file: use `for line in {node.id}` or {node.id}.read(n)", node
            )
        _reject_at(
            f"unknown name {node.id}; a local exists once it is assigned, and the body reads "
            "only its locals, the event, self and jit",
            node,
        )

    def _check_origins(self, name: str, loc: _Local, node: ast.AST) -> None:
        for origin in loc.origins:
            if origin[0] == "doc":
                if self.doc_gen.get(origin[1]) != origin[2]:
                    _reject_at(
                        f"{name} points into {origin[1]}, which is parsed again before this use; "
                        f"read what you need from {name} before the new parse",
                        node,
                    )
            elif origin[0] == "loop":
                if origin[1] not in self._active_loops:
                    _reject_at(f"{name} is valid only inside its for loop", node)
            elif origin[0] == "file":
                if origin[1] not in self.files or self.file_gen.get(origin[1]) != origin[2]:
                    _reject_at(
                        f"{name} is a chunk of {origin[1]} that is read again or closed "
                        "before this use",
                        node,
                    )

    def _origins_of(self, node: ast.expr) -> List[Tuple[Any, ...]]:
        node = _unwrap_index(node)
        if isinstance(node, ast.Name):
            loc = self.locals.get(node.id)
            return builtins.list(loc.origins) if loc is not None else []
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
            return self._origins_of(node.func.value)
        return []

    def _bind_origins(
        self, name: str, loc: _Local, fresh: bool, origins: List[Tuple[Any, ...]], node: ast.AST
    ) -> None:
        if loc.kind == "view" and not fresh:
            old = {o[:2] for o in loc.origins}
            new = {o[:2] for o in origins}
            if old != new and builtins.any(o[0] != "doc" for o in builtins.list(old | new)):
                _reject_at(
                    f"a view cannot be copied into {name}: {name} lives longer than the "
                    "line, chunk or string that the view points into",
                    node,
                )
        loc.origins = origins

    def _new_local(self, name: str, kind: str, node: ast.AST) -> _Local:
        loc = self.locals.get(name)
        if loc is not None:
            if loc.kind != kind and {loc.kind, kind} == {"str", "view"}:
                _reject_at(
                    f"a view cannot be copied into {name}: {name} lives longer than the "
                    "line, chunk or string that the view points into",
                    node,
                )
            if loc.kind != kind:
                _reject_at(
                    f"{name} is {_KIND_WORD[loc.kind]} here and {_KIND_WORD[kind]} elsewhere; "
                    "a local keeps one type",
                    node,
                )
            return loc
        if name in (self.self_name, self.event_name, "jit") or name in self.files:
            _reject_at(f"{name} cannot be assigned: it is already a name of this body", node)
        if self.flow:
            self.local_decls.append(f"{_CTYPE[kind]} v_{name};")
            loc = _Local(kind, f"S->v_{name}", self._loops)
        else:
            self.local_decls.append(f"{_CTYPE[kind]} _v_{name} = {_CINIT[kind]};")
            loc = _Local(kind, f"_v_{name}", self._loops)
        self.locals[name] = loc
        return loc

    def _new_res(self, ctype: str, base: str) -> str:
        self._res_n += 1
        if self.flow:
            self.res_decls.append(f"{ctype} {base}{self._res_n};")
            return f"S->{base}{self._res_n}"
        self.res_decls.append(f"{ctype} _{base}{self._res_n} = NULL;")
        return f"_{base}{self._res_n}"

    def _loc_text(self, node: ast.AST) -> str:
        file, line, col = self._loc(node)
        return _c_str_literal(f"{file}:{line}:{col}")

    def _fail(self, status: str, what: str, arg: str, node: ast.AST) -> str:
        self.uses.add("cap")
        return (
            f"_st = dftu_jit_fail(ctx, &_cap, {status}, {_c_str_literal(what)}, {arg}, "
            f"{self._loc_text(node)}); _unw = 1; goto {self._target()};"
        )

    # A JSON / YAML value: a local of kind ``val`` or a find() chain of one. Its
    # C form is a `const dftu_value*` that is NULL for a missing key.
    def _val_expr(self, node: ast.expr) -> "str | None":
        node = _unwrap_index(node)
        if isinstance(node, ast.Name):
            loc = self.locals.get(node.id)
            if loc is None or loc.kind != "val":
                return None
            self._check_origins(node.id, loc, node)
            return loc.cname
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "find"
        ):
            base = self._val_expr(node.func.value)
            if base is None:
                return None
            if len(node.args) != 1 or node.keywords:
                _reject_at("find takes one key", node)
            key = node.args[0]
            if isinstance(key, ast.Constant) and isinstance(key.value, str):
                text = _c_str_literal(key.value)
            else:
                cfg = self._cfg_field(key)
                if cfg is None or cfg.kind != "str":
                    _reject_at("the key of find is a string literal or a str config field", key)
                text = f"_c->v_{cfg.name}"
            return f"dftu_obj_get({base}, {text})"
        return None

    def _val_call(self, node: ast.expr) -> "Tuple[str, str] | None":
        node = _unwrap_index(node)
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)):
            return None
        meth = node.func.attr
        if meth not in _VAL_METHODS:
            return None
        base = self._val_expr(node.func.value)
        if base is None:
            return None
        if meth == "find":
            found = self._val_expr(node)
            assert found is not None
            return "val", found
        args = node.args
        if node.keywords or len(args) > (0 if meth == "is_null" else 1):
            _reject_at(
                f"{meth} takes {'no' if meth == 'is_null' else 'at most one'} argument", node
            )
        if meth == "is_null":
            return "i64", f"dftu_jit_is_null({base})"
        if meth == "as_str":
            if args:
                if not (self._is_str_element(args[0]) or self._is_str_literal(args[0])):
                    _reject_at("the default of as_str is a string", args[0])
                dflt = (
                    self._str_literal(args[0].value)
                    if isinstance(args[0], ast.Constant) and isinstance(args[0].value, str)
                    else self._value(args[0])
                )
            else:
                dflt = self._str_literal("")
            return "str", f"dftu_jit_as_str(host, {base}, {dflt})"
        if meth == "as_bool":
            if args and isinstance(args[0], ast.Constant) and isinstance(args[0].value, bool):
                dflt = "1" if args[0].value else "0"
            elif args:
                dflt = f"({self._arith(args[0])[0]}) != 0"
            else:
                dflt = "0"
            return "i64", f"dftu_as_bool({base}, {dflt})"
        if meth == "as_int":
            dflt = "0"
            if args:
                expr, is_float = self._arith(args[0])
                if is_float:
                    _reject_at("the default of as_int is an int", args[0])
                dflt = f"(int64_t)({expr})"
            return "i64", f"dftu_as_i64({base}, {dflt})"
        dflt = f"(double)({self._arith(args[0])[0]})" if args else "0.0"
        return "f64", f"dftu_as_f64({base}, {dflt})"

    def _len_call(self, node: ast.expr) -> "str | None":
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "len"
            and len(node.args) == 1
            and not node.keywords
        ):
            return None
        arg = node.args[0]
        val = self._val_expr(arg)
        if val is not None:
            return f"dftu_jit_count({val})"
        if self._is_view_node(arg):
            return f"(int64_t)({self._view_expr(arg)}).n"
        if (
            isinstance(arg, ast.Name)
            and arg.id in self.locals
            and self.locals[arg.id].kind == "str"
        ):
            return f"dftu_jit_slen(host, {self.locals[arg.id].cname})"
        return None

    def _is_view_node(self, node: ast.expr) -> bool:
        node = _unwrap_index(node)
        if isinstance(node, ast.Name):
            loc = self.locals.get(node.id)
            return loc is not None and loc.kind == "view"
        return (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "as_str"
            and self._val_expr(node.func.value) is not None
        )

    def _view_expr(self, node: ast.expr) -> str:
        """The C ``DftuView`` of a string expression, read from its bytes. A view
        is not interned: ``_value`` interns one only where an id is needed."""
        node = _unwrap_index(node)
        if isinstance(node, ast.Name) and node.id in self.locals:
            loc = self._local(node)
            if loc.kind == "view":
                return loc.cname
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "as_str"
        ):
            base = self._val_expr(node.func.value)
            if base is not None:
                if len(node.args) > 1 or node.keywords:
                    _reject_at("as_str takes at most one argument", node)
                if node.args:
                    arg = node.args[0]
                    if not (self._is_str_element(arg) or self._is_str_literal(arg)):
                        _reject_at("the default of as_str is a string", arg)
                    dflt = self._view_expr(arg)
                else:
                    dflt = 'dftu_jit_view("", 0)'
                return f"dftu_jit_as_view({base}, {dflt})"
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return f"dftu_jit_view({_c_str_literal(node.value)}, {len(node.value.encode('utf-8'))})"
        cfg = self._cfg_field(node)
        if cfg is not None and cfg.kind == "str":
            return f"dftu_jit_view_of(host, _cfgs_{cfg.name})"
        operand = self._str_operand(node)
        if operand is None:
            _reject_at("a string compared with something that is not a string field", node)
        return f"dftu_jit_view_of(host, {operand})"

    def _expr_kind(self, node: ast.expr) -> str:
        node = _unwrap_index(node)
        if isinstance(node, ast.Name):
            kind = self._local(node).kind
            return "str" if kind == "view" else kind
        vc = self._val_call(node)
        if vc is not None:
            return vc[0]
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return "str"
        if self._is_str_element(node) or self._is_str_literal(node) or self._is_none_sentinel(node):
            return "str"
        _, is_float = self._arith(node)
        return "f64" if is_float else "i64"

    def _rhs_of_kind(self, node: ast.expr, kind: str) -> str:
        node = _unwrap_index(node)
        if kind == "val":
            val = self._val_expr(node)
            assert val is not None
            return val
        if kind == "view":
            return self._view_expr(node)
        if kind == "str":
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                return self._str_literal(node.value)
            return self._value(node)
        expr, _ = self._arith(node)
        return expr

    def _is_parse_call(self, node: ast.expr) -> "str | None":
        if isinstance(node, ast.Call):
            name = self._jit_name(node.func)
            if name in ("json", "yaml"):
                return name
        return None

    def _flow_stmt(self, stmt: ast.stmt) -> "List[str] | None":
        if isinstance(stmt, ast.AsyncFor):
            return self._async_for(stmt)
        if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call):
            port = self._chan_call(stmt.value)
            if port is not None:
                return self._port_stmt(port)
        if isinstance(stmt, ast.Assign):
            if len(stmt.targets) == 1 and isinstance(stmt.targets[0], (ast.Name, ast.Tuple)):
                return self._assign(stmt.targets[0], stmt.value, stmt)
            return None
        if (
            isinstance(stmt, ast.AnnAssign)
            and isinstance(stmt.target, ast.Name)
            and stmt.value is not None
        ):
            return self._assign(stmt.target, stmt.value, stmt)
        if isinstance(stmt, ast.AugAssign) and isinstance(stmt.target, ast.Name):
            return self._aug_local(stmt)
        if isinstance(stmt, ast.With):
            return self._with(stmt)
        if isinstance(stmt, ast.For):
            return self._for(stmt)
        if isinstance(stmt, ast.Return):
            return self._return(stmt)
        if isinstance(stmt, ast.Pass):
            return []
        if isinstance(stmt, ast.Expr):
            value = stmt.value
            if isinstance(value, ast.Await):
                return self._await_stmt(None, value, stmt)
            if isinstance(value, ast.Constant):
                return []
            if isinstance(value, ast.Call):
                name = self._jit_name(value.func)
                if name == "emit":
                    return self._emit_call(value)
                if self._task_call(value) is not None:
                    _reject_at(
                        "a task call must be awaited (await self.<task>(...)) or passed to "
                        "jit.all or jit.any"
                        if self.flow
                        else "a task is started from the @jit.finalize body: on_batch returns "
                        "before the task could finish and cannot wait for it",
                        value,
                    )
        return None

    def _assign(self, target: ast.expr, value: ast.expr, stmt: ast.stmt) -> List[str]:
        if isinstance(value, ast.Await):
            return self._await_stmt(target, value, stmt)
        port = self._chan_call(value)
        if port is not None:
            return self._port_assign(target, port, stmt)
        if isinstance(target, ast.Tuple):
            _reject_at("tuple assignment needs `await jit.all(...)` on the right", target)
        assert isinstance(target, ast.Name)
        name = target.id
        parse = self._is_parse_call(value)
        if parse is not None:
            assert isinstance(value, ast.Call)
            return self._parse(name, parse, value, target)
        if isinstance(value, ast.Call) and self._jit_name(value.func) in (
            "open",
            "sleep",
            "all",
            "any",
        ):
            _reject_at(
                f"jit.{self._jit_name(value.func)} cannot be assigned"
                + (
                    " (use `with jit.open(path) as f`)"
                    if self._jit_name(value.func) == "open"
                    else " (await it)"
                ),
                value,
            )
        if (
            isinstance(value, ast.Call)
            and isinstance(value.func, ast.Attribute)
            and value.func.attr == "read"
            and isinstance(value.func.value, ast.Name)
            and value.func.value.id in self.files
        ):
            return self._read_stmt(name, value, target)
        self._rhs_node = _unwrap_index(value)
        kind = self._expr_kind(value)
        if kind == "str" and self._is_view_node(value):
            kind = "view"
        fresh = name not in self.locals
        loc = self._new_local(name, kind, target)
        rhs = self._rhs_of_kind(value, kind)
        if kind in ("view", "val"):
            self._bind_origins(name, loc, fresh, self._origins_of(value), target)
        return [f"{loc.cname} = {rhs};"]

    def _aug_local(self, stmt: ast.AugAssign) -> List[str]:
        assert isinstance(stmt.target, ast.Name)
        loc = self._local(stmt.target)
        sym = _ARITH_OP.get(type(stmt.op))
        if sym is None:
            _reject_at("augmented operator (only += -= *= /= are allowed)", stmt)
        if loc.kind not in ("i64", "f64"):
            _reject_at(
                f"{stmt.target.id} is {_KIND_WORD[loc.kind]} and cannot be accumulated", stmt
            )
        expr, is_float = self._arith(stmt.value)
        if isinstance(stmt.op, ast.Div):
            if loc.kind != "f64":
                _reject_at("/= makes a float; the local is an int", stmt)
            return [f"{loc.cname} /= (double)({expr});"]
        if is_float and loc.kind == "i64":
            _reject_at("a float expression cannot be added into an int local", stmt)
        return [f"{loc.cname} {sym}= {expr};"]

    def _parse(self, name: str, fmt: str, call: ast.Call, target: ast.expr) -> List[str]:
        if len(call.args) != 1 or call.keywords:
            _reject_at(f"jit.{fmt} takes one text", call)
        if self._expr_kind(call.args[0]) != "str":
            _reject_at(f"the text of jit.{fmt} is a string", call.args[0])
        is_view = self._is_view_node(call.args[0])
        text = self._view_expr(call.args[0]) if is_view else self._rhs_of_kind(call.args[0], "str")
        loc = self._new_local(name, "val", target)
        if self._loops:
            for other_name, other in self.locals.items():
                if (
                    other_name != name
                    and other.depth < self._loops
                    and builtins.any(o[0] == "doc" and o[1] == name for o in other.origins)
                ):
                    _reject_at(
                        f"{name} is parsed again inside a loop, but {other_name} was taken from "
                        f"it before the loop and would point into freed memory",
                        target,
                    )
        gen = self.doc_gen.get(name, -1) + 1
        self.doc_gen[name] = gen
        loc.origins = [("doc", name, gen)]
        doc = self.docs.get(name)
        if doc is None:
            doc = self._new_res("dftu_doc*", "doc")
            self.docs[name] = doc
            self.res_frees.append(
                f"if ({doc}) {{ {'_fmtj' if fmt == 'json' else '_fmty'}->doc_free({doc}); {doc} = NULL; }}"
            )
        svc = "_fmtj" if fmt == "json" else "_fmty"
        self.uses.add(fmt)
        self.uses.add("cap")
        fail = self._fail("_p", fmt, '""', call)
        if is_view:
            get_text = [
                f"    DftuView _tv = {text};",
                "    const char* _tp = _tv.p;",
                "    uint32_t _tl = _tv.n;",
            ]
        else:
            get_text = [
                "    uint32_t _tl = 0;",
                f"    const char* _tp = {text} == DFTU_STR_NONE ? NULL : host->resolve(host->h, {text}, &_tl);",
            ]
        return [
            "{",
            *get_text,
            '    if (!_tp) { _tp = ""; _tl = 0; }',
            "    _cap.has = 0;",
            "    dftu_doc* _nd = NULL;",
            f"    dftu_status _p = {svc}->parse(&_cap.c, host->h, _tp, _tl, &_nd);",
            f"    if (_p < 0) {{ {fail} }}",
            f"    if ({doc}) {svc}->doc_free({doc});",
            f"    {doc} = _nd;",
            f"    {loc.cname} = {svc}->root({doc});",
            "}",
        ]

    def _no_await(self, body: List[ast.stmt], what: str) -> None:
        for stmt in body:
            for sub in ast.walk(stmt):
                if isinstance(sub, (ast.Await, ast.AsyncFor)):
                    _reject_at(f"await inside {what}; await before or after it", sub)

    def _path_text(self, node: ast.expr) -> Tuple[str, str]:
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            if "\0" in node.value:
                _reject_at("a path has no NUL byte", node)
            lit = _c_str_literal(node.value)
            return lit, lit
        cfg = self._cfg_field(node)
        if cfg is not None and cfg.kind == "str":
            return f"_c->v_{cfg.name}", f"_c->v_{cfg.name}"
        _reject_at("the path of jit.open is a string literal or a str config field", node)

    def _with(self, stmt: ast.With) -> List[str]:
        if len(stmt.items) != 1:
            _reject_at("`with` takes one jit.open(path) as f", stmt)
        item = stmt.items[0]
        call = item.context_expr
        if not (isinstance(call, ast.Call) and self._jit_name(call.func) == "open"):
            _reject(_describe(stmt), stmt)
        if len(call.args) != 1 or call.keywords:
            _reject_at("jit.open takes one path", call)
        if not isinstance(item.optional_vars, ast.Name):
            _reject_at("bind the file: with jit.open(path) as f", stmt)
        fname = item.optional_vars.id
        if fname in self.locals or fname in self.files:
            _reject_at(f"{fname} is already a name of this body", item.optional_vars)
        self._no_await(stmt.body, "a with block")
        path_c, _ = self._path_text(call.args[0])
        reads: List[int] = []
        iterated = False
        for sub in ast.walk(ast.Module(body=stmt.body, type_ignores=[])):
            if isinstance(sub, ast.For) and isinstance(sub.iter, ast.Name) and sub.iter.id == fname:
                iterated = True
            if (
                isinstance(sub, ast.Call)
                and isinstance(sub.func, ast.Attribute)
                and isinstance(sub.func.value, ast.Name)
                and sub.func.value.id == fname
            ):
                if sub.func.attr != "read":
                    _reject_at(
                        f"file method .{sub.func.attr}; use `for line in f` or f.read(n)", sub
                    )
                if len(sub.args) != 1 or sub.keywords:
                    _reject_at("f.read takes one byte count", sub)
                n = sub.args[0]
                if not (
                    isinstance(n, ast.Constant)
                    and isinstance(n.value, int)
                    and not isinstance(n.value, bool)
                    and n.value > 0
                ):
                    _reject_at("f.read takes a positive integer literal", n)
                if reads and reads[0] != n.value:
                    _reject_at("one file reads with one size", n)
                reads.append(n.value)
        if iterated and reads:
            _reject_at("one file is read with `for line in f` or with f.read(n), not both", stmt)
        mode = "chunks" if reads else "lines"
        handle = self._new_res(
            "dftu_chunk_reader*" if mode == "chunks" else "dftu_line_reader*", "fh"
        )
        self.files[fname] = (handle, mode)
        self.file_gen[fname] = self.file_gen.get(fname, 0) + 1
        self.file_path[fname] = path_c
        self.uses.add("fileio")
        self.uses.add("cap")
        self.uses.add("stats")
        outer = self._target()
        n_label = self._label_n
        self._label_n += 1
        wexit = f"_wexit{n_label}"
        open_call = (
            f"_fio->chunks_open(&_cap.c, host->h, {path_c}, {reads[0]}, &{handle})"
            if mode == "chunks"
            else f"_fio->lines_open(&_cap.c, host->h, {path_c}, 0, 0, &{handle})"
        )
        close_call = (
            f"_fio->chunks_close(host->h, {handle})"
            if mode == "chunks"
            else f"_fio->lines_close(host->h, {handle})"
        )
        self._targets.append(wexit)
        defs, body = self._compile_scope(stmt.body)
        self._targets.pop()
        del self.files[fname]
        lines = ["{", "    {", "        _cap.has = 0;", f"        dftu_status _o = {open_call};"]
        lines.append(
            f'        if (_o < 0) {{ _st = dftu_jit_fail(ctx, &_cap, _o, "open", {path_c}, '
            f"{self._loc_text(stmt)}); _unw = 1; goto {outer}; }}"
        )
        lines += ["        dftu_jit_stat_inc(2);", "    }", "    {"]
        lines.extend("        " + ln for ln in defs + body)
        lines += [
            "    }",
            f"{wexit}: ;",
            f"    {close_call};",
            f"    {handle} = NULL;",
            "    dftu_jit_stat_inc(3);",
            f"    if (_unw) goto {outer};",
            "}",
        ]
        return lines

    def _for(self, stmt: ast.For) -> List[str]:
        if stmt.orelse:
            _reject_at("`for` with `else`", stmt)
        it = stmt.iter
        # for line in f
        if isinstance(it, ast.Name) and it.id in self.files:
            handle, mode = self.files[it.id]
            if mode != "lines":
                _reject_at(f"{it.id} is read with read(n); it cannot be iterated", it)
            if not isinstance(stmt.target, ast.Name):
                _reject_at("the loop variable of `for line in f` is one name", stmt.target)
            self._no_await(stmt.body, "a for loop")
            if it.id in self._iter_files:
                _reject_at(
                    f"{it.id} is already iterated by an enclosing for loop; "
                    "a second loop over it would overwrite the line of the first",
                    it,
                )
            var = self._new_local(stmt.target.id, "view", stmt.target)
            self._loop_seq += 1
            self.file_gen[it.id] = self.file_gen.get(it.id, 0) + 1
            var.origins = [("loop", self._loop_seq), ("file", it.id, self.file_gen[it.id])]
            path_c = self.file_path[it.id]
            self._loops += 1
            self._active_loops.append(self._loop_seq)
            self._iter_files.append(it.id)
            defs, body = self._compile_scope(stmt.body)
            self._iter_files.pop()
            self._active_loops.pop()
            self._loops -= 1
            lines = [
                "{",
                "    dftu_line_batch _lb;",
                "    uint32_t _li = 0;",
                "    _lb.lines = NULL;",
                "    _lb.first_line = 0;",
                "    _lb.count = 0;",
                "    for (;;) {",
                "        if (_li >= _lb.count) {",
                "            dftu_task* _t = NULL;",
                "            _cap.has = 0;",
                f"            dftu_status _s = _fio->lines_next(&_cap.c, host->h, {handle}, 0, &_lb, &_t);",
                "            if (_s >= 0) _s = _tsk->run(&_cap.c, host->h, _t);",
                "            if (_s == DFTU_END) break;",
                f"            if (_s < 0) {{ {self._fail('_s', 'read', path_c, stmt)} }}",
                "            _li = 0;",
                "        }",
                f"        {var.cname} = dftu_jit_view((const char*)_lb.lines[_li].data, "
                "(uint32_t)_lb.lines[_li].len);",
                "        ++_li;",
            ]
            lines.extend("        " + ln for ln in defs + body)
            lines += ["    }", "}"]
            self.uses.add("task")
            return lines
        # for x in value / for k, x in value.items()
        val = self._val_expr(it)
        pairs = (
            isinstance(it, ast.Call)
            and isinstance(it.func, ast.Attribute)
            and it.func.attr == "items"
            and not it.args
            and self._val_expr(it.func.value) is not None
        )
        if pairs:
            assert isinstance(it, ast.Call) and isinstance(it.func, ast.Attribute)
            val = self._val_expr(it.func.value)
        if val is None:
            _reject(_describe(stmt), stmt)
        self._no_await(stmt.body, "a for loop")
        if pairs:
            if not (
                isinstance(stmt.target, ast.Tuple)
                and len(stmt.target.elts) == 2
                and builtins.all(isinstance(t, ast.Name) for t in stmt.target.elts)
            ):
                _reject_at("`for key, value in v.items()` takes two names", stmt.target)
            key_t, val_t = stmt.target.elts
            assert isinstance(key_t, ast.Name) and isinstance(val_t, ast.Name)
            key_loc = self._new_local(key_t.id, "view", key_t)
            item_loc = self._new_local(val_t.id, "val", val_t)
            assert isinstance(it, ast.Call) and isinstance(it.func, ast.Attribute)
            doc_origins = self._origins_of(it.func.value)
            key_loc.origins = [("loop", self._loop_seq + 1)] + doc_origins
            item_loc.origins = builtins.list(doc_origins)
            head = [
                "{",
                f"    const dftu_value* _v = {val};",
                "    uint32_t _n = (_v && _v->kind == DFTU_VAL_OBJECT) ? _v->count : 0;",
                "    for (uint32_t _k = 0; _k < _n; ++_k) {",
                f"        {key_loc.cname} = dftu_jit_view(_v->as.members[_k].key, "
                "_v->as.members[_k].key_len);",
                f"        {item_loc.cname} = _v->as.members[_k].value;",
            ]
        else:
            if not isinstance(stmt.target, ast.Name):
                _reject_at("the loop variable of `for x in v` is one name", stmt.target)
            item_loc = self._new_local(stmt.target.id, "val", stmt.target)
            item_loc.origins = self._origins_of(it)
            head = [
                "{",
                f"    const dftu_value* _v = {val};",
                "    uint32_t _n = (_v && (_v->kind == DFTU_VAL_ARRAY || _v->kind == DFTU_VAL_OBJECT)) "
                "? _v->count : 0;",
                "    for (uint32_t _k = 0; _k < _n; ++_k) {",
                f"        {item_loc.cname} = _v->kind == DFTU_VAL_ARRAY ? &_v->as.items[_k] "
                ": _v->as.members[_k].value;",
            ]
        self._loop_seq += 1
        self._loops += 1
        self._active_loops.append(self._loop_seq)
        defs, body = self._compile_scope(stmt.body)
        self._active_loops.pop()
        self._loops -= 1
        head.extend("        " + ln for ln in defs + body)
        return head + ["    }", "}"]

    def _read_stmt(self, name: str, call: ast.Call, target: ast.expr) -> List[str]:
        assert isinstance(call.func, ast.Attribute) and isinstance(call.func.value, ast.Name)
        fname = call.func.value.id
        handle, mode = self.files[fname]
        if mode != "chunks":
            _reject_at(f"{fname} is iterated with `for`; it has no read(n)", call)
        fresh = name not in self.locals
        loc = self._new_local(name, "view", target)
        self.file_gen[fname] = self.file_gen.get(fname, 0) + 1
        self._bind_origins(name, loc, fresh, [("file", fname, self.file_gen[fname])], target)
        path_c = self.file_path[fname]
        self.uses.add("task")
        return [
            "{",
            "    dftu_bytes _cb;",
            "    _cb.data = NULL;",
            "    _cb.len = 0;",
            "    _cb.free_fn = NULL;",
            "    _cb.ud = NULL;",
            "    dftu_task* _t = NULL;",
            "    _cap.has = 0;",
            f"    dftu_status _s = _fio->chunks_next(&_cap.c, host->h, {handle}, &_cb, &_t);",
            "    if (_s >= 0) _s = _tsk->run(&_cap.c, host->h, _t);",
            f'    if (_s == DFTU_END) {{ {loc.cname} = dftu_jit_view("", 0); }}',
            f"    else if (_s < 0) {{ {self._fail('_s', 'read', path_c, call)} }}",
            f"    else {{ {loc.cname} = dftu_jit_view((const char*)_cb.data, (uint32_t)_cb.len); }}",
            "}",
        ]

    def _emit_call(self, call: ast.Call) -> List[str]:
        if not self.flow or self.in_task:
            _reject_at("jit.emit is a statement of a @jit.finalize body", call)
        if len(call.args) != 2 or call.keywords:
            _reject_at("jit.emit takes a result name and a value", call)
        name = call.args[0]
        if not (isinstance(name, ast.Constant) and isinstance(name.value, str) and name.value):
            _reject_at("the result name of jit.emit is a string literal", name)
        if name.value in self.emit_names:
            _reject_at(f"jit.emit('{name.value}') twice; a result is emitted once", name)
        if name.value in self.maps:
            _reject_at(f"'{name.value}' is a map of this plugin; emit another name", name)
        self.emit_names.append(name.value)
        kind = self._expr_kind(call.args[1])
        if kind == "val":
            _reject_at("a JSON value is emitted through as_int, as_str, ...", call.args[1])
        value = self._rhs_of_kind(call.args[1], kind)
        self.uses.add("result")
        body: List[str]
        if kind == "str" and self._is_view_node(call.args[1]):
            body = [
                f"    DftuView _ev = {self._view_expr(call.args[1])};",
                "    _ep = _ev.p;",
                "    _en = _ev.n;",
                '    if (!_ep) { _ep = ""; _en = 0; }',
            ]
        elif kind == "str":
            body = [
                f"    _ep = host->resolve(host->h, {value}, &_en);",
                '    if (!_ep) { _ep = ""; _en = 0; }',
            ]
        elif kind == "f64":
            body = [f'    _en = (uint32_t)snprintf(_eb, sizeof(_eb), "%.17g", (double)({value}));']
        else:
            body = [
                f'    _en = (uint32_t)snprintf(_eb, sizeof(_eb), "%lld", (long long)({value}));'
            ]
        return [
            "{",
            "    char _eb[48];",
            "    const char* _ep = _eb;",
            "    uint32_t _en = 0;",
            *body,
            "    dftu_result_value _rv;",
            "    memset(&_rv, 0, sizeof(_rv));",
            "    _rv.kind = DFTU_RESULT_KIND_BYTES;",
            "    _rv.u.bytes.data = _ep;",
            "    _rv.u.bytes.len = _en;",
            f"    dftu_status _er = _res->emit(ctx, host->h, {_c_str_literal(name.value)}, &_rv);",
            f"    if (_er < 0) {{ _st = _er; _unw = 1; goto {self._target()}; }}",
            "}",
        ]

    def _return(self, stmt: ast.Return) -> List[str]:
        if not self.flow:
            _reject(_describe(stmt), stmt)
        lines: List[str] = []
        if stmt.value is not None:
            if self.task_ret is None:
                _reject_at("this body returns nothing; use a bare `return`", stmt)
            kind = self._expr_kind(stmt.value)
            if kind not in ("i64", "f64"):
                _reject_at("a task returns an int or a float", stmt.value)
            expr, is_float = self._arith(stmt.value)
            if is_float and self.task_ret == "i64":
                _reject_at("this task returns an int; the value is a float", stmt.value)
            ctype = "int64_t" if self.task_ret == "i64" else "double"
            lines.append(f"if (S->out) *({ctype}*)S->out = ({ctype})({expr});")
        elif self.task_ret is not None:
            _reject_at("this task returns a value; write `return <value>`", stmt)
        lines.append(f"_unw = 1; goto {self._target()};")
        return ["{"] + ["    " + ln for ln in lines] + ["}"]

    def _task_call(self, node: ast.expr) -> "Tuple[_TaskSig, List[ast.expr]] | None":
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == self.self_name
            and node.func.attr in self.tasks
        ):
            return None
        return self.tasks[node.func.attr], builtins.list(node.args)

    def _job_of(
        self, node: ast.expr, lines: List[str], nested: bool
    ) -> "Tuple[str, List[str | None], bool]":
        """Build the job of an awaitable into ``lines``: its C variable, the kinds
        of its outputs and whether it is a bare sleep."""
        node = _unwrap_index(node)
        if not isinstance(node, ast.Call):
            _reject_at("await of something that is not a call", node)
        self._job_n += 1
        var = f"_j{self._job_n}"
        name = self._jit_name(node.func)
        if name == "sleep":
            if nested:
                _reject_at(
                    "jit.sleep inside jit.all or jit.any: race a task that sleeps and returns a "
                    "value instead",
                    node,
                )
            if len(node.args) != 1 or node.keywords:
                _reject_at("jit.sleep takes the seconds to wait", node)
            secs, _ = self._arith(node.args[0])
            lines += [
                f"dftu_job* {var} = NULL;",
                "{",
                f"    double _sv = (double)({secs});",
                "    uint64_t _ns = _sv > 0 ? (uint64_t)(_sv * 1e9) : 0;",
                f"    dftu_status _m = _tsk->sleep(ctx, host->h, _ns, &{var});",
                f"    if (_m < 0) {{ _st = _m; _unw = 1; goto {self._target()}; }}",
                "}",
            ]
            return var, [], True
        call = self._task_call(node)
        if call is not None:
            sig, args = call
            if len(args) != len(sig.params) or node.keywords:
                _reject_at(f"{sig.name} takes {len(sig.params)} argument(s), got {len(args)}", node)
            cargs: List[str] = []
            for arg, (pname, pkind) in zip(args, sig.params):
                expr, is_float = self._arith(arg)
                if is_float and pkind == "i64":
                    _reject_at(
                        f"parameter {pname} of {sig.name} is an int; the value is a float", arg
                    )
                cargs.append(f"({'int64_t' if pkind == 'i64' else 'double'})({expr})")
            lines += [
                f"dftu_job* {var} = NULL;",
                "{",
                f"    dftu_status _m = _mkjob_{sig.name}(ctx, R, {''.join(c + ', ' for c in cargs)}&{var});",
                f"    if (_m < 0) {{ _st = _m; _unw = 1; goto {self._target()}; }}",
                "}",
            ]
            self.uses.add("task")
            return var, [sig.ret], False
        if name in ("all", "any"):
            if nested:
                _reject_at(f"jit.{name} inside jit.all or jit.any", node)
            if len(node.args) < 2 or node.keywords:
                _reject_at(f"jit.{name} takes two or more task calls", node)
            kids: List[str] = []
            kinds: List[str | None] = []
            for arg in node.args:
                kid, kk, _ = self._job_of(arg, lines, True)
                if kk[0] is None:
                    _reject_at(f"jit.{name} takes tasks that return a value", arg)
                kids.append(kid)
                kinds.append(kk[0])
            if name == "any" and builtins.any(k != kinds[0] for k in kinds):
                _reject_at("the tasks of jit.any share one return type", node)
            lines += [
                f"dftu_job* {var} = NULL;",
                "{",
                f"    dftu_job* _ks[{len(kids)}] = {{{', '.join(kids)}}};",
                f"    dftu_status _m = _tsk->{name}(ctx, host->h, _ks, {len(kids)}, &{var});",
                f"    if (_m < 0) {{ _st = _m; _unw = 1; goto {self._target()}; }}",
                "}",
            ]
            return var, (kinds if name == "all" else [kinds[0]]), False
        _reject_at(
            "await of this call; the awaitables are jit.sleep(s), jit.all(...), jit.any(...), "
            "a task of this class, a store get, and a channel send or receive",
            node,
        )

    def _await_stmt(self, target: "ast.expr | None", aw: ast.Await, stmt: ast.stmt) -> List[str]:
        if not self.flow or not self.is_async:
            _reject_at("await outside an `async def` @jit.finalize or @jit.task body", aw)
        inner = _unwrap_index(aw.value)
        port = self._chan_call(inner)
        if port is not None:
            return self._port_await(target, port, stmt)
        if isinstance(inner, ast.Call) and self._store_method(inner) is not None:
            if not isinstance(target, ast.Name):
                _reject_at("assign the value of a store get: x = await self.<store>.get(k, d)", aw)
            return self._assign(target, inner, stmt)
        jl: List[str] = []
        var, kinds, sleeping = self._job_of(inner, jl, False)
        self._await_n += 1
        k = self._await_n
        outer = self._target()
        has_out = builtins.any(kd is not None for kd in kinds)
        if has_out:
            self.ob_size = builtins.max(self.ob_size, 8 * len(kinds))
        decode: List[str] = []
        if target is not None:
            if not has_out:
                _reject_at("this awaitable gives no value to assign", target)
            names: List[ast.expr] = (
                builtins.list(target.elts) if isinstance(target, ast.Tuple) else [target]
            )
            if len(names) != len(kinds):
                _reject_at(f"{len(kinds)} result(s) to unpack into {len(names)} name(s)", target)
            for idx, (tn, kd) in enumerate(zip(names, kinds)):
                if not isinstance(tn, ast.Name):
                    _reject_at("a result is assigned to a name", tn)
                assert kd is not None
                loc = self._new_local(tn.id, kd, tn)
                decode.append(f"memcpy(&{loc.cname}, S->ob + {8 * idx}, 8);")
        cancel = _c_str_literal(
            f"jit.Cancelled [{self._loc_text_plain(stmt)}]: cancelled at an await"
        )
        out = "S->ob" if has_out else "NULL"
        lines = ["{"]
        lines.extend("    " + ln for ln in jl)
        lines += [
            f"    dftu_status _w = _tsk->start(ctx, host->h, {var}, NULL, {out}, &wait[0]);",
            f"    if (_w < 0) {{ _st = _w; _unw = 1; goto {outer}; }}",
            "    *n_wait = 1;",
            "    S->tk = wait[0];",
            f"    S->pc = {k};",
            "    return DFTU_OK;",
            "}",
            f"case {k}: ;",
            "{",
            "    if (dftu_check(ctx) < 0) {",
            "        dftu_jit_stat_inc(6);",
            f"        _st = dftu_fail(ctx, DFTU_E_CANCELLED, {cancel}); _unw = 1; goto {outer};",
            "    }",
            "    dftu_status _w = _tsk->status(ctx, host->h, S->tk);",
            "    S->tk = NULL;",
        ]
        if sleeping:
            lines.append("    if (_w == DFTU_E_TIMEOUT) _w = DFTU_OK;")
        lines.append(f"    if (_w < 0) {{ _st = _w; _unw = 1; goto {outer}; }}")
        lines.extend("    " + ln for ln in decode)
        lines.append("}")
        return lines

    def _loc_text_plain(self, node: ast.AST) -> str:
        file, line, col = self._loc(node)
        return f"{file}:{line}:{col}"

    task_ret: "str | None" = None

    def _cfg_field(self, node: ast.expr) -> "_CfgField | None":
        """The config field ``self.config.<name>`` names, else None."""
        if not (
            self.config_fields
            and isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Attribute)
            and node.value.attr == "config"
            and isinstance(node.value.value, ast.Name)
            and node.value.value.id == self.self_name
        ):
            return None
        field = self.config_fields.get(node.attr)
        if field is None:
            _reject(f"unknown config field self.config.{node.attr}", node)
        return field

    # ---- plain-container scan state and jit.Store ----------------------------

    def _self_base(self, node: ast.expr) -> "str | None":
        """The attribute name of the ``self.<attr>`` at the base of a chain of
        subscripts, attributes and calls, else None."""
        cur: ast.expr = node
        while True:
            if isinstance(cur, ast.Subscript):
                cur = cur.value
            elif isinstance(cur, ast.Call):
                cur = cur.func
            elif isinstance(cur, ast.Attribute):
                if isinstance(cur.value, ast.Name) and cur.value.id == self.self_name:
                    return cur.attr
                cur = cur.value
            else:
                return None

    def _plain_attr(self, node: ast.expr) -> "str | None":
        base = self._self_base(node)
        return base if base is not None and base in self.plain else None

    def _mentions_state(self, node: ast.AST) -> bool:
        for sub in ast.walk(node):
            if isinstance(sub, ast.Attribute) and isinstance(sub.value, ast.Name):
                if sub.value.id == self.self_name and (
                    sub.attr in self.plain or sub.attr in self.stores
                ):
                    return True
        return False

    def _merge_error(self, field: _PlainField, node: ast.AST, what: str) -> NoReturn:
        _reject_at(
            f"cannot infer how worker slices merge self.{field.name} from {what}; "
            f"a {field.kind} is merged only when every update is {_STATE_FORMS[field.kind]} "
            "(an explicit merge() for any other update is not implemented yet)",
            node,
        )

    def _reject_expr(self, node: ast.expr) -> NoReturn:
        if isinstance(node, ast.Await):
            _reject_at(
                "await is a statement of its own, or the whole right side of an assignment "
                "(x = await ...); it cannot sit inside a condition or an expression",
                node,
            )
        base = self._self_base(node)
        if base is not None and base in self.plain:
            _reject_at(
                f"self.{base} is scan state and is write-only in the body: each worker "
                "holds only its own slice, so a read would see a partial value",
                node,
            )
        if base is not None and base in self.stores:
            _reject_at(
                f"self.{base} is a jit.Store: use self.{base}.get(key, default) "
                f"and self.{base}.put(key, value)",
                node,
            )
        if base is not None and base in self.chans:
            _reject_at(
                f"self.{base} is a port: a call on it is a statement of its own "
                "(self.out.send(v)), or the right side of an assignment "
                "(ok = self.out.try_send(v), x = self.latest.get(0), x = await self.inp.recv()); "
                "it cannot sit inside an expression or a condition",
                node,
            )
        _reject(_describe(node), node)

    def _dict_keys(self, field: _PlainField, target: ast.expr) -> List[ast.expr]:
        if not (
            isinstance(target, ast.Subscript)
            and isinstance(target.value, ast.Attribute)
            and target.value.attr == field.name
        ):
            self._merge_error(field, target, "this target")
        keys = self._keys(target.slice)
        if len(keys) != len(field.key_types):
            _reject_at(
                f"self.{field.name} takes {len(field.key_types)} key components, got {len(keys)}",
                target,
            )
        return keys

    def _state_call(
        self, field: _PlainField, node: ast.expr, method: str, key_dump: str
    ) -> "List[ast.expr] | None":
        """The arguments of ``self.<field>.<method>(key, ...)`` for that key."""
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == method
            and self._plain_attr(node.func.value) == field.name
            and isinstance(node.func.value, ast.Attribute)
            and len(node.args) == 2
            and not node.keywords
            and ast.dump(_unwrap_index(node.args[0])) == key_dump
        ):
            return None
        return builtins.list(node.args)

    def _dict_assign(self, field: _PlainField, stmt: ast.Assign) -> "_Update":
        target = stmt.targets[0]
        keys = self._dict_keys(field, target)
        assert isinstance(target, ast.Subscript)
        key_dump = ast.dump(_unwrap_index(target.slice))
        value = stmt.value
        if isinstance(value, ast.BinOp) and isinstance(value.op, ast.Add):
            for state_side, other in ((value.left, value.right), (value.right, value.left)):
                got = self._state_call(field, state_side, "get", key_dump)
                zero = got[1] if got is not None else None
                if (
                    zero is not None
                    and isinstance(zero, ast.Constant)
                    and not isinstance(zero.value, bool)
                    and isinstance(zero.value, (int, float))
                    and zero.value == 0
                ):
                    return _Update(field.name, "sum", keys, other, False)
        if (
            isinstance(value, ast.Call)
            and isinstance(value.func, ast.Name)
            and value.func.id in ("max", "min")
            and len(value.args) == 2
            and not value.keywords
        ):
            form = value.func.id
            for state_side, other in (
                (value.args[0], value.args[1]),
                (value.args[1], value.args[0]),
            ):
                if (
                    isinstance(state_side, ast.Subscript)
                    and self._plain_attr(state_side.value) == field.name
                    and isinstance(state_side.value, ast.Attribute)
                    and ast.dump(_unwrap_index(state_side.slice)) == key_dump
                ):
                    if not field.factory:
                        _reject_at(
                            f"self.{field.name}[k] raises KeyError for a missing key on a plain "
                            f"dict: declare {field.name} = defaultdict({_STATE_PY[field.elem]}) "
                            f"or write self.{field.name}[k] = {form}(self.{field.name}.get(k, x), x)",
                            state_side,
                        )
                    return _Update(field.name, form, keys, other, True)
                got = self._state_call(field, state_side, "get", key_dump)
                if got is not None and ast.dump(got[1]) == ast.dump(other):
                    return _Update(field.name, form, keys, other, False)
        self._merge_error(field, stmt, "this assignment")

    def _plain_update(self, stmt: ast.stmt) -> "_Update | None":
        """The merge-inferable update ``stmt`` makes to a plain-container
        field, None when ``stmt`` touches none, and a compile error when it
        touches one in a form whose merge cannot be inferred."""
        if isinstance(stmt, ast.AugAssign):
            attr = self._plain_attr(stmt.target)
            if attr is None:
                return None
            field = self.plain[attr]
            if not isinstance(stmt.op, ast.Add):
                self._merge_error(field, stmt, "an augmented operator other than +=")
            if field.kind == "scalar":
                if not (isinstance(stmt.target, ast.Attribute) and stmt.target.attr == attr):
                    self._merge_error(field, stmt, "this target")
                return _Update(attr, "sum", [], stmt.value, False)
            if field.kind == "dict":
                keys = self._dict_keys(field, stmt.target)
                if not field.factory:
                    _reject_at(
                        f"self.{attr}[k] += x raises KeyError for a missing key on a plain "
                        f"dict: declare {attr} = defaultdict({_STATE_PY[field.elem]}) or write "
                        f"self.{attr}[k] = self.{attr}.get(k, 0) + x",
                        stmt,
                    )
                return _Update(attr, "sum", keys, stmt.value, False)
            self._merge_error(field, stmt, "+=")
        if isinstance(stmt, ast.Assign):
            for tgt in stmt.targets:
                attr = self._plain_attr(tgt)
                if attr is None:
                    continue
                field = self.plain[attr]
                if len(stmt.targets) != 1 or field.kind != "dict":
                    self._merge_error(field, stmt, "this assignment")
                return self._dict_assign(field, stmt)
            return None
        if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call):
            call = stmt.value
            func = call.func
            attr = self._plain_attr(call)
            if attr is None:
                return None
            field = self.plain[attr]
            method = {"set": "add", "list": "append"}.get(field.kind)
            if (
                isinstance(func, ast.Attribute)
                and method is not None
                and func.attr == method
                and isinstance(func.value, ast.Attribute)
                and func.value.attr == attr
                and len(call.args) == 1
                and not call.keywords
            ):
                return _Update(
                    attr, "add" if method == "add" else "append", [], call.args[0], False
                )
            self._merge_error(field, stmt, "this call")
        if _child_blocks(stmt) is None and self._mentions_state(stmt):
            attr = self._plain_attr_in(stmt)
            if attr is not None:
                self._merge_error(self.plain[attr], stmt, "this statement")
        return None

    def _plain_attr_in(self, node: ast.AST) -> "str | None":
        for sub in ast.walk(node):
            if isinstance(sub, ast.Attribute) and isinstance(sub.value, ast.Name):
                if sub.value.id == self.self_name and sub.attr in self.plain:
                    return sub.attr
        return None

    def _infer_plain(self, stmts: List[ast.stmt]) -> None:
        """Pass one: the merge of each plain field from its updates, which must
        agree, then its accumulator declaration."""
        for stmt in stmts:
            blocks = _child_blocks(stmt)
            if blocks is not None:
                for block in blocks:
                    self._infer_plain(block)
                continue
            self._cur = stmt
            update = self._plain_update(stmt)
            if update is None:
                continue
            sig = (update.form, update.identity)
            prev = self.plain_forms.get(update.attr)
            if prev is not None and prev != sig:
                _reject_at(
                    f"self.{update.attr} is updated as {prev[0]} elsewhere and as {sig[0]} here "
                    "(or with a different missing-key rule); a field has one merge",
                    stmt,
                )
            self.plain_forms[update.attr] = sig

    def _declare_plain(self) -> None:
        for attr, (form, _) in self.plain_forms.items():
            field = self.plain[attr]
            monoid: _Monoid
            if form == "sum":
                monoid = (
                    Counter("DFTU_AGG_SUM", elem="i64")
                    if field.elem == "i64"
                    else Sum("DFTU_AGG_SUM", elem="f64")
                )
            elif form == "max":
                monoid = Max("DFTU_AGG_MAX", elem=field.elem)
            elif form == "min":
                monoid = Min("DFTU_AGG_MIN", elem=field.elem)
            elif form == "add":
                monoid = SetV("DFTU_AGG_SET_UNION", elem=field.elem)
            else:
                monoid = ListV("DFTU_AGG_LIST_SORTED", needs_by=True, elem=field.elem)
            self.maps[attr] = _MapDecl(field.key_types, (monoid,), False)

    def _check_family(self, node: ast.expr, kind: str, what: str) -> None:
        is_str = self._is_str_element(node)
        if kind == "str" and not is_str:
            _reject_at(f"{what} is a str and needs a string field such as e.name", node)
        if kind != "str" and is_str:
            _reject_at(f"{what} is a number and cannot be a string field", node)

    def _plain_stmt(self, stmt: ast.stmt) -> "List[str] | None":
        """Pass two: the contribution rows of one plain-container update."""
        update = self._plain_update(stmt)
        if update is None:
            return None
        field = self.plain[update.attr]
        decl = self.maps[update.attr]
        monoid = decl.values[0]
        for kn, kt in zip(update.keys, decl.key_types):
            self._check_family(kn, "str" if kt.dft == "DFTU_T_STR" else "i64", "this key component")
        if update.form in ("add", "append"):
            self._check_family(update.value, field.elem, "this element")
        lines: List[str] = []
        if update.identity:
            zero = ast.copy_location(ast.Constant(0), update.value)
            lines += self._emit_row(update.attr, update.keys, 0, monoid, zero, None)
        by: ast.expr | None = None
        if update.form == "append":
            by = ast.copy_location(
                ast.Attribute(ast.Name(self.event_name, ast.Load()), "ts", ast.Load()),
                update.value,
            )
        lines += self._emit_row(update.attr, update.keys, 0, monoid, update.value, by)
        return lines

    def _store_method(self, node: ast.expr) -> "Tuple[_StoreField, str] | None":
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Attribute)
            and isinstance(node.func.value.value, ast.Name)
            and node.func.value.value.id == self.self_name
            and node.func.value.attr in self.stores
        ):
            return None
        return self.stores[node.func.value.attr], node.func.attr

    def _store_key_args(self, field: _StoreField, node: ast.expr) -> List[str]:
        keys = self._keys(node)
        if len(keys) != len(field.key_kinds):
            _reject_at(
                f"self.{field.name} takes {len(field.key_kinds)} key components, got {len(keys)}",
                node,
            )
        out: List[str] = []
        for kn, kind in zip(keys, field.key_kinds):
            self._check_family(kn, kind, "this key component")
            if kind == "str":
                out.append(f"(dftu_str)({self._value(kn)})")
            else:
                expr, is_float = self._arith(kn)
                if is_float:
                    _reject_at("a float expression cannot be an int store key", kn)
                out.append(f"(int64_t)({expr})")
        return out

    def _store_get(self, node: ast.expr) -> "Tuple[str, bool] | None":
        found = self._store_method(node)
        if found is None:
            return None
        field, method = found
        assert isinstance(node, ast.Call)
        if self.in_task:
            _reject_at(
                "a task runs beside others and cannot use the store; use it in the "
                "@jit.finalize body and pass the value as an argument",
                node,
            )
        if method != "get":
            if method == "put":
                _reject_at("self.<store>.put(...) is a statement, not a value", node)
            _reject_at(f"store method .{method}; use get(key, default) and put(key, value)", node)
        if len(node.args) != 2 or node.keywords:
            _reject_at(f"self.{field.name}.get takes a key and a default value", node)
        keys = self._store_key_args(field, node.args[0])
        self._check_family(node.args[1], "i64", "this default")
        default = self._rhs(node.args[1], field.value_kind)
        if self._targets and node is not self._rhs_node:
            _reject_at(
                "inside a try body a store get is the whole right side of an assignment "
                "(v = self.<store>.get(k, d)), so a failed get leaves no update half done",
                node,
            )
        self.store_gets.add(field.name)
        self._store_read = True
        args = ", ".join(["&_se", "host", *keys, default])
        return f"_st_get_{field.name}({args})", field.value_kind == "f64"

    def _store_put(self, call: ast.Call) -> "List[str] | None":
        found = self._store_method(call)
        if found is None:
            return None
        field, method = found
        if self.in_task:
            _reject_at(
                "a task runs beside others and cannot use the store; use it in the "
                "@jit.finalize body and pass the value as an argument",
                call,
            )
        if method != "put":
            _reject_at(
                f"store method .{method} as a statement; use put(key, value) (get returns a value)",
                call,
            )
        if len(call.args) != 2 or call.keywords:
            _reject_at(f"self.{field.name}.put takes a key and a value", call)
        keys = self._store_key_args(field, call.args[0])
        self._check_family(call.args[1], "i64", "this value")
        value = self._rhs(call.args[1], field.value_kind)
        self.store_puts.add(field.name)
        return [f"_st_put_{field.name}({', '.join(['&_se', 'host', *keys, value])});"]

    # ---- ports: channels, latest values and phase results --------------------

    def _chan_call(self, node: ast.expr) -> "Tuple[_PortDecl, str, ast.Call] | None":
        """``(port, method, call)`` for ``self.<port>.<method>(...)``, else None."""
        node = _unwrap_index(node)
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Attribute)
            and isinstance(node.func.value.value, ast.Name)
            and node.func.value.value.id == self.self_name
            and node.func.value.attr in self.chans
        ):
            return None
        return self.chans[node.func.value.attr], node.func.attr, node

    def _port_where(self) -> str:
        if not self.flow:
            return "the per-event body"
        return "a @jit.task" if self.in_task else "the @jit.finalize body"

    def _port_fail(self, text: str, node: ast.AST) -> str:
        msg = _c_str_literal(f"jit.Error [{self._loc_text_plain(node)}]: {text}")
        return f"_st = dftu_fail(ctx, DFTU_E_INVALID, {msg}); _unw = 1; goto {self._target()};"

    def _port_handle(self, decl: "_PortDecl") -> str:
        self.chan_uses.add(decl.attr)
        self.uses.add("ports")
        return f"R->ch_{decl.attr}" if self.flow else f"_ch_{decl.attr}"

    def _port_value(self, decl: "_PortDecl", node: ast.expr, what: str) -> str:
        self._check_family(node, "i64", what)
        expr, is_float = self._arith(node)
        if decl.elem == "i64":
            if is_float:
                _reject_at(
                    f"self.{decl.attr} carries an int; {what} is a float (round it first)", node
                )
            return f"(int64_t)({expr})"
        return f"(double)({expr})"

    def _port_args(self, decl: "_PortDecl", method: str, call: ast.Call, count: int) -> None:
        if call.keywords or len(call.args) != count:
            _reject_at(
                f"self.{decl.attr}.{method}() takes "
                + ("no argument" if count == 0 else f"{count} argument(s)"),
                call,
            )

    def _port_checks(self, decl: "_PortDecl", method: str, call: ast.Call, awaited: bool) -> None:
        """The rules a call of ``method`` on ``decl`` follows in this body."""
        attr = decl.attr
        methods = {
            "channel": ("send", "try_send", "close", "recv"),
            "latest": ("set", "get"),
            "phase": ("contribute", "seal", "result"),
        }[decl.kind]
        if method not in methods:
            _reject_at(
                f"self.{attr}.{method}(); a {decl.kind} port has " + ", ".join(methods), call
            )
        if decl.kind == "channel":
            sends = method in ("send", "try_send", "close")
            if sends and decl.role == "recv":
                _reject_at(f"self.{attr} is a receiver; it has recv(), not {method}()", call)
            if not sends and decl.role == "send":
                _reject_at(f"self.{attr} is a sender; it has send(), try_send() and close()", call)
            if decl.policy == "block" and not self.flow:
                _reject_at(
                    f"self.{attr} is a Block channel, used from an async @jit.finalize or "
                    "@jit.task, never from the per-event body: a full channel would stop the "
                    "scan loop that has to drain it",
                    call,
                )
            if method in ("recv", "close") and not self.flow:
                _reject_at(
                    f"self.{attr}.{method}() is a call of the @jit.finalize body or a @jit.task"
                    + (": it waits" if method == "recv" else ": it ends the sends of every worker"),
                    call,
                )
            if method == "send" and awaited and not (self.flow and self.is_async):
                _reject_at("await outside an `async def` @jit.finalize or @jit.task body", call)
            if method == "send" and not awaited and decl.policy == "block":
                _reject_at(
                    f"self.{attr} is a Block channel: a send waits for a credit; write "
                    f"`await self.{attr}.send(v)` in an async @jit.finalize or @jit.task body",
                    call,
                )
            if method in ("try_send", "close") and awaited:
                _reject_at(f"self.{attr}.{method}() never waits; do not await it", call)
            if method == "recv" and not awaited:
                _reject_at(
                    f"self.{attr}.recv() waits for a value: `await self.{attr}.recv()`", call
                )
        elif method in ("seal", "result") and not self.flow:
            _reject_at(
                f"self.{attr}.{method}() is a call of the @jit.finalize body or a @jit.task: "
                "a phase is open to contributions while the scan runs",
                call,
            )
        elif awaited:
            _reject_at(f"self.{attr}.{method}() never waits; do not await it", call)

    def _port_frame_fields(self) -> None:
        if not self._port_frame:
            self._port_frame = True
            self.local_decls += ["int32_t pst;", "uint32_t plen;", "int64_t pval;"]

    def _port_stmt(self, call_info: "Tuple[_PortDecl, str, ast.Call]") -> List[str]:
        """A statement that is a port call whose value, if any, is unused."""
        decl, method, call = call_info
        self._port_checks(decl, method, call, False)
        if method in ("get", "result"):
            _reject_at(f"the value of self.{decl.attr}.{method}() is unused; assign it", call)
        return self._port_lines(decl, method, call, None)

    def _port_assign(
        self, target: ast.expr, call_info: "Tuple[_PortDecl, str, ast.Call]", stmt: ast.stmt
    ) -> List[str]:
        decl, method, call = call_info
        self._port_checks(decl, method, call, False)
        if method not in ("try_send", "get", "result"):
            _reject_at(f"self.{decl.attr}.{method}() gives no value to assign", call)
        if not isinstance(target, ast.Name):
            _reject_at("a result is assigned to a name", target)
        kind = "i64" if method == "try_send" else decl.elem
        assert kind is not None
        loc = self._new_local(target.id, kind, target)
        return self._port_lines(decl, method, call, loc.cname)

    def _port_lines(
        self, decl: "_PortDecl", method: str, call: ast.Call, dest: "str | None"
    ) -> List[str]:
        self.uses.add("ports")
        attr, name = decl.attr, decl.full
        tgt = self._target()
        ctype = "int64_t" if decl.elem == "i64" else "double"
        err = f"{{ _st = _s; _unw = 1; goto {tgt}; }}"
        if method in ("send", "try_send"):
            self._port_args(decl, method, call, 1)
            value = self._port_value(decl, call.args[0], "the value")
            h = self._port_handle(decl)
            lines = [
                "{",
                f"    {ctype} _pv = {value};",
                "    dftu_port_status _ps = DFTU_PORT_OK;",
                f"    dftu_status _s = _prt->channel_try_send(ctx, host->h, {h}, &_pv, 8, &_ps);",
                f"    if (_s < 0) {err}",
            ]
            if method == "try_send":
                if dest is not None:
                    lines.append(f"    {dest} = (_ps == DFTU_PORT_OK) ? 1 : 0;")
            else:
                here = self._loc_text_plain(call)
                full = _c_str_literal(f"jit.Error [{here}]: channel '{name}' is full")
                closed = _c_str_literal(f"jit.Error [{here}]: channel '{name}' is closed")
                lines.append(
                    "    if (_ps != DFTU_PORT_OK) { _st = dftu_fail(ctx, DFTU_E_INVALID, "
                    f"_ps == DFTU_PORT_FULL ? {full} : {closed}); _unw = 1; goto {tgt}; }}"
                )
            return lines + ["}"]
        if method == "close":
            self._port_args(decl, method, call, 0)
            h = self._port_handle(decl)
            return [
                "{",
                f"    dftu_status _s = _prt->channel_close(ctx, host->h, {h});",
                f"    if (_s < 0) {err}",
                "}",
            ]
        if method == "set":
            self._port_args(decl, method, call, 1)
            value = self._port_value(decl, call.args[0], "the value")
            return [
                "{",
                f"    {ctype} _pv = {value};",
                "    uint64_t _gen = 0;",
                f"    dftu_status _s = _prt->latest_set(ctx, host->h, {_c_str_literal(name)}, &_pv, 8, &_gen);",
                f"    if (_s < 0) {err}",
                "}",
            ]
        if method == "contribute":
            self._port_args(decl, method, call, 1)
            value = self._port_value(decl, call.args[0], "the value")
            ty = "DFTU_T_I64" if decl.elem == "i64" else "DFTU_T_F64"
            assert decl.merge is not None
            sealed = self._port_fail(f"the phase result '{name}' is sealed", call)
            return [
                "{",
                f"    {ctype} _pv = {value};",
                "    dftu_port_status _ps = DFTU_PORT_OK;",
                f"    dftu_status _s = _prt->phase_contribute(ctx, host->h, {_c_str_literal(name)}, 1, "
                f"{_channels.MERGES[decl.merge]}, {ty}, &_pv, 8, &_ps);",
                f"    if (_s < 0) {err}",
                f"    if (_ps != DFTU_PORT_OK) {{ {sealed} }}",
                "}",
            ]
        if method == "seal":
            self._port_args(decl, method, call, 0)
            return [
                "{",
                f"    dftu_status _s = _prt->phase_seal(ctx, host->h, {_c_str_literal(name)}, 1);",
                f"    if (_s < 0) {err}",
                "}",
            ]
        assert dest is not None and method in ("get", "result")
        if method == "get":
            self._port_args(decl, method, call, 1)
            default = self._port_value(decl, call.args[0], "the default")
            read = (
                f"_prt->latest_get(ctx, host->h, {_c_str_literal(name)}, _pb, 8, &_pl, &_gen, &_ps)"
            )
            absent = f"{dest} = {default};"
            blocked = ""
        else:
            if call.keywords or len(call.args) > 1:
                _reject_at(f"self.{attr}.result() takes at most a default value", call)
            default = (
                self._port_value(decl, call.args[0], "the default")
                if call.args
                else ("0" if decl.elem == "i64" else "0.0")
            )
            read = (
                f"_prt->phase_result(ctx, host->h, {_c_str_literal(name)}, 1, _pb, 8, &_pl, &_ps)"
            )
            absent = self._port_fail(
                f"the phase result '{name}' is still open: call seal() first", call
            )
            blocked = f"if (_ps == DFTU_PORT_NOT_READY) {{ {absent} }}"
        odd = self._port_fail(f"'{name}' holds a value that is not an 8-byte int or float", call)
        lines = [
            "{",
            "    unsigned char _pb[8];",
            "    uint32_t _pl = 0;",
            *(["    uint64_t _gen = 0;"] if method == "get" else []),
            "    dftu_port_status _ps = DFTU_PORT_OK;",
            f"    dftu_status _s = {read};",
            f"    if (_s < 0) {err}",
        ]
        if method == "get":
            lines += [
                f"    if (_ps == DFTU_PORT_NOT_READY) {{ {dest} = {default}; }}",
                f"    else if (_ps == DFTU_PORT_OK && _pl == 8) {{ memcpy(&{dest}, _pb, 8); }}",
                f"    else {{ {odd} }}",
            ]
        else:
            lines += [
                f"    {blocked}",
                f"    else if (_ps == DFTU_PORT_OK && _pl == 0) {{ {dest} = {default}; }}",
                f"    else if (_ps == DFTU_PORT_OK && _pl == 8) {{ memcpy(&{dest}, _pb, 8); }}",
                f"    else {{ {odd} }}",
            ]
        return lines + ["}"]

    def _port_wait(self, start: List[str], resume: List[str], node: ast.AST) -> List[str]:
        """The lines of one await on a task the ports service started: start it,
        park the frame, and on resume check the cancel, take the status and run
        ``resume``."""
        self._port_frame_fields()
        self.uses.add("task")
        self._await_n += 1
        k = self._await_n
        outer = self._target()
        cancel = _c_str_literal(
            f"jit.Cancelled [{self._loc_text_plain(node)}]: cancelled at an await"
        )
        return [
            "{",
            *["    " + ln for ln in start],
            f"    if (_w < 0) {{ _st = _w; _unw = 1; goto {outer}; }}",
            "    *n_wait = 1;",
            "    S->tk = wait[0];",
            f"    S->pc = {k};",
            "    return DFTU_OK;",
            "}",
            f"case {k}: ;",
            "{",
            "    if (dftu_check(ctx) < 0) {",
            "        dftu_jit_stat_inc(6);",
            f"        _st = dftu_fail(ctx, DFTU_E_CANCELLED, {cancel}); _unw = 1; goto {outer};",
            "    }",
            "    dftu_status _w = _tsk->status(ctx, host->h, S->tk);",
            "    S->tk = NULL;",
            f"    if (_w < 0) {{ _st = _w; _unw = 1; goto {outer}; }}",
            *["    " + ln for ln in resume],
            "}",
        ]

    def _port_recv_start(self, decl: "_PortDecl") -> List[str]:
        h = self._port_handle(decl)
        return [
            "S->pst = DFTU_PORT_OK;",
            "S->plen = 0;",
            "S->pval = 0;",
            "dftu_status _w = _prt->channel_recv_exact(ctx, host->h, "
            f"{h}, &S->pval, 8, &S->plen, &S->pst, &wait[0]);",
        ]

    def _port_await(
        self,
        target: "ast.expr | None",
        call_info: "Tuple[_PortDecl, str, ast.Call]",
        stmt: ast.stmt,
    ) -> List[str]:
        decl, method, call = call_info
        self._port_checks(decl, method, call, True)
        if decl.kind != "channel" or method not in ("send", "recv"):
            _reject_at(f"self.{decl.attr}.{method}() is not awaited", call)
        name = decl.full
        here = self._loc_text_plain(stmt)
        self.uses.add("ports")
        if method == "send":
            self._port_args(decl, method, call, 1)
            if target is not None:
                _reject_at("a send gives no value to assign", target)
            ctype = "int64_t" if decl.elem == "i64" else "double"
            value = self._port_value(decl, call.args[0], "the value")
            h = self._port_handle(decl)
            start = [
                "S->pst = DFTU_PORT_OK;",
                f"{ctype} _pv = {value};",
                "memcpy(&S->pval, &_pv, 8);",
                f"dftu_status _w = _prt->channel_send(ctx, host->h, {h}, &S->pval, 8, &S->pst, &wait[0]);",
            ]
            tgt = self._target()
            full = _c_str_literal(f"jit.Error [{here}]: channel '{name}' is full")
            closed = _c_str_literal(f"jit.Error [{here}]: channel '{name}' is closed")
            resume = [
                "if (S->pst != DFTU_PORT_OK) { _st = dftu_fail(ctx, DFTU_E_INVALID, "
                f"S->pst == DFTU_PORT_FULL ? {full} : {closed}); _unw = 1; goto {tgt}; }}"
            ]
            return self._port_wait(start, resume, stmt)
        self._port_args(decl, method, call, 0)
        decode: List[str] = []
        if target is not None:
            if not isinstance(target, ast.Name):
                _reject_at("a result is assigned to a name", target)
            assert decl.elem is not None
            loc = self._new_local(target.id, decl.elem, target)
            decode.append(f"memcpy(&{loc.cname}, &S->pval, 8);")
        tgt = self._target()
        closed_msg = _c_str_literal(f"jit.Closed [{here}]: channel '{name}' is closed and drained")
        start = self._port_recv_start(decl)
        resume = [
            "if (S->pst == DFTU_PORT_CLOSED) {",
            f"    _pclosed = 1; _st = dftu_fail(ctx, DFTU_E_NOT_FOUND, {closed_msg}); _unw = 1; goto {tgt};",
            "}",
            "if (S->pst != DFTU_PORT_OK) { "
            + self._port_fail(
                f"channel '{name}' carried a message that is not an 8-byte int or float", stmt
            )
            + " }",
            *decode,
        ]
        return self._port_wait(start, resume, stmt)

    def _async_for(self, stmt: ast.AsyncFor) -> List[str]:
        if not (self.flow and self.is_async):
            _reject_at("`async for` needs an `async def` @jit.finalize or @jit.task body", stmt)
        if stmt.orelse:
            _reject_at("`async for` with `else`", stmt)
        it = stmt.iter
        decl = (
            self.chans.get(it.attr)
            if isinstance(it, ast.Attribute)
            and isinstance(it.value, ast.Name)
            and it.value.id == self.self_name
            else None
        )
        if decl is None or decl.kind != "channel" or decl.role == "send":
            _reject_at(
                "`async for` takes a receiver or channel of this plugin: "
                "`async for v in self.inp:`",
                it,
            )
        if not isinstance(stmt.target, ast.Name):
            _reject_at("the loop variable of `async for` is one name", stmt.target)
        assert decl.elem is not None
        loc = self._new_local(stmt.target.id, decl.elem, stmt.target)
        self._loop_seq += 1
        self._loops += 1
        self._active_loops.append(self._loop_seq)
        defs, body = self._compile_scope(stmt.body)
        self._active_loops.pop()
        self._loops -= 1
        odd = self._port_fail(
            f"channel '{decl.full}' carried a message that is not an 8-byte int or float", stmt
        )
        self.uses.add("ports")
        start = self._port_recv_start(decl)
        resume = [
            "if (S->pst == DFTU_PORT_CLOSED) break;",
            f"if (S->pst != DFTU_PORT_OK) {{ {odd} }}",
            f"memcpy(&{loc.cname}, &S->pval, 8);",
        ]
        lines = ["for (;;) {"]
        lines += ["    " + ln for ln in self._port_wait(start, resume, stmt)]
        lines += ["    " + ln for ln in defs + body]
        lines.append("}")
        return lines

    def _slice_elts(self, sl: ast.expr) -> List[ast.expr]:
        sl = _unwrap_index(sl)
        return builtins.list(sl.elts) if isinstance(sl, ast.Tuple) else [sl]

    # Hoist each expression used more than once in this straight-line scope into
    # a local computed once, then compile the scope's statements against those
    # locals. Nested guard bodies are their own scopes (a guarded expression is
    # not hoisted above its guard, preserving its execution condition).
    def _compile_scope(self, stmts: List[ast.stmt]) -> Tuple[List[str], List[str]]:
        outer = self._cse
        self._cse = dict(outer)
        counts: Dict[str, int] = {}
        reps: Dict[str, ast.expr] = {}
        for stmt in stmts:
            root = stmt.test if isinstance(stmt, ast.If) else stmt
            for node in _unconditional_nodes(root):
                if isinstance(node, ast.expr) and self._is_cse_candidate(node):
                    d = ast.dump(node)
                    counts[d] = counts.get(d, 0) + 1
                    reps.setdefault(d, node)
        cands = [reps[d] for d, c in counts.items() if c >= 2 and d not in self._cse]
        # Define inner (smaller) expressions first so an outer local can name
        # them.
        order = sorted(range(len(cands)), key=lambda i: builtins.sum(1 for _ in ast.walk(cands[i])))
        shared = [cands[i] for i in order]
        defs: List[str] = []
        for node in shared:
            d = ast.dump(node)
            if d in self._cse:
                continue
            self._cur = node
            expr, is_float = self._arith(node)
            name = f"_cse{self._cse_n}"
            self._cse_n += 1
            defs.append(f"{'double' if is_float else 'int64_t'} {name} = ({expr});")
            self._cse[d] = (name, is_float)
        lines: List[str] = []
        for stmt in stmts:
            lines.extend(self._stmt(stmt))
        self._cse = outer
        return defs, lines

    def _mentions_local(self, node: ast.AST) -> bool:
        known = (self.self_name, self.event_name, "jit")
        for sub in ast.walk(node):
            if isinstance(sub, ast.Name) and sub.id not in known and sub.id not in self.ops:
                return True
        return False

    def _is_cse_candidate(self, node: ast.expr) -> bool:
        if not self._cse_ok or self._mentions_local(node):
            return False
        if self._mentions_state(node):
            return False
        if isinstance(node, ast.BinOp) and type(node.op) in _ARITH_OP:
            return True
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
            f = node.func
            if isinstance(f.value, ast.Name):
                if f.value.id == "jit" and f.attr in _PRIM_C:
                    return True
                if f.value.id == self.event_name and f.attr in _ARG_METHODS:
                    return True
        return False

    def _target(self) -> str:
        return self._targets[-1] if self._targets else self._exit_label

    _exit_label = "_ev_exit"

    def _loc(self, node: ast.AST) -> Tuple[str, int, int]:
        src = self._source
        assert src is not None
        return (
            src.file,
            src.first + getattr(node, "lineno", 1) - 1,
            getattr(node, "col_offset", 0) + src.margin + 1,
        )

    def _jit_name(self, node: ast.expr) -> "str | None":
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id == "jit"
        ):
            return node.attr
        return None

    def _raise(self, stmt: ast.Raise) -> List[str]:
        if stmt.exc is None:
            _reject_at("a bare `raise` re-raises a caught exception; raise jit.Fatal(...)", stmt)
        if stmt.cause is not None:
            _reject_at("`raise ... from ...` is not supported; raise jit.Fatal(...)", stmt)
        call = stmt.exc
        kind = self._jit_name(call.func) if isinstance(call, ast.Call) else None
        if not isinstance(call, ast.Call) or kind is None:
            _reject_at(
                f"raise of {_describe(call)}; a compiled body raises only jit.Fatal(...), "
                "jit.SkipRest() or jit.Cancelled()",
                call,
            )
        if kind not in _RAISABLE:
            _reject_at(
                f"raise of jit.{kind}; a compiled body raises only jit.Fatal(...), "
                "jit.SkipRest() or jit.Cancelled() (jit.Error and its subclasses are what a "
                "failed call raises, and `except` catches them)",
                call,
            )
        if call.keywords or len(call.args) > 1:
            _reject_at(f"jit.{kind} takes at most one string-literal message", call)
        message = kind
        if call.args:
            arg = call.args[0]
            if not (isinstance(arg, ast.Constant) and isinstance(arg.value, str)):
                _reject_at(f"the message of jit.{kind} must be a string literal", arg)
            message = arg.value
        code = _RAISABLE[kind]
        target = self._target()
        if kind == "SkipRest":
            return [f"{{ _unw = 1; goto {target}; }}"]
        file, line, col = self._loc(stmt)
        text = _c_str_literal(f"jit.{kind} [{file}:{line}:{col}]: {message}")
        return [f"{{ _st = dftu_fail(ctx, {code}, {text}); _unw = 1; goto {target}; }}"]

    def _except_cond(self, node: ast.expr) -> str:
        names = (
            [self._jit_name(e) for e in node.elts]
            if isinstance(node, ast.Tuple)
            else [self._jit_name(node)]
        )
        conds: List[str] = []
        for name, at in zip(names, node.elts if isinstance(node, ast.Tuple) else [node]):
            if name is None:
                _reject_at(
                    "`except` names a Python class; catch jit.Error or one of its subclasses "
                    "(jit.NotFound, jit.Closed, jit.IoError, jit.ParseError)",
                    at,
                )
            if name in _UNCATCHABLE:
                _reject_at(
                    f"jit.{name} ends the run and cannot be caught; {_UNCATCHABLE[name]}", at
                )
            if name == "Closed" and not (self.flow and self.is_async):
                _reject_at(
                    "jit.Closed comes from `await self.<channel>.recv()`; catch it in an async "
                    "@jit.finalize or @jit.task body",
                    at,
                )
            cond = _CATCH_COND.get(name)
            if cond is None:
                _reject_at(
                    f"`except jit.{name}`; catch jit.Error or one of its subclasses "
                    "(jit.NotFound, jit.IoError, jit.ParseError)",
                    at,
                )
            conds.append(cond)
        return " || ".join(conds)

    def _try(self, stmt: ast.Try) -> List[str]:
        if stmt.orelse or stmt.finalbody:
            _reject_at("`try` with `else` or `finally`; use try and except only", stmt)
        if not stmt.handlers:
            _reject_at("`try` needs an `except` clause", stmt)
        n = self._label_n
        self._label_n += 1
        catch, end = f"_catch{n}", f"_tend{n}"
        self._targets.append(catch)
        defs, body = self._compile_scope(stmt.body)
        self._targets.pop()
        outer = self._target()
        lines = ["{"]
        lines.append("    {")
        lines.extend("        " + ln for ln in defs + body)
        lines.append("    }")
        lines += [f"    goto {end};", f"{catch}:", "    if (_unw) {"]
        branch = "if"
        for handler in stmt.handlers:
            if handler.type is None:
                _reject_at("a bare `except` catches everything; catch jit.Error", handler)
            if handler.name is not None:
                _reject_at(
                    "`except ... as name` is not supported; the handler cannot read it", handler
                )
            cond = self._except_cond(handler.type)
            hdefs, hbody = self._compile_scope(handler.body)
            lines.append(f"        {branch} (_st < 0 && ({cond})) {{")
            lines.append("            _unw = 0;")
            lines.append("            _st = DFTU_OK;")
            if self.flow:
                lines.append("            _pclosed = 0;")
            lines.extend("            " + ln for ln in hdefs + hbody)
            lines.append("        }")
            branch = "else if"
        lines += [f"        else goto {outer};", "    }", f"{end}: ;", "}"]
        return lines

    def _stmt(self, stmt: ast.stmt) -> List[str]:
        self._cur = stmt
        if isinstance(stmt, ast.Raise):
            return self._raise(stmt)
        if isinstance(stmt, ast.Try):
            return self._try(stmt)
        if (self._targets or self.flow) and self.stores:
            self._store_read = False
            lines = self._stmt_plain(stmt)
            if self._store_read:
                lines.append(
                    f"if (_se.st < 0) {{ _st = _se.st; _se.st = DFTU_OK; _unw = 1; "
                    f"goto {self._target()}; }}"
                )
            return lines
        return self._stmt_plain(stmt)

    def _stmt_plain(self, stmt: ast.stmt) -> List[str]:
        if isinstance(stmt, ast.If):
            guard = self._cond(stmt.test)
            defs, body = self._compile_scope(stmt.body)
            lines = [f"if ({guard}) {{"]
            lines.extend("    " + ln for ln in defs)
            lines.extend("    " + ln for ln in body)
            if not stmt.orelse:
                lines.append("}")
                return lines
            if len(stmt.orelse) == 1 and isinstance(stmt.orelse[0], ast.If):
                chained = self._stmt(stmt.orelse[0])
                lines.append("} else " + chained[0])
                lines.extend(chained[1:])
                return lines
            edefs, ebody = self._compile_scope(stmt.orelse)
            lines.append("} else {")
            lines.extend("    " + ln for ln in edefs)
            lines.extend("    " + ln for ln in ebody)
            lines.append("}")
            return lines
        return self._add_stmt(stmt)

    def _add_stmt(self, stmt: ast.stmt) -> List[str]:
        if self.plain:
            plain_lines = self._plain_stmt(stmt)
            if plain_lines is not None:
                return plain_lines
        flow_lines = self._flow_stmt(stmt)
        if flow_lines is not None:
            return flow_lines
        if isinstance(stmt, ast.AugAssign):
            return self._aug(stmt)
        if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call):
            if self.stores:
                store_lines = self._store_put(stmt.value)
                if store_lines is not None:
                    return store_lines
            func = stmt.value.func
            if isinstance(func, ast.Attribute) and func.attr == "append":
                return self._append(stmt.value)
            return self._observe(stmt.value)
        if isinstance(stmt, (ast.Assign, ast.AnnAssign)) and stmt.value is not None:
            attr = self._plain_attr_in(stmt.value)
            if attr is not None:
                _reject_at(
                    f"self.{attr} is scan state and is write-only in the body: each worker "
                    "holds only its own slice, so a read would see a partial value",
                    stmt.value,
                )
        _reject(_describe(stmt), stmt)

    def _cond(self, node: ast.expr) -> str:
        """C text for a per-event condition: ``and`` / ``or`` / ``not``,
        comparisons (chained, ``in`` a literal tuple, ``is None``) and a bare
        flag or number as its own truth value."""
        if isinstance(node, ast.BoolOp):
            sym = "&&" if isinstance(node.op, ast.And) else "||"
            return "(" + f" {sym} ".join(self._cond(v) for v in node.values) + ")"
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return f"!({self._cond(node.operand)})"
        if isinstance(node, ast.Compare):
            parts = []
            left = node.left
            for op, right in zip(node.ops, node.comparators):
                parts.append(self._cmp_pair(left, op, right, node))
                left = right
            return parts[0] if len(parts) == 1 else "(" + " && ".join(parts) + ")"
        if isinstance(node, ast.NamedExpr):
            _reject("assignment expression (:=)", node)
        flag = self._cfg_field(node)
        if flag is not None and flag.kind == "bool":
            return f"_cfg_{flag.name} != 0"
        if self._is_str_element(node) or self._is_str_literal(node):
            _reject_at("a string is not a truth value; compare it with ==, != or `is None`", node)
        if (
            isinstance(node, ast.Attribute)
            and node.attr == "has_dur"
            and self._is_event(node.value)
        ):
            return self._field("has_dur")
        expr, _ = self._arith(node)
        return f"({expr}) != 0"

    def _is_event(self, node: ast.expr) -> bool:
        return isinstance(node, ast.Name) and node.id == self.event_name

    def _cmp_pair(self, left: ast.expr, op: ast.cmpop, right: ast.expr, whole: ast.expr) -> str:
        op_t = type(op)
        if isinstance(op, (ast.Is, ast.IsNot)):
            return self._is_none(left, right, isinstance(op, ast.Is), whole)
        if isinstance(op, (ast.In, ast.NotIn)):
            if not isinstance(right, (ast.Tuple, ast.List, ast.Set)):
                _reject_at("`in` needs a literal tuple, list or set", right)
            if not builtins.all(isinstance(e, ast.Constant) for e in right.elts):
                _reject_at("`in` takes only literals", right)
            if not right.elts:
                return "0" if isinstance(op, ast.In) else "1"
            alts = [self._cmp_pair(left, ast.Eq(), e, whole) for e in right.elts]
            joined = "(" + " || ".join(alts) + ")"
            return joined if isinstance(op, ast.In) else f"!{joined}"
        sym = _CMP_OP.get(op_t)
        if sym is None:
            _reject("comparison operator", whole)
        str_cmp = self._str_compare(left, right, op_t)
        if str_cmp is not None:
            return str_cmp
        lexpr, _ = self._arith(left)
        rexpr, _ = self._arith(right)
        return f"{lexpr} {sym} {rexpr}"

    def _is_none(self, left: ast.expr, right: ast.expr, positive: bool, whole: ast.expr) -> str:
        if not (isinstance(right, ast.Constant) and right.value is None):
            _reject("`is` comparison (only `is None` / `is not None`)", whole)
        opt = self._cfg_field(left)
        if opt is not None:
            if not opt.optional:
                _reject_at(f"self.config.{opt.name} is not optional, so it is never None", left)
            return f"!_has_{opt.name}" if positive else f"_has_{opt.name}"
        operand = self._str_operand(left)
        if operand is not None:
            return f"{operand} {'==' if positive else '!='} DFTU_STR_NONE"
        _reject_at(
            "`is None` applies to an optional self.config field, a string field such as "
            "e.fhash or e.arg_str(...); a numeric field is never None",
            left,
        )

    def _is_none_sentinel(self, node: ast.expr) -> bool:
        return (
            isinstance(node, ast.Attribute)
            and node.attr == "NONE"
            and isinstance(node.value, ast.Name)
            and node.value.id == "jit"
        )

    def _str_compare(self, a: ast.expr, b: ast.expr, op_t: type) -> str | None:
        a_none = self._is_none_sentinel(a)
        b_none = self._is_none_sentinel(b)
        a_str = a_none or self._is_str_literal(a) or self._is_str_element(a)
        b_str = b_none or self._is_str_literal(b) or self._is_str_element(b)
        if not a_str and not b_str:
            return None
        if a_str != b_str:
            _reject(
                "comparison between a string and a number (a string field compares only with a string)",
                a,
            )
        if op_t not in (ast.Eq, ast.NotEq):
            _reject("string comparison operator other than == / !=", a)
        if (self._is_view_node(a) or self._is_view_node(b)) and not (a_none or b_none):
            eq = f"dftu_jit_veq({self._view_expr(a)}, {self._view_expr(b)})"
            return eq if op_t is ast.Eq else f"!{eq}"
        sides = []
        for side, is_none in ((a, a_none), (b, b_none)):
            if is_none:
                sides.append("DFTU_STR_NONE")
                continue
            cfg = self._cfg_field(side)
            if cfg is not None:
                sides.append(f"_cfgs_{cfg.name}")
            elif isinstance(side, ast.Constant):
                assert isinstance(side.value, str)
                sides.append(self._str_literal(side.value))
            else:
                operand = self._str_operand(side)
                if operand is None:
                    _reject_at("a string compared with something that is not a string field", side)
                sides.append(operand)
        return f"{sides[0]} {_CMP_OP[op_t]} {sides[1]}"

    def _is_str_literal(self, node: ast.expr) -> bool:
        """A string literal, or a str config field (an interned constant)."""
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return True
        cfg = self._cfg_field(node)
        return cfg is not None and cfg.kind == "str"

    def _str_operand(self, node: ast.expr) -> str | None:
        if isinstance(node, ast.Name) and node.id in self.locals:
            loc = self._local(node)
            if loc.kind == "view":
                return f"dftu_jit_vid(host, &{loc.cname})"
            return loc.cname if loc.kind == "str" else None
        vc = self._val_call(node)
        if vc is not None:
            return vc[1] if vc[0] == "str" else None
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id == self.event_name
            and node.attr in _STR_FIELDS
        ):
            return self._field(node.attr)
        arg = self._arg_call(node)
        if arg is not None and arg[0] == "arg_str":
            expr, _ = self._lower_arg(*arg)
            return expr
        return None

    def _arg_call(self, node: ast.expr) -> Tuple[str, str] | None:
        """(method, arg-name) for an ``e.arg_*("NAME")`` call; None if not an event
        method call, JitError for an unknown method or a non-literal name."""
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == self.event_name
        ):
            return None
        method = node.func.attr
        if method not in _ARG_METHODS:
            _reject(f"event method e.{method}(...); use arg_i64 / arg_f64 / arg_str", node)
        if len(node.args) != 1 or node.keywords:
            _reject(f"e.{method} takes exactly one string-literal arg name", node)
        name_node = node.args[0]
        if not (isinstance(name_node, ast.Constant) and isinstance(name_node.value, str)):
            _reject(f"e.{method} arg name must be a string literal", name_node)
        return method, name_node.value

    def _prim_call(self, node: ast.expr) -> Tuple[str, str, List[ast.expr]] | None:
        """(c-function, arg-cast, arg-nodes) for a ``jit.<prim>(...)`` numeric
        primitive; None when not a ``jit.`` call at all, JitError for a bad
        prim or arity."""
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "jit"
        ):
            return None
        prim = node.func.attr
        spec = _PRIM_C.get(prim)
        if spec is None:
            _reject(f"jit.{prim}(...) in an expression; expected a numeric primitive", node)
        c_fn, arity, cast = spec
        if len(node.args) != arity or node.keywords:
            _reject(f"jit.{prim}() takes exactly {arity} argument(s)", node)
        return c_fn, cast, builtins.list(node.args)

    def _op_call(self, node: ast.expr) -> Tuple[str, bool] | None:
        """(c-expr, is_float) for a call to a referenced ``@jit.op`` transform,
        inlining its C body on first use; None when not an op call. A
        ``self.<store>.get(...)`` call is lowered here too."""
        if self.stores:
            store_get = self._store_get(node)
            if store_get is not None:
                return store_get
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id in self.ops
        ):
            return None
        name = node.func.id
        op_obj = self.ops[name]
        if len(node.args) != 1 or node.keywords:
            _reject(f"jit.op '{name}' takes exactly one positional argument", node)
        c_name = f"dftu_jitop_{name}"
        if name not in self.op_emitted:
            self.op_defs.append(jit_op.emit_inline(op_obj, c_name))
            self.op_emitted.add(name)
        arg_expr, _ = self._arith(node.args[0])
        in_ct = jit_op.c_type(op_obj.in_type)
        return f"{c_name}(({in_ct})({arg_expr}))", jit_op.is_float_type(op_obj.out_type)

    def _lower_arg(self, method: str, name: str) -> Tuple[str, bool]:
        """Lower ``e.arg_*("NAME")`` to a read of the batch-resolved "args.NAME"
        dyn column, returning (expr, is_float); the column is resolved once per
        batch in on_batch, by name, not re-scanned per row."""
        self.arg_helpers.add(method)
        if name not in self.arg_keys:
            self.arg_keys.append(name)
        col = f"_argcol{self.arg_keys.index(name)}"
        if method == "arg_str":
            return f"dftu_jit_arg_str(host, &{col}, i)", False
        fn, _dflt = _ARG_METHODS[method]
        return f"{fn}(&{col}, i)", method == "arg_f64"

    def _str_literal(self, s: str) -> str:
        if self.flow:
            return f"dftu_jit_lit(host, {_c_str_literal(s)}, {len(s.encode('utf-8'))})"
        if s not in self.str_literals:
            self.str_literals.append(s)
        return f"lit_{self.str_literals.index(s)}"

    def _resolve_accum(self, target: ast.expr) -> Tuple[str, List[ast.expr], int, _Monoid]:
        comp_name: str | None = None
        node = target
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Subscript):
            comp_name = node.attr
            node = node.value
        if not isinstance(node, ast.Subscript):
            _reject("assignment target (expected self.<map>[(...)])", target)
        subs: List[ast.expr] = []
        cur: ast.expr = node
        while isinstance(cur, ast.Subscript):
            subs.append(cur.slice)
            cur = cur.value
        subs.reverse()
        base = cur
        if not (
            isinstance(base, ast.Attribute)
            and isinstance(base.value, ast.Name)
            and base.value.id == self.self_name
        ):
            _reject("assignment target (expected self.<map>[(...)])", target)
        decl = self.maps.get(base.attr)
        if decl is None:
            _reject(f"unknown map self.{base.attr}", base)
        return self._resolve_flat(base.attr, decl, subs, comp_name)

    def _resolve_flat(
        self, attr: str, decl: _AnyMap, subs: List[ast.expr], comp_name: str | None
    ) -> Tuple[str, List[ast.expr], int, _Monoid]:
        if decl.is_product:
            keys = self._keys(subs[0])
            comp_node = subs[1] if len(subs) > 1 else None
            if len(subs) > 2:
                _reject(f"too many subscripts for product map self.{attr}", subs[2])
            if comp_name is not None:
                comp = self._named_comp(comp_name, decl, attr)
            elif comp_node is not None:
                comp = self._comp(comp_node, decl, attr)
            else:
                _reject(f"product map self.{attr} add without a component", subs[0])
        else:
            if comp_name is not None:
                _reject(f"single-value map self.{attr} add with a component", subs[0])
            keys = []
            for sl in subs:
                keys.extend(self._keys(sl))
            comp = 0
        if len(keys) != len(decl.key_types):
            _reject(
                f"self.{attr} takes {len(decl.key_types)} key components, got {len(keys)}", subs[0]
            )
        return attr, keys, comp, decl.values[comp]

    def _publish_target(self, target: ast.expr) -> str | None:
        if (
            isinstance(target, ast.Attribute)
            and isinstance(target.value, ast.Name)
            and target.value.id == self.self_name
            and target.attr in self.ports
        ):
            return target.attr
        return None

    def _publish(self, name: str, value: ast.expr) -> List[str]:
        port = self.ports[name]
        if port.role != "publish":
            _reject(f"self.{name} is a jit.consume port; read its value, do not += into it", value)
        expr, saw_float = self._arith(value)
        if not port.is_f64 and saw_float:
            _reject("float value into a u64 publish port", value)
        cast = "double" if port.is_f64 else "uint64_t"
        return [f"_pub_{name} += ({cast})({expr});"]

    def _consume_ref(self, name: str) -> str:
        port = self.ports[name]
        if port.role != "consume":
            _reject(f"self.{name} is a jit.publish port; publish into it with +=, do not read it")
        return f"_sub_{name}"

    def _aug(self, stmt: ast.AugAssign) -> List[str]:
        if not isinstance(stmt.op, ast.Add):
            _reject("augmented operator other than +=", stmt)
        pub = self._publish_target(stmt.target)
        if pub is not None:
            return self._publish(pub, stmt.value)
        attr, keys, comp, monoid = self._resolve_accum(stmt.target)
        if monoid.dft in _LIST_OPS:
            _reject("+= on an ordered-list monoid; use .append(elem, order_by=<expr>)", stmt)
        if monoid.dft not in _ADDITIVE_OPS:
            _reject(f"+= on the non-additive monoid {monoid.dft}; use .observe(v) instead", stmt)
        return self._emit_row(attr, keys, comp, monoid, stmt.value, None)

    def _observe(self, call: ast.Call) -> List[str]:
        func = call.func
        if not (isinstance(func, ast.Attribute) and func.attr == "observe"):
            _reject("method call other than .observe(v)", call)
        attr, keys, comp, monoid = self._resolve_accum(func.value)
        if monoid.dft in _LIST_OPS:
            _reject(".observe on an ordered-list monoid; use .append(elem, order_by=<expr>)", call)
        by: ast.expr | None = None
        if monoid.needs_by:
            if len(call.args) != 1:
                _reject(".observe takes one payload and by=<expr>", call)
            for kw in call.keywords:
                if kw.arg == "by":
                    by = kw.value
                else:
                    _reject(f".observe keyword other than by ({kw.arg})", kw.value)
            if by is None:
                _reject_at(
                    f"self.{attr} .observe requires by=<expr>; the by key ranks the payloads", call
                )
        elif len(call.args) != 1 or call.keywords:
            _reject(".observe expects exactly one value argument", call)
        self._check_element(call.args[0], monoid)
        return self._emit_row(attr, keys, comp, monoid, call.args[0], by)

    def _append(self, call: ast.Call) -> List[str]:
        func = call.func
        assert isinstance(func, ast.Attribute)
        if len(call.args) != 1:
            _reject(".append expects exactly one element argument", call)
        order: ast.expr | None = None
        for kw in call.keywords:
            if kw.arg == "order_by":
                order = kw.value
            else:
                _reject(f".append keyword other than order_by ({kw.arg})", kw.value)
        attr, keys, comp, monoid = self._resolve_accum(func.value)
        if monoid.dft not in _LIST_OPS:
            _reject(f".append on the non-list monoid {monoid.dft}; use += or .observe(v)", call)
        if order is None:
            _reject_at(
                "ordered list requires order_by=<expr>; order-by-arrival is not parallel-safe", call
            )
        self._check_element(call.args[0], monoid)
        return self._emit_row(attr, keys, comp, monoid, call.args[0], order)

    def _check_element(self, node: ast.expr, monoid: _Monoid) -> None:
        if monoid.elem not in ("str", "i64"):
            return
        if monoid.dft not in (_SET_OPS | _LIST_OPS | _ARGBY_OPS | _TOPK_OPS) and monoid.dft not in (
            "DFTU_AGG_APPROX_TOPK",
            "DFTU_AGG_SAMPLE",
        ):
            return
        is_str = self._is_str_element(node)
        if monoid.elem == "str" and not is_str:
            _reject_at(
                "a str-valued element must be a string field such as e.name; "
                "use of=jit.i64 for an int64 value",
                node,
            )
        if monoid.elem == "i64" and is_str:
            _reject_at(
                "an i64-valued element must be an int64 expr such as e.dur; "
                "use of=jit.str_ for a string value",
                node,
            )

    def _is_str_element(self, node: ast.expr) -> bool:
        if isinstance(node, ast.Name) and node.id in self.locals:
            return self.locals[node.id].kind in ("str", "view")
        vc = self._val_call(node)
        if vc is not None:
            return vc[0] == "str"
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id == self.event_name
            and node.attr in _STR_FIELDS
        ):
            return True
        arg = self._arg_call(node)
        return arg is not None and arg[0] == "arg_str"

    def _emit_row(
        self,
        attr: str,
        keys: List[ast.expr],
        comp: int,
        monoid: _Monoid,
        value: ast.expr,
        by: ast.expr | None,
    ) -> List[str]:
        """Append one contribution row to ``attr``'s per-batch column buffers;
        the buffers are folded into the DFTU_SVC_AGG accumulator after the loop."""
        decl = self.maps[attr]
        if monoid.elem != "str" and monoid.dft != "DFTU_AGG_DISTINCT":
            self._check_family(value, "i64", "this value")
        self.rows_per_event[attr] = self.rows_per_event.get(attr, 0) + 1
        lines = ["{"]
        if self._has_loops:
            lines += self._grow_rows(attr, decl)
        lines.append(f"    uint32_t _r = _n_{attr}++;")
        for idx, kn in enumerate(keys):
            ktype = decl.key_types[idx].dft
            if ktype == "DFTU_T_BYTES":
                _reject("a bytes key; e.<field> exposes no raw byte blob, author it raw")
            slot = f"_k{idx}_{attr}[_r]"
            if ktype == "DFTU_T_STR":
                lines.append(f"    {slot} = (dftu_str)({self._value(kn)});")
            elif ktype in _FLOAT_TYPES:
                expr, _ = self._arith(kn)
                lines.append(f"    {slot} = (double)({expr});")
            else:
                lines.append(f"    {slot} = (int64_t)({self._value(kn)});")
        if decl.is_product:
            for c, m in enumerate(decl.values):
                lines.append(f"    _v{c}_{attr}[_r] = ({_ELEM_CTYPE[m.elem]})0;")
                lines.append(f"    _o{c}_{attr}[_r] = 0;")
                if m.needs_by:
                    lines.append(f"    _b{c}_{attr}[_r] = 0.0;")
            lines.append(f"    _o{comp}_{attr}[_r] = 1;")
        lines.append(f"    _v{comp}_{attr}[_r] = {self._rhs(value, monoid.elem)};")
        if by is not None:
            by_expr, _ = self._arith(by)
            lines.append(f"    _b{comp}_{attr}[_r] = (double)({by_expr});")
        lines.append("}")
        return lines

    def _grow_rows(self, attr: str, decl: _AnyMap) -> List[str]:
        """Room for one more row of ``attr``: a loop appends an unknown number of
        rows per event, so the batch-sized buffers grow when they fill."""
        bufs: List[Tuple[str, str]] = []
        for idx, kt in enumerate(decl.key_types):
            bufs.append((f"_k{idx}_{attr}", _key_ctype(kt.dft)[0]))
        for c, m in enumerate(decl.values):
            bufs.append((f"_v{c}_{attr}", _ELEM_CTYPE[m.elem]))
            if decl.is_product:
                bufs.append((f"_o{c}_{attr}", "uint8_t"))
            if m.needs_by:
                bufs.append((f"_b{c}_{attr}", "double"))
        fail = (
            f'{{ _st = dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory"); _unw = 1; '
            f"goto {self._target()}; }}"
        )
        lines = [
            f"    if (_n_{attr} >= _cap_{attr}) {{",
            f"        if (_cap_{attr} > 0x3fffffffu) {fail}",
            f"        uint32_t _nc = _cap_{attr} ? _cap_{attr} * 2u : 64u;",
            "        void* _gp;",
        ]
        for name, ctype in bufs:
            lines += [
                f"        _gp = realloc({name}, sizeof({ctype}) * (size_t)_nc);",
                f"        if (!_gp) {fail}",
                f"        {name} = ({ctype}*)_gp;",
            ]
        lines += [f"        _cap_{attr} = _nc;", "    }"]
        return lines

    def _comp(self, node: ast.expr, decl: _AnyMap, attr: str) -> int:
        node = _unwrap_index(node)
        if (
            not isinstance(node, ast.Constant)
            or not isinstance(node.value, int)
            or isinstance(node.value, bool)
        ):
            _reject("component index (expected an integer literal)", node)
        comp = node.value
        if comp < 0 or comp >= len(decl.values):
            _reject(f"component index {comp} out of range for self.{attr}", node)
        return comp

    def _named_comp(self, name: str, decl: _AnyMap, attr: str) -> int:
        names = decl.value_names
        if names is None or name not in names:
            _reject(f"unknown value component .{name} for self.{attr}")
        return names.index(name)

    def _keys(self, sl: ast.expr) -> List[ast.expr]:
        sl = _unwrap_index(sl)
        if isinstance(sl, ast.Tuple):
            return builtins.list(sl.elts)
        return [sl]

    def _value(self, node: ast.expr) -> str:
        node = _unwrap_index(node)
        hoisted = self._cse.get(ast.dump(node))
        if hoisted is not None:
            return hoisted[0]
        if isinstance(node, ast.Name):
            loc = self._local(node)
            if loc.kind == "val":
                _reject_at(f"{node.id} is a JSON value; read it with as_int, as_str, ...", node)
            if loc.kind == "view":
                return f"dftu_jit_vid(host, &{loc.cname})"
            return loc.cname
        vc = self._val_call(node)
        if vc is not None:
            if vc[0] == "val":
                _reject_at(
                    "a JSON value is not a number or a string; read it with as_int, as_str, ...",
                    node,
                )
            return vc[1]
        opc = self._op_call(node)
        if opc is not None:
            return opc[0]
        prim = self._prim_call(node)
        if prim is not None:
            c_fn, cast, arg_nodes = prim
            parts = [f"({cast})({self._value(a)})" for a in arg_nodes]
            return f"{c_fn}({', '.join(parts)})"
        arg = self._arg_call(node)
        if arg is not None:
            expr, _ = self._lower_arg(*arg)
            return expr
        cfg = self._cfg_field(node)
        if cfg is not None:
            return f"_cfgs_{cfg.name}" if cfg.kind == "str" else f"_cfg_{cfg.name}"
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
            if node.value.id == self.self_name and node.attr in self.ports:
                return self._consume_ref(node.attr)
            if node.value.id == self.event_name:
                return self._field(node.attr)
            if node.attr == "NONE":
                return "DFTU_STR_NONE"
        if isinstance(node, ast.Constant):
            return self._const(node)
        self._reject_expr(node)

    def _rhs(self, node: ast.expr, elem: str) -> str:
        """Cast an expression into a value buffer of family ``elem``."""
        if elem == "str":
            return f"(dftu_str)({self._value(node)})"
        expr, saw_float = self._arith(node)
        if elem != "f64" and saw_float:
            _reject("float value into a u64 monoid component", node)
        return f"({'double' if elem == 'f64' else 'int64_t'})({expr})"

    def _arith(self, node: ast.expr) -> Tuple[str, bool]:
        node = _unwrap_index(node)
        hoisted = self._cse.get(ast.dump(node))
        if hoisted is not None:
            return hoisted[0], hoisted[1]
        if isinstance(node, ast.BinOp):
            sym = _ARITH_OP.get(type(node.op))
            if sym is None:
                _reject("arithmetic operator (only + - * / are allowed)", node)
            for operand in (node.left, node.right):
                if self._is_str_element(operand):
                    _reject_at("a string field cannot be an arithmetic operand", operand)
            left, lf = self._arith(node.left)
            right, rf = self._arith(node.right)
            if isinstance(node.op, ast.Div):
                return f"((double)({left}) / (double)({right}))", True
            return f"({left} {sym} {right})", lf or rf
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.USub, ast.UAdd)):
            inner, inner_float = self._arith(node.operand)
            return (f"(-{inner})" if isinstance(node.op, ast.USub) else inner), inner_float
        if isinstance(node, ast.Name):
            loc = self._local(node)
            if loc.kind in ("i64", "f64"):
                return loc.cname, loc.kind == "f64"
            _reject_at(
                f"{node.id} is a {_KIND_WORD[loc.kind]} and cannot be used as a number", node
            )
        vc = self._val_call(node)
        if vc is not None:
            if vc[0] in ("i64", "f64"):
                return vc[1], vc[0] == "f64"
            _reject_at(f"a {_KIND_WORD[vc[0]]} cannot be used as a number", node)
        length = self._len_call(node)
        if length is not None:
            return length, False
        opc = self._op_call(node)
        if opc is not None:
            return opc
        prim = self._prim_call(node)
        if prim is not None:
            c_fn, cast, arg_nodes = prim
            parts = [f"({cast})({self._arith(a)[0]})" for a in arg_nodes]
            return f"{c_fn}({', '.join(parts)})", False
        arg = self._arg_call(node)
        if arg is not None:
            return self._lower_arg(*arg)
        cfg = self._cfg_field(node)
        if cfg is not None:
            if cfg.kind == "str":
                _reject(f"string config field self.config.{cfg.name} in arithmetic", node)
            return f"_cfg_{cfg.name}", cfg.kind == "float"
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
            if node.value.id == self.self_name and node.attr in self.ports:
                port = self.ports[node.attr]
                return self._consume_ref(node.attr), port.is_f64
            if node.value.id == self.event_name:
                return self._field(node.attr), False
        if isinstance(node, ast.Constant):
            v = node.value
            if isinstance(v, bool):
                _reject("boolean literal", node)
            if isinstance(v, int):
                return _int_literal(v), False
            if isinstance(v, float):
                return repr(v), True
            _reject("literal (only integer or float literals are allowed)", node)
        self._reject_expr(node)

    def _const(self, node: ast.Constant) -> str:
        v = node.value
        if isinstance(v, bool):
            _reject("boolean literal", node)
        if isinstance(v, int):
            return _int_literal(v)
        if isinstance(v, float):
            _reject("float literal", node)
        _reject("literal (only integer literals are allowed)", node)

    def _field(self, attr: str) -> str:
        expr = _FIELD_EXPR.get(attr)
        if expr is None:
            _reject(f"event field e.{attr}")
        self.fields_used.add(_FIELD_COLUMN[attr])
        return expr


def _port_ctype(port: _Port) -> Tuple[str, str]:
    return ("double", "0.0") if port.is_f64 else ("uint64_t", "0")


def _wire_id(port: _Port) -> str:
    """A port's resolved wire id: non-None by the time _emit runs, since
    _build_plugin resolves every publish port's name before emitting, and
    consume() never returns a port without one."""
    assert port.name is not None
    return port.name


def _emit_name_lists(provides: List[str], consumes: List[str]) -> Tuple[List[str], List[str]]:
    """The dftu_plugin::provides / ::consumes definitions and the two factory
    assignment lines. Both lists are derived, never author-declared: a plugin
    produces the ports it publishes and the accumulators it creates, and reads
    the ports it consumes."""
    defs: List[str] = []
    assigns: List[str] = []
    for slot, names in (("provides", provides), ("consumes", consumes)):
        if not names:
            assigns.append(f"    g_plugin.{slot} = NULL;")
            continue
        rows = ", ".join(_c_str_literal(n) for n in names)
        defs += [
            f"static const char* const _{slot}_names[{len(names) + 1}] = {{{rows}, NULL}};",
            f"static const char* const* {slot}(void* self) {{",
            "    (void)self;",
            f"    return _{slot}_names;",
            "}",
            "",
        ]
        assigns.append(f"    g_plugin.{slot} = {slot};")
    return defs, assigns


def _emit_port_pre(
    pub_ports: List[Tuple[str, _Port]], sub_ports: List[Tuple[str, _Port]]
) -> List[str]:
    """Batch-scoped port setup emitted before the event loop: fetch the ports
    extension, read each consume port's value for this batch, and zero each
    publish port's per-batch accumulator."""
    out: List[str] = [
        "    const dftu_svc_ports* _ports =",
        "        (const dftu_svc_ports*)host->get_service(host->h, DFTU_SVC_PORTS);",
        "    (void)_ports;",
    ]
    for name, port in sub_ports:
        ctype, zero = _port_ctype(port)
        cap = _c_str_literal(_wire_id(port))
        out += [
            f"    {ctype} _sub_{name} = {zero};",
            "    if (_ports && _ports->consume) {",
            "        uint32_t _sub_len = 0;",
            f"        const void* _sub_p = _ports->consume(host->h, _ports->port_key(host->h, {cap}), &_sub_len);",
            f"        if (_sub_p && _sub_len == 8u) std::memcpy(&_sub_{name}, _sub_p, 8);",
            "    }",
            f"    (void)_sub_{name};",
        ]
    for name, port in pub_ports:
        ctype, zero = _port_ctype(port)
        out.append(f"    {ctype} _pub_{name} = {zero};")
    return out


def _emit_port_flush(pub_ports: List[Tuple[str, _Port]]) -> List[str]:
    """Publish each publish port's per-batch accumulator after the event loop."""
    out: List[str] = []
    for name, port in pub_ports:
        cap = _c_str_literal(_wire_id(port))
        out += [
            "    if (_ports && _ports->publish) {",
            f"        _ports->publish(host->h, _ports->port_key(host->h, {cap}), &_pub_{name}, 8u);",
            "    }",
        ]
    return out


_STR_COL_HELPER = [
    "static dftu_series* dftu_jit_str_col(const dftu_plugin_host* host, const dftu_str* ids,",
    "                                     uint32_t n) {",
    "    int32_t* off = (int32_t*)malloc(sizeof(int32_t) * (size_t)(n + 1u));",
    "    if (!off) return NULL;",
    "    uint32_t tot = 0;",
    "    for (uint32_t i = 0; i < n; ++i) {",
    "        uint32_t len = 0;",
    "        const char* s = host->resolve(host->h, ids[i], &len);",
    "        if (!s) len = 0;",
    "        off[i] = (int32_t)tot;",
    "        tot += len;",
    "    }",
    "    off[n] = (int32_t)tot;",
    "    char* buf = (char*)malloc(tot ? tot : 1u);",
    "    if (!buf) { free(off); return NULL; }",
    "    for (uint32_t i = 0; i < n; ++i) {",
    "        uint32_t len = 0;",
    "        const char* s = host->resolve(host->h, ids[i], &len);",
    "        if (s && len) std::memcpy(buf + off[i], s, len);",
    "    }",
    "    dftu_series* col = dftu_series_new_string(DFTU_TYPE_STRING, off, buf, (int64_t)n, NULL);",
    "    free(buf);",
    "    free(off);",
    "    return col;",
    "}",
    "",
]

_BITMAP_HELPER = [
    "static uint8_t* dftu_jit_bitmap(const uint8_t* flags, uint32_t n) {",
    "    uint32_t nb = (n + 7u) / 8u;",
    "    uint8_t* bm = (uint8_t*)calloc(nb ? nb : 1u, 1);",
    "    if (!bm) return NULL;",
    "    for (uint32_t i = 0; i < n; ++i)",
    "        if (flags[i]) bm[i >> 3] |= (uint8_t)(1u << (i & 7u));",
    "    return bm;",
    "}",
    "",
]


def _quantile_name(q: float) -> str:
    return f"p{q * 100:g}"


def _agg_specs(decl: _AnyMap) -> List[Tuple[str, str, str, str, str]]:
    """The (op, value, out, param, by) dftu_agg_col rows for one accumulator."""
    specs: List[Tuple[str, str, str, str, str]] = []
    for c, m in enumerate(decl.values):
        if isinstance(m, Quantiles):
            specs.append(("DFTU_AGG_COUNT", "NULL", _c_str_literal("count"), "0.0", "NULL"))
            for q in m.qs:
                specs.append(
                    (
                        "DFTU_AGG_PCT",
                        _c_str_literal(f"v{c}"),
                        _c_str_literal(_quantile_name(q)),
                        repr(q),
                        "NULL",
                    )
                )
            continue
        specs.append(
            (
                m.dft,
                _c_str_literal(f"v{c}"),
                _c_str_literal(_out_name(decl, c)),
                repr(m.param),
                _c_str_literal(f"b{c}") if m.needs_by else "NULL",
            )
        )
    return specs


def _emit_buffers(attr: str, decl: _AnyMap, rows: int) -> List[str]:
    """Per-batch row buffers for one accumulator, sized to the batch."""
    out = [
        f"    uint32_t _cap_{attr} = (uint32_t)n * {rows}u;",
        f"    uint32_t _n_{attr} = 0;",
    ]
    for idx, kt in enumerate(decl.key_types):
        ct, _ = _key_ctype(kt.dft)
        out.append(
            f"    {ct}* _k{idx}_{attr} = ({ct}*)malloc(sizeof({ct}) * "
            f"(size_t)(_cap_{attr} ? _cap_{attr} : 1u));"
        )
        out.append(f"    if (!_k{idx}_{attr}) _alloc_ok = 0;")
    for c, m in enumerate(decl.values):
        ct = _ELEM_CTYPE[m.elem]
        out.append(
            f"    {ct}* _v{c}_{attr} = ({ct}*)malloc(sizeof({ct}) * "
            f"(size_t)(_cap_{attr} ? _cap_{attr} : 1u));"
        )
        out.append(f"    if (!_v{c}_{attr}) _alloc_ok = 0;")
        if decl.is_product:
            out.append(
                f"    uint8_t* _o{c}_{attr} = (uint8_t*)malloc("
                f"(size_t)(_cap_{attr} ? _cap_{attr} : 1u));"
            )
            out.append(f"    if (!_o{c}_{attr}) _alloc_ok = 0;")
        if m.needs_by:
            out.append(
                f"    double* _b{c}_{attr} = (double*)malloc(sizeof(double) * "
                f"(size_t)(_cap_{attr} ? _cap_{attr} : 1u));"
            )
            out.append(f"    if (!_b{c}_{attr}) _alloc_ok = 0;")
    return out


def _emit_flush(attr: str, decl: _AnyMap, wire_name: str) -> List[str]:
    """Build one batch frame from the row buffers and fold it into the
    DFTU_SVC_AGG accumulator named ``wire_name`` (the package-qualified id;
    ``attr`` only ever names local C buffers)."""
    cols: List[Tuple[str, List[str]]] = []
    for idx, kt in enumerate(decl.key_types):
        _, dtype = _key_ctype(kt.dft)
        if kt.dft == "DFTU_T_STR":
            cols.append((f"k{idx}", [f"dftu_jit_str_col(host, _k{idx}_{attr}, _n_{attr})"]))
        else:
            cols.append(
                (
                    f"k{idx}",
                    [f"dftu_series_new_flat({dtype}, _k{idx}_{attr}, (int64_t)_n_{attr}, NULL)"],
                )
            )
    for c, m in enumerate(decl.values):
        dtype = _ELEM_DTYPE[m.elem]
        if m.elem == "str":
            cols.append((f"v{c}", [f"dftu_jit_str_col(host, _v{c}_{attr}, _n_{attr})"]))
        elif decl.is_product:
            cols.append(
                (
                    f"v{c}",
                    [
                        f"dftu_series_new_flat({dtype}, _v{c}_{attr}, (int64_t)_n_{attr}, "
                        f"_bm_{attr})"
                    ],
                )
            )
        else:
            cols.append(
                (
                    f"v{c}",
                    [f"dftu_series_new_flat({dtype}, _v{c}_{attr}, (int64_t)_n_{attr}, NULL)"],
                )
            )
        if m.needs_by:
            cols.append(
                (
                    f"b{c}",
                    [
                        f"dftu_series_new_flat(DFTU_TYPE_FLOAT64, _b{c}_{attr}, "
                        f"(int64_t)_n_{attr}, NULL)"
                    ],
                )
            )
    n_cols = len(cols)
    specs = _agg_specs(decl)
    n_keys = len(decl.key_types)
    # The accumulator is created for every batch, empty ones included, so a map
    # that never collected a row still finalizes to a zero-row result.
    out = ["    {"]
    if n_keys:
        key_list = ", ".join(_c_str_literal(f"k{i}") for i in range(n_keys))
        out.append(f"        const char* _keys[{n_keys}] = {{{key_list}}};")
        keys_arg = "_keys"
    else:
        keys_arg = "NULL"
    spec_rows = ", ".join("{" + ", ".join(s) + "}" for s in specs)
    out += [
        f"        const dftu_agg_col _specs[{len(specs)}] = {{{spec_rows}}};",
        "        dftu_agg* _a = NULL;",
        f"        dftu_status _as = _agg->agg_new(ctx, host->h, {_c_str_literal(wire_name)}, "
        f"{keys_arg}, {n_keys}u, _specs, {len(specs)}u, &_a);",
        "        if (_as < 0 && _st >= 0) _st = _as;",
        f"        if (_as >= 0 && _a && _n_{attr} > 0) {{",
        f"            dftu_series* _cols[{n_cols}];",
        f"            const char* _cnames[{n_cols}];",
    ]
    if decl.is_product:
        out.append(f"            uint8_t* _bm_{attr} = NULL;")
    ci = 0
    for cname, col_expr in cols:
        if decl.is_product and cname.startswith("v"):
            comp = cname[1:]
            out.append(f"            _bm_{attr} = dftu_jit_bitmap(_o{comp}_{attr}, _n_{attr});")
        out.append(f"            _cols[{ci}] = {col_expr[0]};")
        out.append(f"            _cnames[{ci}] = {_c_str_literal(cname)};")
        if decl.is_product and cname.startswith("v"):
            out.append(f"            free(_bm_{attr});")
            out.append(f"            _bm_{attr} = NULL;")
        ci += 1
    out += [
        "            int _cok = 1;",
        f"            for (int _ci = 0; _ci < {n_cols}; ++_ci)",
        "                if (!_cols[_ci]) _cok = 0;",
        "            if (_cok) {",
        f"                dftu_dataframe* _df = dftu_dataframe_new(_cnames, _cols, {n_cols});",
        "                if (_df) {",
        "                    _as = _agg->agg_accumulate(ctx, host->h, _a, _df);",
        "                    if (_as < 0 && _st >= 0) _st = _as;",
        "                    dftu_dataframe_free(_df);",
        "                }",
        "            } else {",
        f"                for (int _ci = 0; _ci < {n_cols}; ++_ci)",
        "                    if (_cols[_ci]) dftu_series_free(_cols[_ci]);",
        "            }",
        "        }",
        "    }",
    ]
    return out


def _emit_frees(attr: str, decl: _AnyMap) -> List[str]:
    out = [f"    free(_k{idx}_{attr});" for idx in range(len(decl.key_types))]
    for c, m in enumerate(decl.values):
        out.append(f"    free(_v{c}_{attr});")
        if decl.is_product:
            out.append(f"    free(_o{c}_{attr});")
        if m.needs_by:
            out.append(f"    free(_b{c}_{attr});")
    return out


# The store codec of dftracer.utils.store_codec in C, for int, float, str and
# tuple keys and int and float values, so a Python Namespace reads and writes
# what compiled code does.
_STORE_HELPERS = [
    "struct DftuBuf {",
    "    uint8_t* p;",
    "    uint32_t n;",
    "    uint32_t cap;",
    "    int bad;",
    "};",
    "",
    "struct DftuStoreEnv {",
    "    dftu_ctx* ctx;",
    "    void* h;",
    "    const dftu_svc_store* e;",
    "    dftu_store_snapshot_t* snap;",
    "    dftu_status st;",
    "    DftuBuf kb;",
    "    DftuBuf vb;",
    "    dftu_store_ns_t* ns[DFTU_ST_COUNT];",
    "    dftu_store_batch_t* batch[DFTU_ST_COUNT];",
    "};",
    "",
    "static void dftu_buf_byte(DftuBuf* b, uint8_t c) {",
    "    if (b->bad) return;",
    "    if (b->n == b->cap) {",
    "        uint32_t cap = b->cap ? b->cap * 2u : 32u;",
    "        uint8_t* p = (uint8_t*)realloc(b->p, cap);",
    "        if (!p) {",
    "            b->bad = 1;",
    "            return;",
    "        }",
    "        b->p = p;",
    "        b->cap = cap;",
    "    }",
    "    b->p[b->n++] = c;",
    "}",
    "",
    "static void dftu_enc_int(DftuBuf* b, int64_t v) {",
    "    if (v == 0) {",
    "        dftu_buf_byte(b, 0x14);",
    "        return;",
    "    }",
    "    uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;",
    "    int size = 0;",
    "    for (uint64_t t = mag; t; t >>= 8) ++size;",
    "    uint64_t bits = mag;",
    "    if (v > 0) {",
    "        dftu_buf_byte(b, (uint8_t)(0x14 + size));",
    "    } else {",
    "        dftu_buf_byte(b, (uint8_t)(0x14 - size));",
    "        bits = (size == 8 ? ~(uint64_t)0 : (((uint64_t)1 << (8 * size)) - 1)) - mag;",
    "    }",
    "    for (int i = size - 1; i >= 0; --i) dftu_buf_byte(b, (uint8_t)(bits >> (8 * i)));",
    "}",
    "",
    "static void dftu_enc_f64(DftuBuf* b, double v) {",
    "    uint64_t bits;",
    "    memcpy(&bits, &v, sizeof(bits));",
    "    bits = (bits >> 63) ? ~bits : (bits ^ ((uint64_t)1 << 63));",
    "    dftu_buf_byte(b, 0x21);",
    "    for (int i = 7; i >= 0; --i) dftu_buf_byte(b, (uint8_t)(bits >> (8 * i)));",
    "}",
    "",
    "static void dftu_enc_str(DftuStoreEnv* s, const dftu_plugin_host* host, DftuBuf* b,",
    "                         dftu_str id, const char* field) {",
    "    uint32_t len = 0;",
    "    const char* text = id == DFTU_STR_NONE ? NULL : host->resolve(host->h, id, &len);",
    "    if (!text) {",
    "        char msg[160];",
    '        snprintf(msg, sizeof(msg), "store %s: a store key is a missing string", field);',
    "        s->st = dftu_fail(s->ctx, DFTU_E_INVALID, msg);",
    "        return;",
    "    }",
    "    dftu_buf_byte(b, 0x02);",
    "    for (uint32_t i = 0; i < len; ++i) {",
    "        dftu_buf_byte(b, (uint8_t)text[i]);",
    "        if (text[i] == 0) dftu_buf_byte(b, 0xff);",
    "    }",
    "    dftu_buf_byte(b, 0x00);",
    "}",
    "",
    "static int dftu_dec_int(dftu_store_bytes v, int64_t* out) {",
    "    if (v.size < 1) return 0;",
    "    uint8_t t = v.data[0];",
    "    if (t == 0x14) {",
    "        *out = 0;",
    "        return v.size == 1;",
    "    }",
    "    int size = t > 0x14 ? (int)t - 0x14 : 0x14 - (int)t;",
    "    if (size < 1 || size > 8 || v.size != (uint64_t)size + 1) return 0;",
    "    uint64_t m = 0;",
    "    for (int i = 0; i < size; ++i) m = (m << 8) | v.data[1 + i];",
    "    if (t > 0x14) {",
    "        if (m > (uint64_t)INT64_MAX) return 0;",
    "        *out = (int64_t)m;",
    "        return 1;",
    "    }",
    "    uint64_t mag = (size == 8 ? ~(uint64_t)0 : (((uint64_t)1 << (8 * size)) - 1)) - m;",
    "    if (mag > ((uint64_t)1 << 63)) return 0;",
    "    *out = (int64_t)((uint64_t)0 - mag);",
    "    return 1;",
    "}",
    "",
    "static int dftu_dec_f64(dftu_store_bytes v, double* out) {",
    "    if (v.size != 9 || v.data[0] != 0x21) return 0;",
    "    uint64_t bits = 0;",
    "    for (int i = 1; i < 9; ++i) bits = (bits << 8) | v.data[i];",
    "    bits = (bits >> 63) ? (bits ^ ((uint64_t)1 << 63)) : ~bits;",
    "    memcpy(out, &bits, sizeof(bits));",
    "    return 1;",
    "}",
    "",
]


def _store_key_lines(field: "_StoreField", buf: str) -> List[str]:
    lines = [f"    dftu_buf_byte({buf}, 0x05);"] if field.is_tuple else []
    for idx, kind in enumerate(field.key_kinds):
        if kind == "str":
            lines.append(f"    dftu_enc_str(s, host, {buf}, k{idx}, {_c_str_literal(field.name)});")
        else:
            lines.append(f"    dftu_enc_int({buf}, k{idx});")
    if field.is_tuple:
        lines.append(f"    dftu_buf_byte({buf}, 0x00);")
    return lines


def _emit_store_defs(
    stores: "Dict[str, _StoreField]",
    gets: "builtins.set[str]",
    puts: "builtins.set[str]",
) -> List[str]:
    """File-scope C for the plugin's :class:`Store` fields: the codec, one typed
    ``get`` and ``put`` per field the body uses, and the per-batch open/close."""
    count = len(stores)
    names = ", ".join(_c_str_literal(f.name) for f in stores.values())
    put_flags = ", ".join("1" if f.name in puts else "0" for f in stores.values())
    out = [f"#define DFTU_ST_COUNT {count}", ""]
    out += _STORE_HELPERS
    out += [
        f"static const char* const _ST_NAMES[] = {{{names}}};",
        f"static const int _ST_PUT[] = {{{put_flags}}};",
        f"static const int _ST_HAS_GETS = {1 if gets else 0};",
        "",
    ]
    for f in stores.values():
        params = "".join(
            f", {'dftu_str' if k == 'str' else 'int64_t'} k{i}" for i, k in enumerate(f.key_kinds)
        )
        ctype = "double" if f.value_kind == "f64" else "int64_t"
        dec = "dftu_dec_f64" if f.value_kind == "f64" else "dftu_dec_int"
        enc = "dftu_enc_f64" if f.value_kind == "f64" else "dftu_enc_int"
        kind_name = "a float" if f.value_kind == "f64" else "an int"
        if f.name in gets:
            out += [
                f"static {ctype} _st_get_{f.name}(DftuStoreEnv* s, const dftu_plugin_host* host{params},",
                f"                         {ctype} dflt) {{",
                "    if (s->st < 0) return dflt;",
                "    s->kb.n = 0;",
                *_store_key_lines(f, "&s->kb"),
                "    if (s->st < 0) return dflt;",
                "    if (s->kb.bad) {",
                '        s->st = dftu_fail(s->ctx, DFTU_E_NO_MEMORY, "out of memory");',
                "        return dflt;",
                "    }",
                "    dftu_store_scope sc = {DFTU_STORE_SCOPE_ROOT, NULL, 0};",
                "    dftu_store_value_t* v = NULL;",
                f"    dftu_status r = s->e->get(s->ctx, s->snap, s->ns[{f.index}], &sc, s->kb.p, s->kb.n, &v);",
                "    if (r < 0) {",
                "        s->st = r;",
                "        return dflt;",
                "    }",
                "    if (!v) return dflt;",
                f"    {ctype} out = dflt;",
                f"    int ok = {dec}(s->e->value_bytes(v), &out);",
                "    s->e->value_free(v);",
                "    if (!ok) {",
                "        s->st = dftu_fail(s->ctx, DFTU_E_INVALID,",
                f'                          "store {f.name}: a stored value is not {kind_name}");',
                "        return dflt;",
                "    }",
                "    return out;",
                "}",
                "",
            ]
        if f.name in puts:
            out += [
                f"static void _st_put_{f.name}(DftuStoreEnv* s, const dftu_plugin_host* host{params},",
                f"                         {ctype} value) {{",
                "    if (s->st < 0) return;",
                "    s->kb.n = 0;",
                "    s->vb.n = 0;",
                *_store_key_lines(f, "&s->kb"),
                f"    {enc}(&s->vb, value);",
                "    if (s->st < 0) return;",
                "    if (s->kb.bad || s->vb.bad) {",
                '        s->st = dftu_fail(s->ctx, DFTU_E_NO_MEMORY, "out of memory");',
                "        return;",
                "    }",
                f"    dftu_status r = s->e->put(s->ctx, s->batch[{f.index}], s->kb.p, s->kb.n, s->vb.p, s->vb.n);",
                "    if (r < 0) s->st = r;",
                "}",
                "",
            ]
    out += [
        "static dftu_status _st_open(DftuStoreEnv* s, dftu_ctx* ctx, const dftu_plugin_host* host) {",
        "    memset(s, 0, sizeof(*s));",
        "    s->ctx = ctx;",
        "    s->h = host->h;",
        "    s->e = (const dftu_svc_store*)host->get_service(host->h, DFTU_SVC_STORE);",
        '    if (!s->e) return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "the host has no store service");',
        "    dftu_store_scope sc = {DFTU_STORE_SCOPE_ROOT, NULL, 0};",
        "    for (int i = 0; i < DFTU_ST_COUNT; ++i) {",
        "        dftu_status r = s->e->ns(ctx, host->h, _ST_NAMES[i], 1, DFTU_STORE_FOLD_NONE, 1, 1, &s->ns[i]);",
        "        if (r < 0) return r;",
        "        if (_ST_PUT[i]) {",
        "            r = s->e->batch(ctx, host->h, s->ns[i], &sc, 0, NULL, 0, &s->batch[i]);",
        "            if (r < 0) return r;",
        "        }",
        "    }",
        "    if (_ST_HAS_GETS) {",
        "        dftu_status r = s->e->snapshot(ctx, host->h, &s->snap);",
        "        if (r < 0) return r;",
        "    }",
        "    return DFTU_OK;",
        "}",
        "",
        "static void _st_close(DftuStoreEnv* s, int commit) {",
        "    if (s->e) {",
        "        for (int i = 0; i < DFTU_ST_COUNT; ++i) {",
        "            if (s->batch[i]) {",
        "                if (commit) {",
        "                    dftu_status r = s->e->commit(s->ctx, s->h, s->batch[i]);",
        "                    if (r < 0 && s->st >= 0) {",
        "                        char msg[160];",
        '                        snprintf(msg, sizeof(msg), "store %s: the commit failed (status %d)", _ST_NAMES[i], (int)r);',
        "                        s->st = dftu_fail(s->ctx, r, msg);",
        "                    }",
        "                } else {",
        "                    s->e->batch_free(s->batch[i]);",
        "                }",
        "            }",
        "            if (s->ns[i]) s->e->ns_free(s->ns[i]);",
        "        }",
        "        if (s->snap) s->e->snapshot_free(s->snap);",
        "    }",
        "    free(s->kb.p);",
        "    free(s->vb.p);",
        "}",
        "",
    ]
    return out


def _emit(
    maps: Dict[str, _AnyMap],
    body: List[str],
    fields_used: builtins.set[str],
    plan_duql: str | None,
    str_literals: List[str],
    arg_keys: List[str],
    arg_helpers: builtins.set[str],
    rows_per_event: Dict[str, int],
    op_defs: List[str] | None = None,
    ports: Dict[str, _Port] | None = None,
    config_fields: "Dict[str, _CfgField] | None" = None,
    raw_event: bool = False,
    map_ids: "Dict[str, str] | None" = None,
    stores: "Dict[str, _StoreField] | None" = None,
    store_gets: "builtins.set[str] | None" = None,
    store_puts: "builtins.set[str] | None" = None,
    mask_guard: "str | None" = None,
    batch: "BatchHook | None" = None,
    can_raise: bool = False,
    flow_uses: "builtins.set[str] | None" = None,
    res_decls: "List[str] | None" = None,
    res_frees: "List[str] | None" = None,
    flow_defs: "List[str] | None" = None,
    finalize_lines: "List[str] | None" = None,
    chans: "Dict[str, _PortDecl] | None" = None,
    chan_uses: "builtins.set[str] | None" = None,
    plugin_name: str = "dftracer.jit",
) -> str:
    op_defs = op_defs or []
    stores = stores or {}
    ports = ports or {}
    chans = chans or {}
    chan_uses = chan_uses or builtins.set()
    flow_uses = flow_uses if flow_uses is not None else builtins.set()
    res_decls = res_decls or []
    res_frees = res_frees or []
    has_flows = finalize_lines is not None and flow_defs is not None
    needs_stats = (
        "stats" in flow_uses
        or has_flows
        or builtins.any(u in flow_uses for u in ("json", "yaml", "task"))
    )
    map_ids = map_ids if map_ids is not None else {attr: attr for attr in maps}
    pub_ports = [(n, p) for n, p in ports.items() if p.role == "publish"]
    sub_ports = [(n, p) for n, p in ports.items() if p.role == "consume"]
    needs_str = builtins.any(
        kt.dft == "DFTU_T_STR" for decl in maps.values() for kt in decl.key_types
    ) or builtins.any(m.elem == "str" for decl in maps.values() for m in decl.values)
    needs_bitmap = builtins.any(decl.is_product for decl in maps.values())
    plan_duql_field = "plan_duql" if plan_duql is not None else "NULL"
    out: List[str] = [
        "#include <dftracer/utils/plugins/abi.h>",
        "#include <dftracer/utils/plugins/prims.h>",
        "#include <dftracer/utils/dataframe/c_api.h>",
        "",
        "#include <cstring>",
        *(["#include <atomic>"] if needs_stats else []),
        "#include <stdint.h>",
        "#include <stdio.h>",
        "#include <stdlib.h>",
        "",
    ]
    if fields_used or arg_keys or raw_event:
        out += _COLBUF_HELPERS
    if raw_event:
        out += _RAW_EVENT_SHIM_TYPE
    for method in ("arg_i64", "arg_f64", "arg_str"):
        if method in arg_helpers:
            out += _ARG_HELPER_DEFS[method]
    if needs_str:
        out += _STR_COL_HELPER
    if needs_bitmap:
        out += _BITMAP_HELPER
    if can_raise or has_flows:
        out += _ERROR_HELPER
    if "cap" in flow_uses or has_flows:
        out += _flow.C_CAPTURE
    if needs_stats:
        out += _flow.C_STATS
    if builtins.any(u in flow_uses for u in ("json", "yaml", "task")) or has_flows:
        out += _flow.C_VALUE_HELPERS
    if _channels.channels_of(chans):
        out += _channels.C_PORT_HELPERS
    if stores:
        out += _emit_store_defs(stores, store_gets or builtins.set(), store_puts or builtins.set())
    for fn_src in op_defs:
        out += [fn_src, ""]
    cfgs = config_fields if config_fields is not None else {}
    str_cfgs = [f for f in cfgs.values() if f.kind == "str"]
    # Config and interned literals belong to one loaded instance (DftuInst) and
    # one worker slice (DftuSlice), never to file-scope statics: two instances of
    # one library keep their own values, and slices need no lock.
    if cfgs:
        out.append("struct DftuCfg {")
        out += [f"    {m}" for f in cfgs.values() for m in f.members()]
        out += [
            "};",
            "",
            "struct DftuInst {",
            "    dftu_plugin plugin;",
            "    DftuCfg cfg;",
            "};",
            "",
        ]
    has_slice = bool(str_literals or cfgs)
    if has_slice:
        out += [
            "struct DftuSlice {",
            *(["    const DftuCfg* cfg;"] if cfgs else []),
            "    int resolved;",
            f"    dftu_str lit_ids[{builtins.max(len(str_literals), 1)}];",
            f"    dftu_str cfg_ids[{builtins.max(len(str_cfgs), 1)}];",
            "};",
            "",
        ]
    # A plugin that reads config declares its keys, so the host rejects a
    # config key this plugin does not read instead of ignoring it. A plugin
    # with no config fields declares nothing and stays unvalidated.
    if cfgs:
        out.append("static const dftu_config_key g_config_keys[] = {")
        for f in cfgs.values():
            out.append(
                f"    {{{_c_str_literal(f.name)}, {f.value_kind()}, {int(f.required)}, NULL}},"
            )
        out += [
            "    {NULL, DFTU_CONFIG_ANY, 0, NULL},",
            "};",
            "",
            "static const dftu_config_key* config_keys(void* self) {",
            "    (void)self;",
            "    return g_config_keys;",
            "}",
            "",
        ]
    # Config reads for the factory: one block per field, defaults from the dataclass.
    cfg_reads = "\n".join(
        ["    (void)config;"]
        + (
            [
                "    DftuInst* inst = (DftuInst*)calloc(1, sizeof(DftuInst));",
                '    if (!inst) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
                "    DftuCfg* _c = &inst->cfg;",
            ]
            if cfgs
            else []
        )
        + [ln for f in cfgs.values() for ln in f.factory_reads()]
        + (
            ["    dftu_plugin& g_plugin = inst->plugin;", "    g_plugin.self = inst;"]
            if cfgs
            else ["    g_plugin.self = NULL;"]
        )
    )
    if plan_duql is not None:
        out += [
            "static const char* plan_duql(void* self) {",
            "    (void)self;",
            f"    return {_c_str_literal(plan_duql)};",
            "}",
            "",
        ]
    # The batch columns this plugin reads are exactly the ones on_batch
    # resolves, which the compiler already knows. Declaring them projects the
    # batch, so a trace with many arg keys does not build a Series per key for
    # a plugin that never looks at them. A raw body is not AST-analyzable, so
    # it declares nothing and keeps the whole batch.
    read_cols = [] if raw_event else sorted(fields_used) + [f"args.{a}" for a in sorted(arg_keys)]
    if batch is not None:
        read_cols = batch.columns
    if read_cols:
        out.append("static const char* const g_reads[] = {")
        for col in read_cols:
            out.append(f"    {_c_str_literal(col)},")
        out += [
            "    NULL,",
            "};",
            "",
            "static const char* const* reads(void* self) {",
            "    (void)self;",
            "    return g_reads;",
            "}",
            "",
        ]

    name_defs, name_assigns = _emit_name_lists(
        sorted(
            [_wire_id(p) for _, p in pub_ports]
            + [map_ids[attr] for attr in maps]
            + _channels.provided_names(chans)
        ),
        sorted([_wire_id(p) for _, p in sub_ports] + _channels.consumed_names(chans)),
    )
    out += name_defs
    if has_slice:
        out += [
            "static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {",
            "    (void)self;",
            "    DftuSlice* s = (DftuSlice*)calloc(1, sizeof(DftuSlice));",
            '    if (!s) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
        ]
        if cfgs:
            out.append("    s->cfg = &((DftuInst*)self)->cfg;")
        out += ["    *out = s;", "    return DFTU_OK;", "}", ""]
    else:
        out += [
            "static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {",
            "    (void)self;",
            "    *out = calloc(1, 1);",
            '    return *out ? DFTU_OK : dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
            "}",
            "",
        ]
    out += [
        "static dftu_status on_batch(dftu_ctx* ctx, void* slice, const dftu_dataframe* df,",
        "                            const dftu_plugin_host* host) {",
        "    (void)slice;",
        "    (void)ctx;",
        "    dftu_status _st = DFTU_OK;",
        "    const dftu_svc_agg* _agg =",
        "        (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);",
    ]
    if maps:
        out.append(
            "    if (!_agg || !_agg->agg_new || !_agg->agg_accumulate)"
            ' return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "the host has no aggregation service");'
        )
    else:
        out.append("    (void)_agg;")
    if stores:
        out += [
            "    DftuStoreEnv _se;",
            "    {",
            "        dftu_status _so = _st_open(&_se, ctx, host);",
            "        if (_so < 0) _st = _so;",
            "    }",
        ]
    out.append("    int64_t n = dftu_dataframe_num_rows(df);")
    if has_slice:
        out.append("    DftuSlice* _sl = (DftuSlice*)slice;")
        if cfgs:
            out.append("    const DftuCfg* _c = _sl->cfg;")
        out.append("    if (!_sl->resolved) {")
        for idx, lit in enumerate(str_literals):
            blen = len(lit.encode("utf-8"))
            out.append(
                f"        _sl->lit_ids[{idx}] = host->intern(host->h, {_c_str_literal(lit)}, {blen});"
            )
        for idx, f in enumerate(str_cfgs):
            out.append(
                f"        _sl->cfg_ids[{idx}] = host->intern(host->h, _c->v_{f.name}, _c->l_{f.name});"
            )
        out += ["        _sl->resolved = 1;", "    }"]
        for idx, _ in enumerate(str_literals):
            out.append(f"    const dftu_str lit_{idx} = _sl->lit_ids[{idx}];")
            out.append(f"    (void)lit_{idx};")
        for idx, f in enumerate(str_cfgs):
            out.append(f"    const dftu_str _cfgs_{f.name} = _sl->cfg_ids[{idx}];")
            out.append(f"    (void)_cfgs_{f.name};")
        for f in cfgs.values():
            out += f.locals_()
    for field in sorted(fields_used):
        out.append(
            f"    dftu_jit_col _col_{field} = dftu_jit_resolve(df, {_c_str_literal(field)});"
        )
    for idx, argname in enumerate(arg_keys):
        col_name = _c_str_literal(f"args.{argname}")
        out.append(f"    dftu_jit_col _argcol{idx} = dftu_jit_resolve(df, {col_name});")
    out.append("    int _alloc_ok = 1;")
    if can_raise:
        out += ["    int _unw = 0;", "    (void)_unw;"]
    if "cap" in flow_uses:
        out += ["    DftuCap _cap;", "    dftu_cap_init(&_cap, ctx);"]
    for svc, ctype, ident, word in (
        ("fileio", "dftu_svc_fileio", "DFTU_SVC_FILEIO", "file"),
        ("task", "dftu_svc_task", "DFTU_SVC_TASK", "task"),
        ("json", "dftu_svc_format", "DFTU_SVC_JSON", "JSON"),
        ("yaml", "dftu_svc_format", "DFTU_SVC_YAML", "YAML"),
        ("ports", "dftu_svc_ports", "DFTU_SVC_PORTS", "ports"),
    ):
        if svc in flow_uses:
            var = {
                "fileio": "_fio",
                "task": "_tsk",
                "json": "_fmtj",
                "yaml": "_fmty",
                "ports": "_prt",
            }[svc]
            out += [
                f"    const {ctype}* {var} = (const {ctype}*)host->get_service(host->h, {ident});",
                f'    if (!{var} && _st >= 0) _st = dftu_fail(ctx, DFTU_E_UNSUPPORTED, "the host has no {word} service");',
            ]
    out += ["    " + ln for ln in res_decls]
    for attr, decl in maps.items():
        out += _emit_buffers(attr, decl, builtins.max(1, rows_per_event.get(attr, 1)))
    out.append("    if (_alloc_ok && _st >= 0) {")
    inner: List[str] = []
    if ports:
        inner += _emit_port_pre(pub_ports, sub_ports)
    inner += _channels.event_open_lines(chans, chan_uses)
    if batch is not None:
        inner.append("    if (n > 0) {")
        inner += ["        " + ln for ln in batch.lines]
        inner.append("    }")
    else:
        if mask_guard is not None:
            inner += [
                "    uint32_t* _sel = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(n > 0 ? n : 1));",
                "    if (!_sel) _alloc_ok = 0;",
                "    int64_t _ns = 0;",
                "    for (int64_t i = 0; _sel && i < n; ++i) {",
                "        _sel[_ns] = (uint32_t)i;",
                f"        _ns += ({mask_guard}) ? 1 : 0;",
                "    }",
                "    for (int64_t _j = 0; _j < _ns; ++_j) {",
                "        const int64_t i = (int64_t)_sel[_j];",
                "        (void)i;",
            ]
        else:
            inner.append("    for (int64_t i = 0; i < n; ++i) {")
        if raw_event:
            inner += ["        " + ln for ln in _RAW_EVENT_SHIM_POPULATE]
        for line in body:
            inner.append("        " + line)
        if stores:
            inner.append("        if (_se.st < 0) break;")
        inner.append("    }")
        if can_raise:
            inner.append("_ev_exit: ;")
            inner += ["    " + ln for ln in res_frees]
        if mask_guard is not None:
            inner.append("    free(_sel);")
    tail: List[str] = []
    if ports:
        tail += _emit_port_flush(pub_ports)
    for attr, decl in maps.items():
        tail += _emit_flush(attr, decl, map_ids[attr])
    if can_raise and tail:
        inner += ["    if (_st >= 0) {"] + ["    " + ln for ln in tail] + ["    }"]
    else:
        inner += tail
    out += ["    " + ln for ln in inner]
    out.append("    }")
    if stores:
        out += [
            "    _st_close(&_se, _st >= 0 && _se.st >= 0);",
            "    if (_st >= 0 && _se.st < 0) _st = _se.st;",
        ]
    out.append('    if (!_alloc_ok) _st = dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");')
    for attr, decl in maps.items():
        out += _emit_frees(attr, decl)
    for field in sorted(fields_used):
        out.append(f"    dftu_series_free(_col_{field}.handle);")
    for idx in range(len(arg_keys)):
        out.append(f"    dftu_series_free(_argcol{idx}.handle);")
    out.extend(
        [
            "    return _st;",
            "}",
            "",
            "static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {",
            "    (void)ctx;",
            "    (void)into;",
            "    (void)other;",
            "    return DFTU_OK;",
            "}",
            "",
        ]
        + (
            (flow_defs or []) + (finalize_lines or [])
            if has_flows
            else _channels.close_without_finalize(chans)
        )
        + [
            "static void destroy_slice(void* slice) { free(slice); }",
            "",
        ]
        + (
            ["static void destroy(void* self) {", "    DftuInst* inst = (DftuInst*)self;"]
            + [f"    free(inst->cfg.v_{f.name});" for f in str_cfgs]
            + ["    free(inst);", "}", ""]
            if cfgs
            else [
                "static void destroy(void* self) { (void)self; }",
                "",
                "static dftu_plugin g_plugin;",
                "",
            ]
        )
        + [
            f"DFTU_PLUGIN_INFO({_c_str_literal(plugin_name)}, NULL)",
            "",
            "DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h,",
            "                                               const dftu_value* config,",
            "                                               dftu_plugin** out) {",
            "    (void)ctx;",
            "    (void)h;",
            *_channels.factory_lines(chans),
            cfg_reads,
            "    g_plugin.struct_size = sizeof(g_plugin);",
            f"    g_plugin.plan_duql = {plan_duql_field};",
            "    g_plugin.make_slice = make_slice;",
            "    g_plugin.on_batch = on_batch;",
            "    g_plugin.merge = merge;",
            "    g_plugin.on_finalize = on_finalize;",
            "    g_plugin.destroy_slice = destroy_slice;",
            "    g_plugin.destroy = destroy;",
        ]
        + (["    g_plugin.config_keys = config_keys;"] if cfgs else [])
        + (["    g_plugin.reads = reads;"] if read_cols else [])
        + name_assigns
    )
    out.extend(
        [
            "    *out = &g_plugin;",
            "    return DFTU_OK;",
            "}",
            "",
        ]
    )
    return "\n".join(out)


# Row budget per event per accumulator for a raw each_event body; the emitted
# buffers hold this many rows per scanned event and a raw body must not append
# more (there is no AST to count its contributions from).
_RAW_ROWS_PER_EVENT = 4


def _raw_body(fn: Callable[..., object]) -> List[str]:
    argc = getattr(fn, "__code__").co_argcount
    result = fn(*([None] * argc))
    if not isinstance(result, str):
        source = _Source(fn)
        raise source.error(
            "@jit.each_event(raw=True) method must return a C++ body string",
            ast.parse(source.text).body[0],
        )
    return textwrap.dedent(result).splitlines()


def _referenced_ops(fn: Callable[..., object]) -> Dict[str, Op]:
    """The ``@jit.op`` transforms the each_event body can reach by name, from its
    globals and closure - so calling one inlines that op into the plugin."""
    try:
        cv = inspect.getclosurevars(fn)
    except TypeError:
        return {}
    scope = {**cv.globals, **cv.nonlocals}
    return {name: val for name, val in scope.items() if isinstance(val, Op)}


# ---- dataclass config ------------------------------------------------------

_CFG_KINDS: Dict[type, str] = {bool: "bool", int: "int", float: "float", str: "str"}
_CFG_NATIVE_KIND = {
    "bool": "DFTU_VAL_BOOL",
    "int": "DFTU_VAL_I64",
    "float": "DFTU_VAL_F64",
    "str": "DFTU_VAL_STR",
}


class _CfgField:
    """One field of a plugin's config dataclass: a value of ``kind`` (int,
    float, bool or str) that is optionally None, with the dataclass default."""

    __slots__ = ("name", "kind", "optional", "default", "required")

    def __init__(
        self,
        name: str,
        kind: str,
        optional: bool,
        default: "Union[int, float, bool, str, None]",
        required: bool,
    ) -> None:
        self.name = name
        self.kind = kind
        self.optional = optional
        self.default = default
        self.required = required

    def value_kind(self) -> str:
        # An optional field may be sent as null, so only the key is declared.
        return "DFTU_CONFIG_ANY" if self.optional else _CFG_NATIVE_KIND[self.kind]

    def members(self) -> List[str]:
        """The DftuCfg members of one loaded instance. A prefix per member kind
        keeps every name apart: ``v_`` value, ``l_`` string length, ``h_`` set."""
        n = self.name
        if self.kind == "str":
            out = [f"char* v_{n};", f"uint32_t l_{n};"]
        elif self.kind == "float":
            out = [f"double v_{n};"]
        else:
            out = [f"int64_t v_{n};"]
        if self.optional:
            out.append(f"int h_{n};")
        return out

    def locals_(self) -> List[str]:
        """The on_batch locals the body reads (``_cfg_``, ``_has_``); a string's
        interned id is ``_cfgs_`` and is declared where it is resolved."""
        n = self.name
        out: List[str] = []
        if self.kind != "str":
            ctype = "double" if self.kind == "float" else "int64_t"
            out += [f"    const {ctype} _cfg_{n} = _c->v_{n};", f"    (void)_cfg_{n};"]
        if self.optional:
            out += [f"    const int _has_{n} = _c->h_{n};", f"    (void)_has_{n};"]
        return out

    def _c_default(self) -> str:
        d = self.default
        if d is None:
            return "0.0" if self.kind == "float" else "0"
        if self.kind == "float":
            return repr(float(d))
        if self.kind == "bool":
            return "1" if d else "0"
        return _int_literal(int(d))

    def factory_reads(self) -> List[str]:
        """The factory statements that load the field into the instance's
        DftuCfg, falling back to the dataclass default when the key is absent
        and to unset when it is null."""
        n = self.name
        key = _c_str_literal(n)
        out = ["    {", f"        const dftu_value* v = dftu_obj_get(config, {key});"]
        if self.optional:
            out += [
                "        int is_null = v != NULL && v->kind == DFTU_VAL_NULL;",
                f"        _c->h_{n} = is_null ? 0 : (v != NULL ? 1 : {0 if self.default is None else 1});",
            ]
            # An optional key is declared with any kind so it may be null, which
            # leaves the host no type check to make; make it here.
            if self.kind == "str":
                ok = "v->kind == DFTU_VAL_STR"
            else:
                ok = (
                    "v->kind == DFTU_VAL_BOOL || v->kind == DFTU_VAL_I64 || v->kind == DFTU_VAL_F64"
                )
            out += [
                f"        if (v != NULL && !is_null && !({ok})) {{",
                "            destroy(inst);",
                "            return dftu_fail(ctx, DFTU_E_INVALID,",
                f"                             {_c_str_literal(f'config key {n!r} has the wrong type')});",
                "        }",
            ]
        else:
            out.append("        int is_null = 0;")
        if self.kind == "str":
            default = "" if self.default is None else str(self.default)
            blen = len(default.encode("utf-8"))
            out += [
                "        uint32_t len = 0;",
                "        const char* text = is_null ? NULL : dftu_as_str(v, &len);",
                f"        if (!text && !is_null) {{ text = {_c_str_literal(default)}; len = {blen}; }}",
                "        char* copy = (char*)malloc((size_t)len + 1);",
                "        if (!copy) {",
                "            destroy(inst);",
                '            return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
                "        }",
                "        if (len) memcpy(copy, text, len);",
                "        copy[len] = 0;",
                f"        _c->v_{n} = copy;",
                f"        _c->l_{n} = len;",
            ]
        else:
            fn = {"float": "dftu_as_f64", "bool": "dftu_as_bool"}.get(self.kind, "dftu_as_i64")
            out.append(f"        _c->v_{n} = is_null ? 0 : {fn}(v, {self._c_default()});")
        out.append("    }")
        return out


def _cfg_kind(tp: object) -> "Tuple[str, bool] | None":
    """(kind, is_optional) for a supported config field type, else None."""
    if isinstance(tp, type) and tp in _CFG_KINDS:
        return _CFG_KINDS[tp], False
    union_type = getattr(types, "UnionType", None)
    origin = getattr(tp, "__origin__", None)
    if origin is Union or (union_type is not None and isinstance(tp, union_type)):
        args = [a for a in getattr(tp, "__args__", ()) if a is not type(None)]
        if len(args) == 1 and isinstance(args[0], type) and args[0] in _CFG_KINDS:
            return _CFG_KINDS[args[0]], True
    return None


def _type_hints(obj: type) -> Dict[str, object]:
    try:
        return get_type_hints(obj)
    except (NameError, TypeError):
        return dict(getattr(obj, "__dict__", {}).get("__annotations__", {}))


def _config_fields(cls: type) -> Dict[str, _CfgField]:
    """The fields of the dataclass ``cls`` declares as ``config: Cfg``; empty
    when it declares none. Raises :class:`JitError` at the offending annotation
    for a config type that is not a dataclass or a field type that has no
    native form."""
    if "config" not in cls.__dict__.get("__annotations__", {}):
        return {}
    cfg_type = _type_hints(cls).get("config")
    if not (isinstance(cfg_type, type) and dataclasses.is_dataclass(cfg_type)):
        raise _class_error(
            cls,
            f"@jit.plugin config must be annotated with a dataclass, got {cfg_type!r}",
            "config",
        )
    hints = _type_hints(cfg_type)
    fields: Dict[str, _CfgField] = {}
    for f in dataclasses.fields(cfg_type):
        if not f.name.isascii():
            raise _class_error(cfg_type, f"config field '{f.name}' must have an ASCII name", f.name)
        tp = hints.get(f.name, f.type)
        kind = _cfg_kind(tp)
        if kind is None:
            raise _class_error(
                cfg_type,
                f"config field '{f.name}' has type {tp!r}; a config field is an int, float, "
                "bool or str, or Optional of one",
                f.name,
            )
        if f.default_factory is not dataclasses.MISSING:
            raise _class_error(
                cfg_type,
                f"config field '{f.name}' uses default_factory; give it a plain default",
                f.name,
            )
        has_default = f.default is not dataclasses.MISSING
        if (
            has_default
            and not (f.default is None and kind[1])
            and not _cfg_value_ok(kind[0], f.default)
        ):
            raise _class_error(
                cfg_type,
                f"config field '{f.name}' has default {f.default!r}, not a {kind[0]}",
                f.name,
            )
        default = cast("Union[int, float, bool, str, None]", f.default) if has_default else None
        if kind[0] == "float" and isinstance(default, float) and not math.isfinite(default):
            raise _class_error(cfg_type, f"config field '{f.name}' default must be finite", f.name)
        fields[f.name] = _CfgField(f.name, kind[0], kind[1], default, not has_default)
    return fields


def _cfg_value_ok(kind: str, value: object) -> bool:
    if kind == "bool":
        return isinstance(value, bool)
    if kind == "str":
        return isinstance(value, str)
    if isinstance(value, bool):
        return False
    return isinstance(value, (int, float)) if kind == "float" else isinstance(value, int)


def _check_cfg_value(
    plugin: str, field: _CfgField, value: "JSONValue"
) -> "Union[int, float, bool, str, None]":
    if value is None and field.optional:
        return None
    if not _cfg_value_ok(field.kind, value):
        expected = f"Optional[{field.kind}]" if field.optional else field.kind
        raise JitError(
            f"{plugin} config field '{field.name}' expects {expected}, got "
            f"{type(value).__name__} ({value!r})"
        )
    if field.kind == "float":
        return float(cast(float, value))
    return cast("Union[int, bool, str]", value)


def plugin_config(
    cls: type, config: "Optional[Mapping[str, JSONValue]]"
) -> "Optional[Dict[str, JSONValue]]":
    """The load config for ``cls``, checked against its config dataclass.

    A plugin without a ``config: Cfg`` annotation gets ``config`` back as is.
    Otherwise every key must be a field, a required field must be present, a
    value must have its field's type (an int is accepted for a float), and the
    dataclass default fills an absent field; an optional field that is None is
    sent as null, which is how a caller turns an Optional field off even when its
    default is a value. Raises :class:`JitError` naming the field."""
    fields = getattr(cls, "_jit_config", None)
    if fields is None:
        return dict(config) if config is not None else None
    given = dict(config) if config is not None else {}
    name = cls.__name__
    for key in given:
        if key not in fields:
            raise JitError(
                f"{name} has no config field '{key}'; its fields are {', '.join(fields) or 'none'}"
            )
    out: Dict[str, JSONValue] = {}
    for fname, field in fields.items():
        if fname in given:
            value = given[fname]
        elif field.required:
            raise JitError(f"{name} config field '{fname}' is required and was not given")
        else:
            value = field.default
        out[fname] = _check_cfg_value(name, field, value)
    return out


def _store_plugin_name(identity: str) -> str:
    """The plugin name a store-using plugin loads under, which is the stem of its
    ``.so`` and the host's namespace prefix: its identity with every byte outside
    ``[a-z0-9-]`` escaped (``_`` as ``__``, ``.`` as ``_d``, ``/`` as ``_s``, any other
    as ``_u<hex>_``), so two identities never share a name."""
    out: List[str] = []
    for c in identity:
        if c in _STORE_NAME_CHARS and c != "_":
            out.append(c)
        elif c == "_":
            out.append("__")
        elif c == ".":
            out.append("_d")
        elif c == "/":
            out.append("_s")
        else:
            out.append(f"_u{ord(c):x}_")
    return "".join(out)


_STORE_NAME_CHARS = frozenset("abcdefghijklmnopqrstuvwxyz0123456789_-")


_PORT_ANNOTATION = {
    "Sender": ("channel", ("send",)),
    "Receiver": ("channel", ("recv",)),
    "Channel": ("channel", ("both",)),
    "Latest": ("latest", ("own", "ref")),
    "Phase": ("phase", ("own", "ref")),
}
_PORT_ORIGINS = {
    Sender: "Sender",
    Receiver: "Receiver",
    Channel: "Channel",
    Latest: "Latest",
    Phase: "Phase",
}
_PORT_FACTORY = {
    ("channel", "send"): "jit.sender(...)",
    ("channel", "recv"): "jit.receiver(...)",
    ("channel", "both"): "jit.channel(...)",
    ("latest", "own"): "jit.latest()",
    ("latest", "ref"): "jit.latest(...)",
    ("phase", "own"): "jit.phase(...)",
    ("phase", "ref"): "jit.phase(...)",
}


def _port_decls(
    cls: type, pkg: "JitPackage", declared: "Dict[str, _PortDecl]"
) -> "Dict[str, _PortDecl]":
    """Complete the port attributes of ``cls``: the element type from the
    annotation, the host name from the plugin identity, and, for a reference, the
    options and type of the declaration it names (checked against this end)."""
    if not declared:
        return declared
    own = _own_annotations(cls)
    plugin_name = pkg.identity(cls)
    own_full = {
        f"plugin.{plugin_name}.{a}"
        for a, d in declared.items()
        if d.kind == "channel" and d.role in ("send", "both")
    }
    for attr, decl in declared.items():
        try:
            _channels.check_logical(attr)
        except JitError as exc:
            raise _class_error(cls, exc.reason, attr) from None
        if attr not in own:
            raise _class_error(
                cls,
                f"the port '{attr}' needs an annotation with its element type, "
                f"such as `{attr}: jit.{'Sender' if decl.role == 'send' else 'Receiver'}[int]`",
                attr,
            )
        tp = _resolve_hint(cls, attr, own[attr])
        name = _PORT_ORIGINS.get(get_origin(tp))  # type: ignore[arg-type]
        want = _PORT_FACTORY[(decl.kind, decl.role)]
        args = get_args(tp)
        if name is None or len(args) != 1:
            raise _class_error(
                cls,
                f"the port '{attr}' is declared with {want}; annotate it "
                "jit.Sender[int], jit.Receiver[float], jit.Channel[int], jit.Latest[int] or "
                "jit.Phase[float]",
                attr,
            )
        kind, roles = _PORT_ANNOTATION[name]
        if kind != decl.kind or decl.role not in roles:
            raise _class_error(
                cls,
                f"the port '{attr}' is annotated jit.{name}[...] but declared with {want}",
                attr,
            )
        elem = _channels.elem_of(args[0])
        if elem is None:
            raise _class_error(
                cls,
                f"the port '{attr}' carries {getattr(args[0], '__name__', args[0])!s}; a port "
                "carries an int or a float (a str is a view or an interned id that is valid "
                "only in the body that read it, so it cannot cross a channel)",
                attr,
            )
        decl.attr = attr
        decl.elem = elem
        decl.owner = True
        decl.logical = attr
        decl.full = f"plugin.{plugin_name}.{attr}"
        src = decl.source
        if isinstance(src, str):
            if decl.kind == "channel" and src in own_full:
                raise _class_error(
                    cls,
                    f"the port '{attr}' receives {src}, a channel of this plugin: its sends "
                    "close only when its finalize ends, so a receive in it would wait for "
                    "ever; use jit.channel for both ends in one plugin",
                    attr,
                )
            decl.owner = False
            decl.logical = decl.full = src
        elif src is not None:
            if not src.full:
                raise _class_error(
                    cls,
                    f"the port '{attr}' refers to a port of a plugin that is not built yet; "
                    "declare and decorate that class first",
                    attr,
                )
            if src.elem != elem:
                raise _class_error(
                    cls,
                    f"the port '{attr}' carries {args[0].__name__} but {src.full} carries "
                    f"{'int' if src.elem == 'i64' else 'float'}",
                    attr,
                )
            if src.policy == "block":
                raise _class_error(
                    cls,
                    f"the port '{attr}' refers to the Block channel {src.full}, which stays in "
                    "the plugin that owns it (a Block send to another plugin would wait for "
                    "ever once it passed the credits); use policy 'fail' or 'drop_oldest'",
                    attr,
                )
            decl.owner = False
            decl.logical = decl.full = src.full
            decl.credits, decl.policy = src.credits, src.policy
            decl.max_msg_bytes, decl.merge = src.max_msg_bytes, src.merge
    seen: Dict[str, str] = {}
    for attr, decl in declared.items():
        if decl.logical in seen:
            raise _class_error(
                cls, f"the ports '{seen[decl.logical]}' and '{attr}' name one port", attr
            )
        seen[decl.logical] = attr
    return declared


def _build_plugin(cls: type, package: "JitPackage | None" = None) -> type:
    pkg = package if package is not None else _DEFAULT_PACKAGE
    maps: Dict[str, _AnyMap] = {}
    ports: Dict[str, _Port] = {}
    chans: Dict[str, _PortDecl] = {}
    each: List[_EachEvent] = []
    batches: List[_EachBatch] = []
    task_fns: Dict[str, Callable[..., object]] = {}
    finalize_fns: List[Callable[..., object]] = []
    for attr, val in vars(cls).items():
        if isinstance(val, _MapDecl):
            maps[attr] = val
        elif isinstance(val, _Port):
            ports[attr] = val
        elif isinstance(val, _PortDecl):
            chans[attr] = val
        elif isinstance(val, _EachEvent):
            each.append(val)
        elif isinstance(val, _EachBatch):
            batches.append(val)
        elif callable(val) and getattr(val, _flow.TASK_ATTR, False):
            task_fns[attr] = val
        elif callable(val) and getattr(val, _flow.FINALIZE_ATTR, False):
            finalize_fns.append(val)
    plains, stores = _state_fields(cls)
    if not maps and not ports and not plains and not stores and not chans:
        raise _class_error(
            cls,
            "@jit.plugin needs at least one jit.map, plain-container state field, jit.Store, "
            "jit.publish/jit.consume attribute or port (jit.sender, jit.receiver, jit.channel, "
            "jit.latest, jit.phase)",
        )
    if batches:
        if each:
            raise _class_error(
                cls, "@jit.plugin takes one @jit.each_event or one @jit.each_batch, not both"
            )
        if task_fns or finalize_fns:
            raise _class_error(
                cls,
                "@jit.task and @jit.finalize need an @jit.each_event plugin, not @jit.each_batch",
            )
        return _build_batch_plugin(
            cls, pkg, maps, ports, plains, stores, batches, _port_decls(cls, pkg, chans)
        )
    if len(each) != 1:
        raise _class_error(
            cls, "@jit.plugin needs exactly one @jit.each_event or @jit.each_batch method"
        )
    if (plains or stores) and each[0].raw:
        raise _class_error(
            cls, "plain-container state and jit.Store need an AST-compiled each_event body, not raw"
        )
    if (task_fns or finalize_fns) and each[0].raw:
        raise _class_error(cls, "@jit.task and @jit.finalize need an AST-compiled each_event body")
    if chans and each[0].raw:
        raise _class_error(
            cls, "channels, latest values and phase results need an AST-compiled each_event body"
        )
    chans = _port_decls(cls, pkg, chans)
    if len(finalize_fns) > 1:
        raise _class_error(cls, "@jit.plugin takes one @jit.finalize method")
    if task_fns and not finalize_fns:
        raise _class_error(
            cls, "a @jit.task is started from the @jit.finalize body; this plugin has none"
        )
    # A publish port's wire id is only knowable once its class is built: it is
    # package + module + class + attribute, resolved here so a later
    # consume(ThisClass.port) sees a real name.
    for attr, port in ports.items():
        if port.role == "publish":
            port.name = pkg.identity(cls, attr)
    for attr, decl in maps.items():
        if decl.is_product and builtins.any(m.elem == "str" for m in decl.values):
            raise _class_error(
                cls,
                f"@jit.plugin map '{attr}': a string-valued reduction "
                "(jit.set/list/argmin/argmax/topk/sample with of=jit.str_) cannot be a "
                "product component; declare it as its own jit.map",
                attr,
            )
    plan_duql = getattr(cls, "plan_duql", None)
    if plan_duql is not None and not isinstance(plan_duql, str):
        raise _class_error(cls, "@jit.plugin plan_duql must be a query DSL string", "plan_duql")
    configs = _config_fields(cls)
    op_defs: List[str] = []
    task_sigs: Dict[str, _TaskSig] = {}
    if each[0].raw:
        body = _raw_body(each[0].fn)
        # A raw body is spliced verbatim, so which fields it reads off `e` is
        # not knowable; resolve all of them.
        fields_used: builtins.set[str] = builtins.set(_ALL_FIXED_FIELDS)
        str_literals: List[str] = []
        arg_keys: List[str] = []
        arg_helpers: builtins.set[str] = builtins.set()
        # A raw body appends rows itself, so the row budget cannot be counted
        # from the AST; RAW_ROWS_PER_EVENT is the contract it must respect.
        rows_per_event = {attr: _RAW_ROWS_PER_EVENT for attr in maps}
    else:
        compiler = _Compiler(
            maps,
            _referenced_ops(each[0].fn),
            ports=ports,
            config_fields=configs,
            plain=plains,
            stores=stores,
            chans=chans,
        )
        for attr, fn in task_fns.items():
            src = _Source(fn)
            try:
                task_sigs[attr] = _flow.task_signature(fn, attr, src.text)
            except JitError as exc:
                raise src.relocate(exc, None) from None
        compiler.tasks = task_sigs
        body = compiler.lower(each[0].fn)
        for attr in plains:
            if attr not in compiler.plain_forms:
                raise _class_error(
                    cls,
                    f"'{attr}' is never updated in the each_event body, so its merge "
                    "cannot be inferred; remove it or update it",
                    attr,
                )
        fields_used = compiler.fields_used
        str_literals = compiler.str_literals
        arg_keys = compiler.arg_keys
        arg_helpers = compiler.arg_helpers
        op_defs = compiler.op_defs
        rows_per_event = compiler.rows_per_event
    flow_fns: "List[_FlowFn]" = []
    flow_defs: "List[str] | None" = None
    finalize_lines: "List[str] | None" = None
    flow_uses: "builtins.set[str]" = (
        builtins.set(compiler.uses) if not each[0].raw else builtins.set()
    )
    extra_gets: "builtins.set[str]" = builtins.set()
    if finalize_fns:
        sigs = task_sigs
        for fn, sig in [(finalize_fns[0], None)] + [(task_fns[a], sigs[a]) for a in task_fns]:
            flow_compiler = _Compiler(
                maps,
                _referenced_ops(fn),
                ports={},
                config_fields=configs,
                plain={},
                stores=stores,
                chans=chans,
            )
            flow_compiler.tasks = sigs
            flow = flow_compiler.lower_flow(fn, sig)
            flow_fns.append(flow)
            for def_text in flow.op_defs:
                if def_text not in op_defs:
                    op_defs.append(def_text)
            extra_gets |= flow.store_gets
            flow_uses |= flow.uses
        cfg_pre: List[str] = ["    const DftuCfg* _c = R->cfg;", "    (void)_c;"] if configs else []
        for cf in configs.values():
            cfg_pre += cf.locals_()
            if cf.kind == "str":
                cfg_pre += [
                    f"    const dftu_str _cfgs_{cf.name} = host->intern(host->h, _c->v_{cf.name}, "
                    f"_c->l_{cf.name});",
                    f"    (void)_cfgs_{cf.name};",
                ]
        flow_defs, finalize_lines, fu = _flow.emit_flows(
            flow_fns,
            has_cfg=bool(configs),
            cfg_pre=cfg_pre,
            has_stores=bool(stores),
            chans=chans,
        )
        flow_uses |= fu
    map_ids = {attr: pkg.identity(cls, attr) for attr in maps}
    source = _emit(
        maps,
        body,
        fields_used,
        plan_duql,
        str_literals,
        arg_keys,
        arg_helpers,
        rows_per_event,
        op_defs,
        ports,
        configs,
        raw_event=each[0].raw,
        map_ids=map_ids,
        stores=stores,
        store_gets=(compiler.store_gets | extra_gets) if not each[0].raw else None,
        store_puts=compiler.store_puts if not each[0].raw else None,
        mask_guard=compiler.mask_guard if not each[0].raw else None,
        can_raise=compiler.can_raise if not each[0].raw else False,
        flow_uses=flow_uses,
        res_decls=compiler.res_decls if not each[0].raw else None,
        res_frees=compiler.res_frees if not each[0].raw else None,
        flow_defs=flow_defs,
        finalize_lines=finalize_lines,
        chans=chans,
        chan_uses=compiler.chan_uses if not each[0].raw else None,
        plugin_name=pkg.identity(cls) if chans else "dftracer.jit",
    )
    result_names = {qid: attr for attr, qid in map_ids.items()}
    name = _store_plugin_name(pkg.identity(cls)) if stores else cls.__name__
    if each[0].raw:
        lowering = "raw"
    elif compiler.mask_guard is not None:
        lowering = "mask"
    else:
        lowering = "branch"
    plugin = JitPlugin(name, source, {}, result_names, pkg.shippable(cls), lowering)
    setattr(cls, "_jit_plugin", plugin)
    if configs:
        setattr(cls, "_jit_config", configs)
    if chans:
        setattr(cls, "_jit_ports", chans)
    return cls


def _build_batch_plugin(
    cls: type,
    pkg: "JitPackage",
    maps: Dict[str, _AnyMap],
    ports: Dict[str, _Port],
    plains: Dict[str, _PlainField],
    stores: Dict[str, _StoreField],
    batches: "List[_EachBatch]",
    chans: "Dict[str, _PortDecl]",
) -> type:
    from ._batch import HELPERS, lower_batch

    if len(batches) != 1:
        raise _class_error(cls, "@jit.plugin needs exactly one @jit.each_batch method")
    if ports or stores:
        raise _class_error(
            cls,
            "an @jit.each_batch plugin folds into scalar state fields and key-less jit.map "
            "descriptors; publish/consume ports and jit.Store need @jit.each_event",
        )
    plan_duql = getattr(cls, "plan_duql", None)
    if plan_duql is not None and not isinstance(plan_duql, str):
        raise _class_error(cls, "@jit.plugin plan_duql must be a query DSL string", "plan_duql")
    if _config_fields(cls):
        raise _class_error(cls, "an @jit.each_batch plugin takes no config; use @jit.each_event")
    hook = lower_batch(
        batches[0].fn,
        cast("Dict[str, object]", maps),
        cast("Dict[str, object]", plains),
        cast("Dict[str, object]", chans),
    )
    for attr in plains:
        if attr not in hook.updated:
            raise _class_error(
                cls,
                f"'{attr}' is never updated in the each_batch body, so its merge cannot be "
                "inferred; remove it or update it",
                attr,
            )
        field = plains[attr]
        monoid: _Monoid = (
            Counter("DFTU_AGG_SUM", elem="i64")
            if field.elem == "i64"
            else Sum("DFTU_AGG_SUM", elem="f64")
        )
        maps[attr] = _MapDecl((), (monoid,), False)
    map_ids = {attr: pkg.identity(cls, attr) for attr in maps}
    source = _emit(
        maps,
        [],
        builtins.set(),
        plan_duql,
        [],
        [],
        builtins.set(),
        hook.rows,
        HELPERS,
        {},
        {},
        map_ids=map_ids,
        batch=hook,
        flow_uses=builtins.set(["ports"]) if chans else None,
        chans=chans,
        chan_uses=hook.chan_uses,
        plugin_name=pkg.identity(cls) if chans else "dftracer.jit",
    )
    result_names = {qid: attr for attr, qid in map_ids.items()}
    plugin = JitPlugin(cls.__name__, source, {}, result_names, pkg.shippable(cls), "batch")
    setattr(cls, "_jit_plugin", plugin)
    if chans:
        setattr(cls, "_jit_ports", chans)
    return cls


# ---- vfold: a per-batch fold on columns -----------------------------------

# Reducer sugar accepted on a scalar accumulator; each must match the
# accumulator's own reduction.
_VFOLD_REDUCERS = {
    "sum": "DFTU_AGG_SUM",
    "min": "DFTU_AGG_MIN",
    "max": "DFTU_AGG_MAX",
}
_VFOLD_TOP_FIELDS = frozenset({"name", "cat", "pid", "tid", "ts", "dur", "ph", "fhash", "hhash"})
# Numeric top-level columns; an ordering (by) column must be one of these.
_VFOLD_NUMERIC = frozenset({"pid", "tid", "ts", "dur", "ph"})
_VFOLD_STRING_FIELDS = frozenset({"name", "cat", "fhash", "hhash"})


def _vfold_df_field(node: ast.expr) -> str:
    if not (
        isinstance(node, ast.Subscript)
        and isinstance(node.value, ast.Name)
        and node.value.id == "df"
    ):
        raise JitError('@jit.vfold body must be self.<acc> += df["<field>"].<reducer>()')
    key = _unwrap_index(node.slice)
    if not (isinstance(key, ast.Constant) and isinstance(key.value, str)):
        raise JitError('@jit.vfold: df subscript must be a string field name, df["dur"]')
    return key.value


def _vfold_specs(opd: Dict[str, object]) -> List[Tuple[str, str, str, str, str]]:
    """The (op, value, out, param, by) dftu_agg_col rows for one vfold op."""
    mon = cast(_Monoid, opd["mon"])
    vfield = opd["vfield"]
    byfield = opd["byfield"]
    value = _c_str_literal(cast(str, vfield)) if vfield is not None else "NULL"
    by = _c_str_literal(cast(str, byfield)) if byfield is not None else "NULL"
    if isinstance(mon, Quantiles):
        specs = [("DFTU_AGG_COUNT", "NULL", _c_str_literal("count"), "0.0", "NULL")]
        for q in mon.qs:
            specs.append(
                ("DFTU_AGG_PCT", value, _c_str_literal(_quantile_name(q)), repr(q), "NULL")
            )
        return specs
    if vfield is None:
        return [("DFTU_AGG_COUNT", "NULL", _c_str_literal("value"), "0.0", "NULL")]
    return [(mon.dft, value, _c_str_literal("value"), repr(mon.param), by)]


def _compile_vfold(
    fn: Callable[..., object],
    accums: Dict[str, _Monoid],
    maps: Dict[str, _AnyMap],
) -> Tuple[List[Dict[str, object]], "builtins.set[str]"]:
    source = _Source(fn)
    try:
        return _compile_vfold_body(source, accums, maps)
    except JitError as exc:
        raise source.relocate(exc, source.cur) from None


def _compile_vfold_body(
    source: _Source,
    accums: Dict[str, _Monoid],
    maps: Dict[str, _AnyMap],
) -> Tuple[List[Dict[str, object]], "builtins.set[str]"]:
    tree = ast.parse(source.text)
    func = tree.body[0]
    source.cur = func
    if not isinstance(func, ast.FunctionDef):
        raise JitError("@jit.each_batch must decorate a function")
    ops: List[Dict[str, object]] = []
    fields: "builtins.set[str]" = builtins.set()
    shape = (
        '@jit.vfold body must be self.<acc> += df["f"].<reducer>() '
        'or self.<map>[df["k"]] += df["v"]'
    )
    for stmt in func.body:
        source.cur = stmt
        if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Constant):
            continue  # docstring
        if not (isinstance(stmt, ast.AugAssign) and isinstance(stmt.op, ast.Add)):
            raise JitError(shape)
        tgt = stmt.target
        rhs = stmt.value
        keyfields: List[str] = []
        if (
            isinstance(tgt, ast.Subscript)
            and isinstance(tgt.value, ast.Attribute)
            and isinstance(tgt.value.value, ast.Name)
            and tgt.value.value.id == "self"
        ):
            attr = tgt.value.attr
            if attr not in maps:
                raise JitError(f"@jit.vfold: '{attr}' is not a declared jit.map")
            decl = maps[attr]
            sl = _unwrap_index(tgt.slice)
            elts = builtins.list(sl.elts) if isinstance(sl, ast.Tuple) else [sl]
            keyfields = [_vfold_df_field(k) for k in elts]
            if len(keyfields) != len(decl.key_types):
                raise JitError(
                    f"@jit.vfold: '{attr}' takes {len(decl.key_types)} key columns, "
                    f"got {len(keyfields)}"
                )
            for kt, kf in zip(decl.key_types, keyfields):
                if kf not in _VFOLD_TOP_FIELDS:
                    raise JitError(f"@jit.vfold: key column '{kf}' is not a scan column")
                if (kf in _VFOLD_STRING_FIELDS) != (kt.dft == "DFTU_T_STR"):
                    raise JitError(
                        f"@jit.vfold: key column '{kf}' does not match the declared key type; "
                        "a string column needs jit.str_, a numeric one an integer type"
                    )
            mon = decl.values[0]
        elif (
            isinstance(tgt, ast.Attribute)
            and isinstance(tgt.value, ast.Name)
            and tgt.value.id == "self"
        ):
            attr = tgt.attr
            if attr not in accums:
                raise JitError(f"@jit.vfold: '{attr}' is not a declared accumulator")
            mon = accums[attr]
        else:
            raise JitError(shape)
        vfield: str | None = None
        byfield: str | None = None
        if isinstance(rhs, ast.Call):
            # Scalar sugar: df["f"].sum() / .min() / .max().
            if not (not rhs.args and isinstance(rhs.func, ast.Attribute)):
                raise JitError('@jit.vfold: scalar value must be df["<field>"].<reducer>()')
            reducer = rhs.func.attr
            if reducer not in _VFOLD_REDUCERS:
                raise JitError(f"@jit.vfold: unsupported reducer '.{reducer}()' (use sum/min/max)")
            if _VFOLD_REDUCERS[reducer] != mon.dft:
                raise JitError(
                    f"@jit.vfold: '.{reducer}()' does not match accumulator '{attr}' "
                    f"(a jit.{reducer}() accumulator)"
                )
            vfield = _vfold_df_field(rhs.func.value)
        elif mon.needs_by:
            if not (isinstance(rhs, ast.Tuple) and len(rhs.elts) == 2):
                raise JitError(
                    f"@jit.vfold: '{attr}' needs a (value, by) column pair, "
                    'e.g. self.m[df["pid"]] += df["ts"], df["dur"]'
                )
            vfield = _vfold_df_field(rhs.elts[0])
            byfield = _vfold_df_field(rhs.elts[1])
            if byfield not in _VFOLD_NUMERIC:
                raise JitError(
                    f"@jit.vfold: the by column '{byfield}' must be numeric (pid/tid/ts/dur)"
                )
        elif isinstance(rhs, ast.Constant) and isinstance(rhs.value, int):
            if not isinstance(mon, Counter) or rhs.value != 1:
                raise JitError(
                    f"@jit.vfold: '{attr}' reduces a value column; only a jit.count() "
                    "accumulator takes the constant 1"
                )
        elif isinstance(rhs, ast.Tuple):
            raise JitError(f"@jit.vfold: '{attr}' takes a single value column, not a pair")
        else:
            vfield = _vfold_df_field(rhs)
        if isinstance(mon, Counter) and vfield is not None:
            raise JitError(
                f"@jit.vfold: a jit.count() accumulator ('{attr}') counts rows; "
                "write += 1, not a column"
            )
        if vfield is not None:
            if vfield not in _VFOLD_TOP_FIELDS:
                raise JitError(f"@jit.vfold: column '{vfield}' is not a scan column")
            want_str = mon.elem == "str"
            if (vfield in _VFOLD_STRING_FIELDS) != want_str:
                raise JitError(
                    f"@jit.vfold: '{attr}' reduces a "
                    f"{'string' if want_str else 'numeric'} column, but '{vfield}' is not one"
                )
            fields.add(vfield)
        if byfield is not None:
            fields.add(byfield)
        fields.update(keyfields)
        ops.append(
            {
                "attr": attr,
                "mon": mon,
                "keyfields": keyfields,
                "vfield": vfield,
                "byfield": byfield,
            }
        )
    if not ops:
        raise JitError("@jit.vfold: the @jit.each_batch body is empty")
    return ops, fields


def _emit_vfold(
    ops: List[Dict[str, object]],
    plan_duql: str | None,
) -> str:
    plan_duql_field = "plan_duql" if plan_duql is not None else "NULL"
    out: List[str] = [
        "#include <dftracer/utils/plugins/abi.h>",
        "#include <dftracer/utils/dataframe/c_api.h>",
        "",
        "#include <stdint.h>",
        "#include <stdlib.h>",
        "",
    ]
    if plan_duql is not None:
        out += [
            "static const char* plan_duql(void* self) {",
            "    (void)self;",
            f"    return {_c_str_literal(plan_duql)};",
            "}",
            "",
        ]
    name_defs, name_assigns = _emit_name_lists(sorted({cast(str, opd["attr"]) for opd in ops}), [])
    out += name_defs
    out += [
        "static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {",
        "    (void)self;",
        "    *out = calloc(1, 1);",
        '    return *out ? DFTU_OK : dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
        "}",
        "",
        "static dftu_status on_batch(dftu_ctx* ctx, void* slice,",
        "                            const dftu_dataframe* df,",
        "                            const dftu_plugin_host* host) {",
        "    (void)slice;",
        "    const dftu_svc_agg* agg =",
        "        (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);",
        "    if (!agg || !agg->agg_new || !agg->agg_accumulate)",
        '        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "the host has no aggregation service");',
    ]
    for opd in ops:
        attr = cast(str, opd["attr"])
        keyfields = cast(List[str], opd["keyfields"])
        specs = _vfold_specs(opd)
        spec_rows = ", ".join("{" + ", ".join(s) + "}" for s in specs)
        out.append("    {")
        out.append(
            f"        const dftu_agg_col specs[{len(specs)}] = {{{spec_rows}}};",
        )
        if keyfields:
            key_list = ", ".join(_c_str_literal(k) for k in keyfields)
            out.append(f"        const char* keys[{len(keyfields)}] = {{{key_list}}};")
            keys_arg = "keys"
        else:
            keys_arg = "NULL"
        out += [
            "        dftu_agg* a = NULL;",
            f"        dftu_status s = agg->agg_new(ctx, host->h, {_c_str_literal(attr)}, "
            f"{keys_arg}, {len(keyfields)}u, specs, {len(specs)}u, &a);",
            "        if (s < 0) return s;",
            "        if (a) {",
            "            s = agg->agg_accumulate(ctx, host->h, a, df);",
            "            if (s < 0) return s;",
            "        }",
            "    }",
        ]
    out += [
        "    return DFTU_OK;",
        "}",
        "",
        "static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {",
        "    (void)ctx;",
        "    (void)into;",
        "    (void)other;",
        "    return DFTU_OK;",
        "}",
        "",
        "static dftu_status on_finalize(dftu_ctx* ctx, void* slice,",
        "                               const dftu_plugin_host* host, dftu_task** pending) {",
        "    (void)ctx;",
        "    (void)slice;",
        "    (void)host;",
        "    *pending = NULL;",
        "    return DFTU_OK;",
        "}",
        "",
        "static void destroy_slice(void* slice) { free(slice); }",
        "static void destroy(void* self) { (void)self; }",
        "",
        "static dftu_plugin g_plugin;",
        "",
        'DFTU_PLUGIN_INFO("dftracer.jit", NULL)',
        "",
        "DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h,",
        "                                               const dftu_value* config,",
        "                                               dftu_plugin** out) {",
        "    (void)ctx;",
        "    (void)h;",
        "    (void)config;",
        "    g_plugin.struct_size = sizeof(g_plugin);",
        "    g_plugin.self = NULL;",
        f"    g_plugin.plan_duql = {plan_duql_field};",
        "    g_plugin.make_slice = make_slice;",
        "    g_plugin.merge = merge;",
        "    g_plugin.on_finalize = on_finalize;",
        "    g_plugin.destroy_slice = destroy_slice;",
        "    g_plugin.destroy = destroy;",
        "    g_plugin.on_batch = on_batch;",
    ]
    out += name_assigns
    out += [
        "    *out = &g_plugin;",
        "    return DFTU_OK;",
        "}",
        "",
    ]
    return "\n".join(out)


def _build_vfold(cls: type[_C]) -> type[_C]:
    accums: Dict[str, _Monoid] = {}
    maps: Dict[str, _AnyMap] = {}
    batch: List[_EachBatch] = []
    for attr, val in vars(cls).items():
        if isinstance(val, _MapDecl):
            maps[attr] = val
        elif isinstance(val, _Monoid):
            accums[attr] = val
        elif isinstance(val, _EachBatch):
            batch.append(val)
    for attr, m in maps.items():
        if m.is_product:
            raise _class_error(
                cls,
                f"@jit.vfold: '{attr}' must be a simple jit.map(key=<type>, value=<reduction>)",
                attr,
            )
        if not m.key_types:
            raise _class_error(cls, f"@jit.vfold: '{attr}' needs at least one key", attr)
    if not accums and not maps:
        raise _class_error(
            cls, "@jit.vfold needs an accumulator (jit.sum()/min()/max()) or a jit.map"
        )
    if len(batch) != 1:
        raise _class_error(cls, "@jit.vfold needs exactly one @jit.each_batch method")
    plan_duql = getattr(cls, "plan_duql", None)
    if plan_duql is not None and not isinstance(plan_duql, str):
        raise _class_error(cls, "@jit.vfold plan_duql must be a query DSL string", "plan_duql")
    # Every batch's dyn arg columns are resolved unconditionally now (no
    # needs() negotiation), so the field set from _compile_vfold is only used
    # for its own validation, not to gate what on_batch resolves.
    ops, _fields = _compile_vfold(batch[0].fn, accums, maps)
    source = _emit_vfold(ops, plan_duql)
    setattr(cls, "_jit_plugin", JitPlugin(cls.__name__, source, {}, lowering="vfold"))
    return cls


def vfold(cls: type[_C]) -> type[_C]:
    """Author a vectorized fold: a per-batch fold whose body runs SIMD column
    ops on the batch and folds them into scalar accumulators or keyed maps.

    Declare scalar accumulators as a bare reduction (``jit.sum()``, ``jit.min()``,
    ...) and keyed accumulators as ``jit.map(key=<types>, value=<reduction>)``,
    plus one :func:`each_batch` method whose body is a sequence of
    ``self.<acc> += df["f"]`` (or the ``df["f"].sum()/min()/max()`` sugar) and
    ``self.<map>[df["k"]] += df["v"]`` (a multi-key map subscripts a tuple of
    columns, and ``+= 1`` counts rows into a ``jit.count()``). A reduction that
    reads a second column - argmin/argmax, top-k/bottom-k, ordered list, the
    occupancy ops and the co-moments - takes a ``(value, by)`` pair written
    ``self.<map>[df["k"]] += df["v"], df["by"]``.

    Every accumulator folds through the engine's DFTU_SVC_AGG AggState: a keyed
    map is one with key columns, a scalar accumulator one with none. The host
    merges same-named accumulators across worker slices and finalizes each to a
    native DataFrame; run it through
    :class:`dftracer.utils.plugins.Plugins` like any other jit plugin."""
    return _build_vfold(cls)


# The op registry is shared with the engine's built-ins, whose names are bare
# (`add`, `sum`) or `dftu.`-prefixed; both namespaces are the host's, so a user
# op must be `<module>.<name>` (plugin ABI rule, see plugins/reserved_names.h).
_HOST_NAME_PREFIX = "dftu."


def _series_module(
    fn: Callable[..., Expr], module: "str | None", package: "JitPackage | None"
) -> str:
    if module is not None:
        mod = module
    else:
        fn_module = getattr(fn, "__module__", None)
        if not isinstance(fn_module, str) or not fn_module:
            raise JitError('@jit.series needs a module prefix; pass module="<name>"')
        pkg = package if package is not None else _DEFAULT_PACKAGE
        # namespace + rest reconstructs fn.__module__ exactly for a default
        # package (namespace is its top-level segment), so an explicit
        # JitPackage only ever changes the leading segment.
        rest = _module_rest(fn_module)
        mod = pkg._namespace_for(fn_module) + (f".{rest}" if rest else "")
    if mod == _HOST_NAME_PREFIX[:-1] or mod.startswith(_HOST_NAME_PREFIX):
        raise JitError(
            f"module '{mod}' is reserved for host ops; the 'dftu.' namespace is not "
            "available to a user op"
        )
    return mod


def _series_impl(
    fn: Callable[..., Expr], module: "str | None", package: "JitPackage | None" = None
) -> Callable[..., Result]:
    from . import ops as _ops

    fname = getattr(fn, "__name__", None)
    if not isinstance(fname, str):
        raise JitError("@jit.series must decorate a named function")
    params = builtins.list(inspect.signature(fn).parameters.values())
    if builtins.any(p.kind not in (p.POSITIONAL_OR_KEYWORD, p.POSITIONAL_ONLY) for p in params):
        raise JitError("@jit.series takes only positional column arguments")
    n = len(params)
    if n == 0:
        raise JitError("@jit.series needs at least one column argument")
    built = fn(*[col(f"__x{i}__") for i in range(n)])
    if not isinstance(built, Expr):
        raise JitError("@jit.series body must return a column expression built from its arguments")
    name = f"{_series_module(fn, module, package)}.{fname}"
    _ops._register_user(name, n, built)
    return _ops.get(name)


@overload
def series(fn: "Callable[..., Expr]") -> "Callable[..., Result]": ...
@overload
def series(
    fn: None = None, *, module: "str | None" = None
) -> "Callable[[Callable[..., Expr]], Callable[..., Result]]": ...
def series(
    fn: "Callable[..., Expr] | None" = None, *, module: "str | None" = None
) -> "Callable[..., Result] | Callable[[Callable[..., Expr]], Callable[..., Result]]":
    """Author a reusable column op from an expression over its arguments.

    The body builds a lazy column expression from its Series arguments using the
    columnar DSL (arithmetic, comparisons, clip/cast/fillna, prims), e.g.::

        @jit.series
        def doubled(dur):
            return dur * 2

    It returns a callable, so ``doubled(s)`` applies it via the engine's Expr
    evaluator - no compile step, and it fuses with built-in Expr math.

    It also registers in dftracer.utils.jit.ops under ``<module>.<name>``, where
    the module defaults to the defining function's :class:`JitPackage` (a bare
    ``fn.__module__`` for a default package); pass ``module="stats"`` to choose
    it explicitly, or use :meth:`JitPackage.series` for a named package. The
    registry is shared with the engine's built-in ops, so a user op never takes
    a bare name, and the host's ``dftu.`` namespace is refused. Reach it as
    ``ops.run("stats.<name>", s)``, ``ops.stats.<name>(s)`` and
    ``s.ops.stats.<name>()``. Arguments are positional column operands."""
    if fn is None:
        return lambda f: _series_impl(f, module)
    return _series_impl(fn, module)


@overload
def plugin(cls: type[_C]) -> type[_C]: ...
@overload
def plugin(cls: None = None) -> Callable[[type[_C]], type[_C]]: ...
def plugin(cls: type | None = None) -> type | Callable[[type], type]:
    """Class decorator: AST-compile the state, the ``each_event`` or ``each_batch``
    body and any ``finalize`` and ``task`` methods to a C plugin.

    Attaches the emitted source under ``cls._jit_plugin``; the native ``.so`` is
    built lazily on first load. The batch columns the body reads are declared
    for you, from the same AST pass that emits it, so the scan materializes
    only those. Raises :class:`JitError` for any construct outside the
    supported subset.
    """
    if cls is None:
        return lambda c: _build_plugin(c)
    return _build_plugin(cls)


def plugin_result_names(obj: object) -> Dict[str, str]:
    """``{wire id: attribute name}`` for a ``@jit.plugin`` class's maps, so a
    host can rewrite its package-qualified result keys back to the attribute
    name callers index ``results[...]`` with."""
    spec = getattr(obj, "_jit_plugin", None)
    if isinstance(spec, JitPlugin):
        return spec.result_names
    return {}


def compile_plugin(
    spec: JitPlugin,
    *,
    cache_dir: "str | Path | None" = None,
    extra_cflags: "List[str] | None" = None,
    out: "str | None" = None,
) -> str:
    """Build ``spec`` to a native ``.so`` and return its path.

    With ``cache_dir``/``extra_cflags``/``out`` unset, this is a content-hashed
    cache build (an unchanged plugin never rebuilds); pass them (see
    :class:`JitPackage`) for a fixed output path or extra compiler flags.
    Raises :class:`JitError` if the compile fails.
    """
    try:
        include = _plugin_build.include_dir()
        cxx = _plugin_build.compiler()
        flags = builtins.list(_plugin_build.cflags())
        if extra_cflags:
            flags += builtins.list(extra_cflags)
        cache = Path(cache_dir) if cache_dir is not None else _plugin_build.cache_dir()
        cache.mkdir(parents=True, exist_ok=True)
        if out is not None:
            out_path = Path(out)
        else:
            digest = _plugin_build.source_digest(spec.source, include, cxx)
            # The digest is the directory, not part of the file name: the host
            # names a plugin, and its store namespaces, after the .so stem.
            out_path = cache / digest / f"{spec.name}.so"
            if out_path.is_file():
                return str(out_path)
            out_path.parent.mkdir(parents=True, exist_ok=True)
        src_path = out_path.with_suffix(".cpp")
        src_path.write_text(spec.source, encoding="utf-8")
        cmd = [cxx, *flags, "-o", str(out_path), str(src_path)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise _plugin_build.PluginBuildError(proc.stderr)
        return str(out_path)
    except _plugin_build.PluginBuildError as exc:
        raise JitError(f"jit compile failed:\n{exc}", locate=False) from exc


def compile_class(obj: object) -> str:
    """Compile a ``@jit.plugin`` class to a native ``.so`` and return the path."""
    spec = getattr(obj, "_jit_plugin", None)
    if not isinstance(spec, JitPlugin):
        raise TypeError("expected a @jit.plugin-decorated class")
    return compile_plugin(spec)


def build(cls: type, *, out: "str | None" = None) -> str:
    """Compile ``cls`` to a shippable native ``.so`` under the default package.

    Equivalent to ``JitPackage().build(cls, out=out)``; raises :class:`JitError`
    for a class authored in ``__main__`` (pass a :class:`JitPackage` there
    instead - see :meth:`JitPackage.build`)."""
    return _DEFAULT_PACKAGE.build(cls, out=out)


def is_jit_plugin(obj: object) -> bool:
    """True if ``obj`` is a ``@jit.plugin``-decorated class."""
    return isinstance(getattr(obj, "_jit_plugin", None), JitPlugin)


def provided_names(cls: type) -> Dict[str, str]:
    """``{attribute: qualified identity}`` for every name ``cls`` provides: each
    :func:`map` accumulator and each :func:`publish` port, already resolved
    under whichever :class:`JitPackage` decorated it."""
    spec = getattr(cls, "_jit_plugin", None)
    if not isinstance(spec, JitPlugin):
        raise TypeError("expected a @jit.plugin-decorated class")
    names: Dict[str, str] = {attr: qid for qid, attr in spec.result_names.items()}
    for attr, val in vars(cls).items():
        if isinstance(val, _Port) and val.role == "publish" and val.name is not None:
            names[attr] = val.name
    return names
