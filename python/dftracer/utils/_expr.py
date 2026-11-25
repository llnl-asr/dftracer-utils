# SPDX-License-Identifier: MIT
"""The unified expression DSL: one ``F`` for both column values and row filters.

``F.dur`` (or ``col("dur")`` / ``F("args.level")``) is an :class:`Expr` leaf.
Build from it in two directions with the same object:

- **Value expressions** - ``+ - * /``, scalar broadcast, and the numeric
  primitives (``.ilog2()``, ``.bit_width()``, ``.popcount()``, ``.clz()``,
  ``.ctz()``, ``.mix64()``). Evaluate in memory with ``.apply(source)``, which
  runs on our native SIMD columnar engine (a ``Series``, Highway-backed) and
  returns a native :class:`~dftracer.utils.Series`. Arrow appears only at the
  explicit ``.to_arrow()`` / ``.to_pandas()`` edge.
- **Filter predicates** - comparisons (``> >= < <= == !=``), string matching
  (``.like()``, ``.ilike()``, ``.regex()``, ``.contains()``, ``.starts_with()``,
  ``.ends_with()``), membership (``.is_in()`` / ``.not_in()``) and
  ``& | ~``. A predicate over a bare field serializes to
  duql (``str(expr)`` / ``.to_duql()``) that ``TraceViewer.filter()``
  / ``.duql()`` push down to the index.
- **String values** - ``.lower()``, ``.upper()``, ``.strip()``, ``.len_bytes()``,
  ``.find()``, ``.replace()``, ``.slice()`` and friends.

Every predicate and string op also evaluates in memory to a Series via
``.apply()`` and runs inside a ``LazyFrame`` plan, through the engine's own
string kernels. The two exceptions are ``iregex`` (filter-only) and a bool
comparison.
"""

from __future__ import annotations

import atexit
import builtins
import contextlib
import copy
import datetime
import math
import typing
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    ContextManager,
    Generator,
    Iterator,
    Literal,
    Mapping,
    NoReturn,
    Sequence,
    TypeVar,
    Union,
    overload,
)

from . import dftracer_utils_ext as _ext
from ._aliases import install as _install_aliases
from ._convert import _arrow_native
from ._enums import (
    CAST_DTYPES,
    DT_UNIT_CODES,
    EPOCH_NS,
    UNIT_CODES,
    DType,
    coerce_duration,
    rule_to_units,
)
from ._series import (
    NormForm,
    Series,
    norm_form_code,
    require_utf8,
    translate_pairs,
    wrap_flag_bits,
)
from .errors import DFTUtilsValueError

if TYPE_CHECKING:
    from typing_extensions import Concatenate, ParamSpec

    from ._eval import Columnar, Frame, Source

_R = TypeVar("_R")
if TYPE_CHECKING:
    _P = ParamSpec("_P")


# SPDX-License-Identifier: MIT
"""The unified expression DSL: one ``F`` for both column values and row filters.

``F.dur`` (or ``col("dur")`` / ``F("args.level")``) is an :class:`Expr` leaf.
Build from it in two directions with the same object:

- **Value expressions** - ``+ - * /``, scalar broadcast, and the numeric
  primitives (``.ilog2()``, ``.bit_width()``, ``.popcount()``, ``.clz()``,
  ``.ctz()``, ``.mix64()``). Evaluate in memory with ``.apply(source)``, which
  runs on our native SIMD columnar engine (a ``Series``, Highway-backed) and
  returns a native :class:`~dftracer.utils.Series`. Arrow appears only at the
  explicit ``.to_arrow()`` / ``.to_pandas()`` edge.
- **Filter predicates** - comparisons (``> >= < <= == !=``), string matching
  (``.like()``, ``.ilike()``, ``.regex()``, ``.contains()``, ``.starts_with()``,
  ``.ends_with()``), membership (``.is_in()`` / ``.not_in()``) and
  ``& | ~``. A predicate over a bare field serializes to
  duql (``str(expr)`` / ``.to_duql()``) that ``TraceViewer.filter()``
  / ``.duql()`` push down to the index.
- **String values** - ``.lower()``, ``.upper()``, ``.strip()``, ``.len_bytes()``,
  ``.find()``, ``.replace()``, ``.slice()`` and friends.

Every predicate and string op also evaluates in memory to a Series via
``.apply()`` and runs inside a ``LazyFrame`` plan, through the engine's own
string kernels. The two exceptions are ``iregex`` (filter-only) and a bool
comparison.
"""


Scalar = Union[int, float]


#: A scalar the filter DSL can compare a field against.
Value = Union[str, int, float, bool]


# TypeId codes the .cast() method accepts, off DType so the ordinals live in one place.
CAST_CODES = {name: int(dtype) for name, dtype in CAST_DTYPES.items()}


# vec::Prim codes (kernels/prims.h). Exposed as methods, e.g. F.dur.ilog2().
PRIM_CODES = {
    "ilog2": 0,
    "bit_width": 1,
    "popcount": 2,
    "clz": 3,
    "ctz": 4,
    "mix64": 5,
}


# DFTU_CMP_* and DFTU_LOGICAL_* codes (dataframe/c_api.h).
CMP_CODES = {"gt": 0, "ge": 1, "lt": 2, "le": 3, "eq": 4, "ne": 5}


_CMP_SYMBOL = {"gt": ">", "ge": ">=", "lt": "<", "le": "<=", "eq": "==", "ne": "!="}


LOGICAL_CODES = {"and": 0, "or": 1}


# vec_eval opcodes (mirror the Op enum in columnar_eval.cpp).
# Post-order AST node opcodes for vec_eval (mirror columnar_eval.cpp). This is a
# pure structural serialization - no type logic; the C++ compiler does type
# inference, CSE, and lowering, so C/C++ consumers get the same engine.
_AST_COL, _AST_LIT_I, _AST_LIT_F, _AST_BIN = 0, 1, 2, 3


_AST_PRIM, _AST_CMP, _AST_LOGICAL, _AST_NOT, _AST_CAST = 4, 5, 6, 7, 8


_AST_UNARY, _AST_CLIP, _AST_FILLNA = 9, 10, 11


_AST_STR_PRED, _AST_STR_MAP, _AST_STR_LEN, _AST_STR_FIND = 12, 13, 14, 15


_AST_STR_REPLACE, _AST_STR_SLICE, _AST_IS_IN, _AST_SELECT = 16, 17, 18, 19


_AST_STR_PRED_COL, _AST_STR_REPLACE_COL, _AST_LIT_S = 23, 24, 25


_AST_UDF = 26


_AST_IS_NULL, _AST_CMP_EXPR, _AST_STR_FN = 20, 21, 22


# StrPredOp / StrMapOp codes (dataframe/types.h).
_STR_PRED_CODES = {
    "contains": 0,
    "starts_with": 1,
    "ends_with": 2,
    "like": 3,
    "fullmatch": 4,
    "regex": 5,
}


_STR_MAP_CODES = {
    "lower": 0,
    "upper": 1,
    "strip": 2,
    "lstrip": 3,
    "rstrip": 4,
    "capitalize": 5,
    "title": 6,
    "swapcase": 7,
}


# StrFn codes (dataframe/types.h); 0-8 are the character classes.
_STR_FN_CODES = {
    "isalnum": 0,
    "isalpha": 1,
    "isdigit": 2,
    "isdecimal": 3,
    "isnumeric": 4,
    "isspace": 5,
    "islower": 6,
    "isupper": 7,
    "istitle": 8,
    "pad_start": 9,
    "pad_end": 10,
    "center": 11,
    "zfill": 12,
    "removeprefix": 13,
    "removesuffix": 14,
    "repeat": 15,
    "slice_replace": 16,
    "split": 17,
    "partition": 18,
    "rpartition": 19,
    "findall": 20,
    "extract": 21,
    "rfind": 22,
    "index": 23,
    "rindex": 24,
    "join": 25,
    "get": 26,
    "cat": 27,
    "regex_replace": 28,
    "normalize": 29,
    "translate": 30,
    "wrap": 31,
}


_STR_NO_STOP = -(1 << 63)  # slice_replace without a stop (INT64_MIN)


# UnaryOp codes (dataframe/types.h). Exposed as F.dur.floor() etc. The is_*
# predicates yield a Bool mask; log/sqrt/exp widen to float.
_UNARY_CODES = {
    "abs": 0,
    "round": 1,
    "floor": 2,
    "ceil": 3,
    "log": 4,
    "sqrt": 5,
    "exp": 6,
    "sign": 7,
    "negate": 8,
    "trunc": 9,
    "is_nan": 10,
    "is_finite": 11,
    "is_infinite": 12,
    "log1p": 13,
    "expm1": 14,
}


# Unary ops whose result is a Bool mask (not the input's numeric type).
UNARY_BOOL = frozenset({"is_nan", "is_finite", "is_infinite"})


# Unary ops that widen to float.
UNARY_FLOAT = frozenset({"log", "sqrt", "exp", "log1p", "expm1"})


# BinaryOp codes (vec::BinaryOp).
_BIN_OP_CODE = {"+": 0, "-": 1, "*": 2, "/": 3}


# vec::AggOp codes (agg.h), mirrored by the C ABI DFTU_AGG_* and the C++ builders.
AGG_CODE = {
    "count": 0,
    "sum": 1,
    "min": 2,
    "max": 3,
    "mean": 4,
    "var": 5,
    "std": 6,
    "skew": 7,
    "kurt": 8,
    "skew_unbiased": 38,
    "kurt_unbiased": 39,
    "first": 9,
    "last": 10,
    "pct": 11,
    "hist": 12,
    "argmax": 13,
    "sumsq": 14,
    "set_union": 15,
    "count_valid": 20,
    "argmin": 21,
    "distinct": 23,
    "list_sorted": 24,
    "topk": 25,
    "bottomk": 26,
    "approx_topk": 27,
    "sample": 28,
    "corr": 29,
    "covar_pop": 30,
    "covar_samp": 31,
    "regr_slope": 32,
    "regr_intercept": 33,
    "regr_r2": 34,
    "prod": 35,
    "sketch_quantile": 36,
    "udaf": 37,
    "quantile": 40,
    "arg_max": 41,
    "arg_min": 42,
    "bitwise_and": 43,
    "bitwise_or": 44,
    "bitwise_xor": 45,
}


_INTERPOLATIONS = {"linear": 0, "nearest": 1, "lower": 2, "higher": 3, "midpoint": 4}


def check_quantile(q: float, interpolation: str) -> int:
    """The checked level ``q`` and ``interpolation`` of the exact ``quantile``
    aggregate; the interpolation's code (``Interpolation`` in types.h)."""
    if not 0.0 <= q <= 1.0:
        raise ValueError(f"quantile: q must be in [0, 1], not {q!r}")
    code = _INTERPOLATIONS.get(interpolation) if isinstance(interpolation, str) else None
    if code is None:
        raise ValueError(
            "quantile: interpolation must be 'linear', 'nearest', 'lower', 'higher' or 'midpoint'"
        )
    return code


_NOT_PUSHABLE = (
    "not an index-pushable predicate; compute it with .apply() or filter the materialized frame"
)


_PREDICATE_ONLY = (
    "a case-insensitive regex (iregex) is filter-only and has no in-memory "
    ".apply() form; push it down with TraceViewer.filter()/.duql(), or use "
    "regex() on lower()"
)


# The group transform behind an ``over`` for the kinds the engine has, keyed
# by the column op's registry name.
TRANSFORM_KINDS: dict[str, str] = {
    "dftu.series.cumsum": "cumsum",
    "dftu.series.cum_prod": "cumprod",
    "dftu.series.cummax": "cummax",
    "dftu.series.cummin": "cummin",
    "dftu.series.shift": "shift",
    "dftu.series.diff": "diff",
    "dftu.series.pct_change": "pct_change",
    "dftu.series.rank": "rank",
    "dftu.series.ffill": "ffill",
    "dftu.series.bfill": "bfill",
}

RANK_METHODS = {"average": 0, "min": 1, "dense": 2, "ordinal": 3, "first": 3, "max": 4}
ROLLING_OPS = {"sum": 0, "mean": 1, "min": 2, "max": 3}


class ColumnOp:
    """One registered column op and its operands, as the expression language
    carries it: ``op`` is the registry name, ``column2`` a second column
    operand (an expression), ``a`` / ``b`` the scalar operands, ``text`` a
    string or char operand, in the op's signature order."""

    __slots__ = ("op", "column2", "a", "b", "text", "over", "kind", "n", "agg", "unit")

    def __init__(
        self,
        op: str,
        column2: Expr | None = None,
        a: Scalar | None = 0,
        b: Scalar = 0,
        text: str | None = None,
        over: list[str] | None = None,
        kind: str | None = None,
        n: int = 0,
        agg: Agg | None = None,
        unit: str | None = None,
    ) -> None:
        self.op = op
        self.column2 = column2
        self.a = a
        self.b = b
        self.text = text
        self.over = over
        self.kind = kind
        self.n = n
        # An aggregate's over: the group value broadcast back onto its rows.
        self.agg = agg
        # A dt_round rule kept as text in `text`, turned into a count of the
        # column's own unit once the column is known; `unit` is the unit the
        # caller named, None to read the column's.
        self.unit = unit


if TYPE_CHECKING:

    class _Namespaces:
        """The ``str`` and ``dt`` namespaces, bound on ``Expr`` after its body
        (below); declared in a base so the name ``str`` does not shadow the
        builtin type inside the ``Expr`` body."""

        @property
        def str(self) -> _ExprStr: ...
        @property
        def dt(self) -> _ExprDt: ...

else:
    _Namespaces = object


class Expr(_Namespaces):
    """A lazy expression. Build values with ``+ - * /`` and the numeric prims,
    or predicates with comparisons / string-match / membership / ``& | ~``.
    Evaluate a value or a numeric-comparison mask in memory with :meth:`apply`;
    serialize a pure predicate for index pushdown with :meth:`to_duql`."""

    def __add__(self, other: object) -> Expr:
        return Bin("+", self, wrap(other))

    def __sub__(self, other: object) -> Expr:
        return Bin("-", self, wrap(other))

    def __mul__(self, other: object) -> Expr:
        return Bin("*", self, wrap(other))

    def __truediv__(self, other: object) -> Expr:
        return Bin("/", self, wrap(other))

    def __radd__(self, other: object) -> Expr:
        return Bin("+", wrap(other), self)

    def __rsub__(self, other: object) -> Expr:
        return Bin("-", wrap(other), self)

    def __rmul__(self, other: object) -> Expr:
        return Bin("*", wrap(other), self)

    def __rtruediv__(self, other: Expr | Agg | int | float) -> Expr:
        return Bin("/", wrap(other), self)

    def _series_arith(self, name: str, other: object, reverse: bool = False) -> Expr:
        """The Python-semantics ``floordiv`` / ``mod`` / ``pow`` of the Series
        kernels, as a column op: ``other`` is an expression or a number."""

        if isinstance(other, Expr):
            first, second = (other, self) if reverse else (self, other)
            return ColumnOpNode(first, ColumnOp(f"dftu.series.{name}", second))
        if isinstance(other, bool) or not isinstance(other, (int, float)):
            raise TypeError(f"{name}: expected a column expr or a number, got {other!r}")
        return ColumnOpNode(
            self, ColumnOp(f"dftu.series.{name}_scalar", None, other, 1 if reverse else 0)
        )

    def __floordiv__(self, other: object) -> Expr:
        return self._series_arith("floordiv", other)

    def __rfloordiv__(self, other: object) -> Expr:
        return self._series_arith("floordiv", other, True)

    def __mod__(self, other: object) -> Expr:
        return self._series_arith("mod", other)

    def __rmod__(self, other: object) -> Expr:
        return self._series_arith("mod", other, True)

    def __pow__(self, other: object) -> Expr:
        return self._series_arith("pow", other)

    def __rpow__(self, other: object) -> Expr:
        return self._series_arith("pow", other, True)

    def floordiv(self, other: Expr | int | float) -> Expr:
        """Floor division (Python semantics); ``//``."""
        return self._series_arith("floordiv", other)

    def mod(self, other: Expr | int | float) -> Expr:
        """Remainder with the sign of the divisor (Python semantics); ``%``."""
        return self._series_arith("mod", other)

    def pow(self, exponent: Expr | int | float) -> Expr:
        """This expression raised to ``exponent``; ``**``."""
        return self._series_arith("pow", exponent)

    # Comparisons build predicate Exprs. Against a number, or == / != against a
    # string, they also evaluate in memory to a boolean mask; against a bool
    # they are filter-only. Against another column expression they compare the
    # two row by row (null where either side is null) and are not pushable.
    def __gt__(self, other: Value | Expr) -> Expr:
        return Cmp("gt", self, other)

    def __ge__(self, other: Value | Expr) -> Expr:
        return Cmp("ge", self, other)

    def __lt__(self, other: Value | Expr) -> Expr:
        return Cmp("lt", self, other)

    def __le__(self, other: Value | Expr) -> Expr:
        return Cmp("le", self, other)

    def __eq__(self, other: Value | Expr) -> Expr:
        return Cmp("eq", self, other)

    def __ne__(self, other: Value | Expr) -> Expr:
        return Cmp("ne", self, other)

    def eq(self, other: Value | Expr) -> Expr:
        """Equality predicate (the method form of ``==``)."""
        return Cmp("eq", self, other)

    def ne(self, other: Value | Expr) -> Expr:
        """Inequality predicate (the method form of ``!=``)."""
        return Cmp("ne", self, other)

    def lt(self, other: Value | Expr) -> Expr:
        """Less-than predicate (the method form of ``<``)."""
        return Cmp("lt", self, other)

    def le(self, other: Value | Expr) -> Expr:
        """Less-or-equal predicate (the method form of ``<=``)."""
        return Cmp("le", self, other)

    def gt(self, other: Value | Expr) -> Expr:
        """Greater-than predicate (the method form of ``>``)."""
        return Cmp("gt", self, other)

    def ge(self, other: Value | Expr) -> Expr:
        """Greater-or-equal predicate (the method form of ``>=``)."""
        return Cmp("ge", self, other)

    def add(self, other: Value | Expr) -> Expr:
        """The method form of ``+``."""
        return Bin("+", self, wrap(other))

    def sub(self, other: Value | Expr) -> Expr:
        """The method form of ``-``."""
        return Bin("-", self, wrap(other))

    def mul(self, other: Value | Expr) -> Expr:
        """The method form of ``*``."""
        return Bin("*", self, wrap(other))

    def truediv(self, other: Value | Expr) -> Expr:
        """The method form of ``/``."""
        return Bin("/", self, wrap(other))

    def neg(self) -> Expr:
        """The negation of each value."""
        return Unary("negate", self)

    def and_(self, other: Value | Expr) -> Expr:
        """The method form of ``&``."""
        return Logical("and", self, as_expr(other))

    def or_(self, other: Value | Expr) -> Expr:
        """The method form of ``|``."""
        return Logical("or", self, as_expr(other))

    def not_(self) -> Expr:
        """The method form of ``~``."""
        return Logical("not", self, None)

    def radians(self) -> Expr:
        """Each value, in degrees, as radians (Float64; polars ``radians``)."""
        return self * (math.pi / 180.0)

    def degrees(self) -> Expr:
        """Each value, in radians, as degrees (Float64; polars ``degrees``)."""
        return self * (180.0 / math.pi)

    def pipe(
        self,
        function: Callable[Concatenate[Expr, _P], _R],
        *args: _P.args,
        **kwargs: _P.kwargs,
    ) -> _R:
        """``function(self, *args, **kwargs)`` (polars ``pipe``)."""
        return function(self, *args, **kwargs)

    def count(self) -> Agg:
        """The number of non-null values of this expression per group, as an
        :class:`Agg` (pandas ``count``). The group row count is the bare
        :func:`count`."""
        return Agg("count_valid", self)

    # Comparisons build exprs, not bools, so a column is unhashable.
    if TYPE_CHECKING:

        def __hash__(self) -> NoReturn: ...
    else:
        __hash__ = None

    # A column has no single truth value; `if expr:` or `and` / `or` on one
    # would otherwise take one branch for every row. pandas and numpy refuse
    # this the same way.
    def __bool__(self) -> bool:
        raise TypeError(
            "the truth value of a column expression is ambiguous; use & | ~ to "
            "combine predicates, or is_between()/clip() in place of a branch"
        )

    # Combine predicates: &, |, ~ (Python and/or cannot be overloaded).
    def __and__(self, other: object) -> Expr:
        return Logical("and", self, as_expr(other))

    def __or__(self, other: object) -> Expr:
        return Logical("or", self, as_expr(other))

    def __invert__(self) -> Expr:
        return Logical("not", self, None)

    if TYPE_CHECKING:
        # Installed by ``_expr_ops``: evaluating reaches the engine and the
        # group-by, which build on this module.
        def apply(self, source: Source) -> Series: ...

    def to_duql(self) -> str:
        """Serialize this predicate to an index-pushable duql string.

        Raises ``TypeError`` if the expression mixes value ops (arithmetic or
        numeric prims) into the predicate, i.e. it is not pushable; evaluate
        those in memory with :meth:`apply` instead."""
        return self._dsl()

    def _dsl(self) -> str:
        raise TypeError(_NOT_PUSHABLE)

    def __str__(self) -> str:
        return self._dsl()

    def clip(
        self,
        lower_bound: int | float | None = None,
        upper_bound: int | float | None = None,
    ) -> Expr:
        """Clamp each value to ``[lower_bound, upper_bound]`` (elementwise,
        SIMD); a bound that is ``None`` or NaN clips nothing and a NaN value
        stays NaN, as in Polars."""
        lo = -math.inf if lower_bound is None else lower_bound
        hi = math.inf if upper_bound is None else upper_bound
        return Clip(self, lo, hi)

    def cast(
        self, dtype: Literal["int64", "uint64", "float64", "bool", "string", "binary"]
    ) -> Expr:
        """Cast each value to ``dtype``. ``'string'`` gives the decimal digits of an
        integer, the shortest text of a float that shows a point or exponent
        (``2.0``, ``1e+21``) and ``true`` / ``false`` for a bool, the engine's
        spelling (the eager ``Series.astype`` says ``True`` / ``False``); a null
        stays null. A pair the engine cannot cast, such as string to bool, fails
        when the plan runs."""
        return Cast(CAST_CODES[dtype], self)

    def fill_null(self, value: Value) -> Expr:
        """Replace nulls with ``value`` (elementwise)."""
        return Fillna(self, value)

    def fill_nan(self, value: int | float | None) -> Expr:
        """Replace the NaN of a float column with ``value`` (null for
        ``None``); a null stays null and another type is unchanged (a column
        op: the whole column at once)."""
        return self._op("dftu.series.fill_nan", a=value)

    def fillna(self, value: int | float | builtins.str) -> Expr:
        """The pandas ``fillna``: a null and, in a float column, a NaN become
        ``value``."""
        return Fillna(self, value, True)

    def is_not_nan(self) -> Expr:
        """Bool mask of the values that are not NaN (a null stays null)."""
        return ~self.is_nan()

    def is_between(self, lower: int | float, upper: int | float) -> Expr:
        """Bool mask of ``lower <= x <= upper`` (desugars to two compares)."""
        return (self >= lower) & (self <= upper)

    def where(self, cond: Expr, other: Expr | int | float) -> Expr:
        """Per row, this value where ``cond`` holds, else ``other`` (pandas
        ``Series.where``; a null condition takes ``other``)."""
        return Select(as_expr(cond), self, wrap(other))

    def mask(self, cond: Expr, other: Expr | int | float) -> Expr:
        """Per row, ``other`` where ``cond`` holds, else this value (pandas
        ``Series.mask``)."""
        return Select(as_expr(cond), wrap(other), self)

    def is_null(self) -> Expr:
        """Bool mask of the null rows."""
        return IsNull(self, True)

    def is_not_null(self) -> Expr:
        """Bool mask of the present rows."""
        return IsNull(self, False)

    def isna(self) -> Expr:
        """The pandas ``isna``: the Bool mask of the null rows and, in a float
        column, the NaN rows (a row-wise node, so a plan streams it)."""
        return IsNull(self, True, True)

    def notna(self) -> Expr:
        """The pandas ``notna``: the Bool mask of the rows that are neither null
        nor, in a float column, NaN."""
        return IsNull(self, False, True)

    # -- the column ops: the whole column at once (polars spellings) ----------
    def _op(
        self,
        op: str,
        a: int | float | None = 0,
        b: int | float = 0,
        text: str | None = None,
        kind: str | None = None,
        n: int = 0,
        unit: str | None = None,
    ) -> Expr:

        return ColumnOpNode(self, ColumnOp(op, None, a, b, text, None, kind, n, None, unit))

    def cum_sum(self) -> Expr:
        """The running sum (``cum_prod`` / ``cum_max`` / ``cum_min`` /
        ``cum_count`` likewise): a column op, the whole column at once; in a
        plan a breaker step. ``.over(keys)`` makes it group-wise."""
        return self._op("dftu.series.cumsum")

    def cum_max(self) -> Expr:
        return self._op("dftu.series.cummax")

    def cum_min(self) -> Expr:
        return self._op("dftu.series.cummin")

    def between(self, left: int | float, right: int | float) -> Expr:
        """The pandas ``between``: :meth:`is_between` with both ends included,
        pandas' default ``inclusive="both"``."""
        return self.is_between(left, right)

    def shift(self, n: int = 1) -> Expr:
        return self._op("dftu.series.shift", a=int(n), n=int(n))

    def rank(self, method: str = "average", descending: bool = False) -> Expr:

        if method not in RANK_METHODS:
            raise ValueError(f"rank: unknown method {method!r}")
        return self._op("dftu.series.rank", a=RANK_METHODS[method], b=1 if descending else 0)

    def forward_fill(self) -> Expr:
        return self._op("dftu.series.ffill")

    def backward_fill(self) -> Expr:
        return self._op("dftu.series.bfill")

    def rolling_sum(self, window_size: int) -> Expr:
        """The trailing window of ``window_size`` rows (``rolling_mean`` /
        ``_min`` / ``_max`` / ``_var`` / ``_std`` / ``_median`` /
        ``_quantile`` likewise), null until it is full."""
        return self._rolling("sum", window_size)

    def rolling_mean(self, window_size: int) -> Expr:
        return self._rolling("mean", window_size)

    def rolling_min(self, window_size: int) -> Expr:
        return self._rolling("min", window_size)

    def rolling_max(self, window_size: int) -> Expr:
        return self._rolling("max", window_size)

    def _rolling(self, op: str, window_size: int) -> Expr:

        if window_size < 1:
            raise ValueError("rolling: window_size must be at least 1")
        return self._op(
            "dftu.series.rolling",
            a=int(window_size),
            b=ROLLING_OPS[op],
            kind=f"rolling_{op}",
            n=int(window_size),
        )

    def rolling_var(self, window_size: int) -> Expr:
        return self._op("dftu.series.rolling_var", a=int(window_size))

    def rolling_std(self, window_size: int) -> Expr:
        return self._op("dftu.series.rolling_std", a=int(window_size))

    def rolling_median(self, window_size: int) -> Expr:
        return self._op("dftu.series.rolling_median", a=int(window_size))

    def rolling_quantile(self, quantile: float, window_size: int) -> Expr:
        return self._op("dftu.series.rolling_quantile", a=int(window_size), b=float(quantile))

    def ewm_mean(self, alpha: float) -> Expr:
        return self._op("dftu.series.ewm_mean", a=float(alpha))

    def ewm_std(self, alpha: float) -> Expr:
        return self._op("dftu.series.ewm_std", a=float(alpha))

    def sort(self, *, descending: bool = False, nulls_last: bool = False) -> Expr:
        """This column's values in order (the other columns do not move, as
        polars ``col.sort()`` inside ``with_columns``). Nulls come first, or
        last with ``nulls_last=True``."""
        return self._op("dftu.series.sort", a=1 if descending else 0, b=1 if nulls_last else 0)

    def arg_sort(self, descending: bool = False, nulls_last: bool = False) -> Expr:
        """The row indices that sort this column; nulls first by default, or
        last with ``nulls_last=True``. Ties keep input order."""
        return self._op("dftu.series.argsort", a=1 if descending else 0, b=1 if nulls_last else 0)

    def duplicated(self, keep: Literal["first", "last"] | bool = "first") -> Expr:
        """Bool mask of the values that repeat an earlier one (pandas
        ``Series.duplicated``): ``keep="first"`` leaves the first of equal
        values unmarked, ``"last"`` the last, ``False`` marks every one
        (:meth:`is_duplicated`). Nulls compare equal. ``"first"`` and
        ``"last"`` need a bare column, as ``col("x").duplicated()``."""
        if keep is False:
            return self.is_duplicated()
        if keep not in ("first", "last"):
            raise ValueError(f"duplicated: keep must be 'first', 'last' or False, not {keep!r}")
        if not isinstance(self, Col):
            raise TypeError("duplicated(keep=...): use a bare column, as col('x').duplicated()")
        one = self.is_null().cast("int64") * 0 + 1
        nth = one.cum_sum().over(self.name)
        if keep == "first":
            return nth > 1
        return nth < one.sum().over(self.name)

    def hash(self) -> Expr:
        """A 64-bit hash of each value (``fnv1a`` over its repr)."""
        return self._op("dftu.hash.fnv1a")

    def over(self, keys: str | Sequence[str]) -> Expr:
        """The group-wise form of the column op this expression is (polars
        ``over``): ``col("x").cum_sum().over("k")`` is the engine's group
        transform, one value per input row in input order. An aggregate's
        ``over`` is spelled ``col("x").sum().over("k")`` on the aggregate."""

        if not isinstance(self, ColumnOpNode):
            raise TypeError(
                "over: only a column op (cum_sum, shift, rank, rolling_*, ...) takes over()"
            )
        spec = self.spec
        if spec.op == "dftu.series.cum_count":
            # The running count of present values within each group.
            return self.arg.is_not_null().cast("int64").cum_sum().over(keys)
        kind = spec.kind or TRANSFORM_KINDS.get(spec.op)
        if kind is None:
            raise TypeError(f"over: {spec.op} has no group-wise form")

        spec = copy.copy(spec)
        spec.over = [keys] if isinstance(keys, str) else list(keys)
        spec.kind = kind
        return ColumnOpNode(self.arg, spec)

    def alias(self, name: str) -> Named:
        """This expression under an output name, for ``select`` /
        ``with_columns`` (the polars spelling)."""
        return Named(name, self)

    # String predicates (Bool masks). Each runs in memory through the engine's
    # string kernels and, on a bare field, pushes down as duql.
    def contains(self, sub: str | Expr, case: bool = True) -> Expr:
        """Substring predicate. Case-sensitive like pandas / polars; ``case=False``
        folds both sides to lowercase first (duql's ``"sub" in field``). ``sub``
        may be an expression: the per-row needle, null where either side is
        null, true for an empty needle (``case=False`` needs a ``str``)."""
        if isinstance(sub, Expr):
            if not case:
                raise TypeError("contains(case=False) needs a str needle")
            return _StrPredCol("contains", self, sub)
        if case:
            return _StrPred("contains", self, sub)
        return _IContains(self, sub)

    def starts_with(self, prefix: str | Expr) -> Expr:
        """Prefix predicate; ``prefix`` may be an expression (per-row needle)."""
        if isinstance(prefix, Expr):
            return _StrPredCol("starts_with", self, prefix)
        return _StrPred("starts_with", self, prefix)

    def ends_with(self, suffix: str | Expr) -> Expr:
        """Suffix predicate; ``suffix`` may be an expression (per-row needle)."""
        if isinstance(suffix, Expr):
            return _StrPredCol("ends_with", self, suffix)
        return _StrPred("ends_with", self, suffix)

    def like(self, pattern: str) -> Expr:
        """SQL LIKE: ``%`` matches any run, ``_`` any single char, ``\\`` escapes."""
        return _StrPred("like", self, pattern)

    def ilike(self, pattern: str) -> Expr:
        """Case-insensitive LIKE (both sides lowercased)."""
        return _ILike(self, pattern)

    def regex(self, pattern: str) -> Expr:
        """duql regex found anywhere in the value (a search, like
        duql's ``~``). See :meth:`fullmatch` for a whole-string match."""
        return _StrPred("regex", self, pattern)

    def iregex(self, pattern: str) -> Expr:
        """Case-insensitive regex search. Filter-only: it has no in-memory form."""
        return _IRegex(self, pattern)

    def fullmatch(self, pattern: str) -> Expr:
        """duql regex matching the whole value (pandas ``str.fullmatch``).
        In-memory only: duql has no anchored form."""
        return _StrPred("fullmatch", self, pattern)

    def encode(self, encoding: str = "utf-8") -> Expr:
        """The UTF-8 bytes of each string, as a binary column (pandas
        ``str.encode``). Only UTF-8, the engine's text encoding."""
        require_utf8("encode", encoding)
        return self.cast("binary")

    def decode(self, encoding: str = "utf-8") -> Expr:
        """Each binary value read as UTF-8 text (pandas ``str.decode``); a
        value that is not valid UTF-8 is null. Only UTF-8."""
        require_utf8("decode", encoding)
        return self.cast("string")

    def isascii(self) -> Expr:
        """True where every character is ASCII, as ``str.isascii`` (an empty
        string too)."""
        return self.fullmatch("[\\x00-\\x7f]*")

    def is_in(self, values: Sequence[Value]) -> Expr:
        """Membership predicate: the value is one of ``values`` (all ints or all
        strings)."""
        return _IsIn(self, values, negate=False)

    def not_in(self, values: Sequence[Value]) -> Expr:
        """Exclusion predicate: the value is none of ``values``."""
        return _IsIn(self, values, negate=True)

    # String maps.
    def lower(self) -> Expr:
        """ASCII lowercase."""
        return _StrMap("lower", self)

    def upper(self) -> Expr:
        """ASCII uppercase."""
        return _StrMap("upper", self)

    def strip(self) -> Expr:
        """Drop leading and trailing whitespace."""
        return _StrMap("strip", self)

    def lstrip(self) -> Expr:
        """Drop leading whitespace."""
        return _StrMap("lstrip", self)

    def rstrip(self) -> Expr:
        """Drop trailing whitespace."""
        return _StrMap("rstrip", self)

    # The expression forms of the eager ``Series.str`` methods: the same
    # names, the same values, nulls and types (ASCII; a byte outside ASCII is
    # left as it is by the case forms and fails the character classes).
    def capitalize(self) -> Expr:
        """First letter upper, the rest lower."""
        return _StrMap("capitalize", self)

    def title(self) -> Expr:
        """Each run of letters starts upper, the rest lower."""
        return _StrMap("title", self)

    def swapcase(self) -> Expr:
        """Every letter's case flipped."""
        return _StrMap("swapcase", self)

    def casefold(self) -> Expr:
        """ASCII casefold (the lowercase form)."""
        return _StrMap("lower", self)

    def isalnum(self) -> Expr:
        """Bool: non-empty and all letters or digits."""
        return _StrFn("isalnum", self)

    def isalpha(self) -> Expr:
        """Bool: non-empty and all letters."""
        return _StrFn("isalpha", self)

    def isdigit(self) -> Expr:
        """Bool: non-empty and all digits."""
        return _StrFn("isdigit", self)

    def isdecimal(self) -> Expr:
        """Bool: as :meth:`isdigit` over ASCII."""
        return _StrFn("isdecimal", self)

    def isnumeric(self) -> Expr:
        """Bool: as :meth:`isdigit` over ASCII."""
        return _StrFn("isnumeric", self)

    def isspace(self) -> Expr:
        """Bool: non-empty and all whitespace."""
        return _StrFn("isspace", self)

    def islower(self) -> Expr:
        """Bool: has a letter and every letter is lower."""
        return _StrFn("islower", self)

    def isupper(self) -> Expr:
        """Bool: has a letter and every letter is upper."""
        return _StrFn("isupper", self)

    def istitle(self) -> Expr:
        """Bool: title case, as Python ``str.istitle``."""
        return _StrFn("istitle", self)

    def pad_start(self, width: int, fill: str = " ") -> Expr:
        """Left-pad to ``width`` bytes with ``fill``."""
        return _StrFn("pad_start", self, text=_fill_char(fill), i0=int(width))

    def pad_end(self, width: int, fill: str = " ") -> Expr:
        """Right-pad to ``width`` bytes with ``fill``."""
        return _StrFn("pad_end", self, text=_fill_char(fill), i0=int(width))

    def pad(self, width: int, side: Literal["left", "right"] = "left", fillchar: str = " ") -> Expr:
        """Pad on ``side`` (pandas ``str.pad``)."""
        if side == "left":
            return self.pad_start(width, fillchar)
        if side == "right":
            return self.pad_end(width, fillchar)
        raise ValueError("side must be 'left' or 'right'")

    def ljust(self, width: int, fillchar: str = " ") -> Expr:
        """As :meth:`pad_end`."""
        return self.pad_end(width, fillchar)

    def rjust(self, width: int, fillchar: str = " ") -> Expr:
        """As :meth:`pad_start`."""
        return self.pad_start(width, fillchar)

    def center(self, width: int, fillchar: str = " ") -> Expr:
        """Center in ``width`` bytes, Python's ``str.center`` placement."""
        return _StrFn("center", self, text=_fill_char(fillchar), i0=int(width))

    def zfill(self, width: int) -> Expr:
        """Left-pad with ``0`` to ``width`` after an optional sign."""
        return _StrFn("zfill", self, i0=int(width))

    def removeprefix(self, prefix: str) -> Expr:
        """Drop a leading ``prefix`` when present."""
        return _StrFn("removeprefix", self, text=prefix)

    def removesuffix(self, suffix: str) -> Expr:
        """Drop a trailing ``suffix`` when present."""
        return _StrFn("removesuffix", self, text=suffix)

    def repeat(self, repeats: int) -> Expr:
        """Each value repeated ``repeats`` times."""
        return _StrFn("repeat", self, i0=int(repeats))

    def slice_replace(self, start: int = 0, stop: int | None = None, repl: str = "") -> Expr:
        """Bytes ``[start, stop)`` of each value replaced with ``repl``."""
        return _StrFn(
            "slice_replace",
            self,
            text=repl,
            i0=int(start),
            i1=_STR_NO_STOP if stop is None else int(stop),
        )

    def split(self, pat: str) -> Expr:
        """Split on the literal ``pat``: a ``list<string>`` (no ``expand``;
        take the pieces with :meth:`get`)."""
        return _StrFn("split", self, text=pat)

    def rsplit(self, pat: str) -> Expr:
        """As :meth:`split` (every occurrence, so the same parts)."""
        return _StrFn("split", self, text=pat)

    def partition(self, sep: str = " ") -> Expr:
        """Split at the first ``sep`` into a 3-element ``list<string>``
        (head, sep, tail); take the pieces with :meth:`get`."""
        return _StrFn("partition", self, text=sep)

    def rpartition(self, sep: str = " ") -> Expr:
        """As :meth:`partition`, at the last ``sep``."""
        return _StrFn("rpartition", self, text=sep)

    def extract(self, pat: str, group: int = 1) -> Expr:
        """The capture ``group`` of the first regex ``pat`` match, null where
        there is none."""
        return _StrFn("extract", self, text=pat, i0=int(group))

    def regex_replace(self, pat: str, to: str) -> Expr:
        """Replace every regex ``pat`` match with ``to`` (``$n``, ``${name}``,
        ``$$``); an invalid pattern or replacement raises at evaluation."""
        return _StrFn("regex_replace", self, text=pat, text2=to)

    def findall(self, pat: str) -> Expr:
        """Every regex match as a ``list<string>``."""
        return _StrFn("findall", self, text=pat)

    def normalize(self, form: NormForm) -> Expr:
        """Each value in the Unicode normalization ``form``; null for a value
        that is not valid UTF-8."""
        return _StrFn("normalize", self, i0=norm_form_code(form))

    def translate(self, table: Mapping[int | str, str | int | None]) -> Expr:
        """Each codepoint replaced by ``table``, as ``str.translate``: a key is
        a codepoint or one character, a value a string, a codepoint or ``None``
        to delete it."""
        keys, values = translate_pairs(table)
        if any(v is not None and len(v) + 1 >= 0xD800 for v in values):
            raise ValueError("translate value is too long")
        # StrFn::Translate: text = keys, text2 = per key chr(1 + len) + value,
        # or chr(0) to delete it.
        text2 = "".join(chr(0) if v is None else chr(len(v) + 1) + v for v in values)
        return _StrFn("translate", self, text="".join(keys), text2=text2)

    def wrap(
        self,
        width: int,
        expand_tabs: bool = True,
        replace_whitespace: bool = True,
        drop_whitespace: bool = True,
        break_long_words: bool = True,
        break_on_hyphens: bool = True,
    ) -> Expr:
        """Each value wrapped to lines of at most ``width`` codepoints, joined
        by newlines (``textwrap.TextWrapper`` of CPython 3.13.10, 3.14.1 and later with these
        options)."""
        if width < 1:
            raise ValueError(f"invalid width {width!r} (must be > 0)")
        flags = wrap_flag_bits(
            expand_tabs, replace_whitespace, drop_whitespace, break_long_words, break_on_hyphens
        )
        return _StrFn("wrap", self, i0=int(width), i1=flags)

    def match(self, pat: str) -> Expr:
        """Regex anchored at the start (pandas ``str.match``)."""
        return _StrPred("regex", self, "^(?:" + pat + ")")

    def rfind(self, sub: str) -> Expr:
        """Byte index of the last ``sub``, or -1 (Int64)."""
        return _StrFn("rfind", self, text=sub)

    def index(self, sub: str) -> Expr:
        """As :meth:`find`, but a plan run over a row without ``sub`` raises
        ``ValueError``, as Python's ``str.index``."""
        return _StrFn("index", self, text=sub)

    def rindex(self, sub: str) -> Expr:
        """As :meth:`rfind`, raising like :meth:`index`."""
        return _StrFn("rindex", self, text=sub)

    def join(self, sep: str) -> Expr:
        """Each ``list<string>`` value joined with ``sep``."""
        return _StrFn("join", self, text=sep)

    def get(self, i: int) -> Expr:
        """A list's element ``i`` (negative counts from the end), or a
        string's byte ``i`` (non-negative; null past the end)."""
        return _StrFn("get", self, i0=int(i))

    def cat(self, others: Expr | None = None, sep: str = "") -> Expr:
        """Row-wise concatenation with the column ``others``, ``sep`` between.
        The one-column form joins the whole column into one string, which is
        an aggregate: use ``Series.str.cat``."""
        if others is None:
            raise NotImplementedError(
                "cat() without others joins the whole column; use Series.str.cat"
            )
        if not isinstance(others, Expr):
            raise TypeError("cat needs another column expression (use lit() for a constant)")
        return _StrFn("cat", self, other=others, text=sep)

    def get_dummies(self, sep: str = "|") -> Expr:
        """Not an expression: it makes one column per token, which a
        column expression cannot. Use ``Series.str.get_dummies``."""
        raise NotImplementedError(
            "get_dummies makes a column per token, so it has no expression form; "
            "use Series.str.get_dummies"
        )

    def len_bytes(self) -> Expr:
        """Per-row byte length (Int64)."""
        return _StrLen(self, chars=False)

    def len_chars(self) -> Expr:
        """Per-row UTF-8 codepoint count (Int64)."""
        return _StrLen(self, chars=True)

    def find(self, needle: str) -> Expr:
        """Byte index of the first ``needle`` (Int64), null where it is absent
        (Polars ``str.find``); ``Series.str.find`` gives -1, as pandas."""
        return _StrFind(self, needle, True)

    def replace(self, old: str, new: str) -> Expr:
        """Replace the first ``old`` with ``new`` in each value. Takes ``str``
        only; use :meth:`replace_all` for expression arguments."""
        if isinstance(old, Expr) or isinstance(new, Expr):
            raise TypeError("replace takes str arguments; use replace_all for columns")
        return _StrReplace(self, old, new, all=False)

    def replace_all(self, old: str | Expr, new: str | Expr) -> Expr:
        """Replace every ``old`` with ``new`` in each value. Either may be an
        expression (a ``str`` beside one is a literal); null where any operand
        is null, the value unchanged for an empty ``old``."""
        if isinstance(old, Expr) or isinstance(new, Expr):
            return _StrReplaceCol(
                self,
                old if isinstance(old, Expr) else lit(old),
                new if isinstance(new, Expr) else lit(new),
            )
        return _StrReplace(self, old, new, all=True)

    def slice(self, start: int, length: int) -> Expr:
        """The byte substring ``[start, start + length)`` of each value."""
        return _StrSlice(self, start, length)

    def quantile(
        self,
        quantile: float,
        interpolation: Literal["linear", "nearest", "lower", "higher", "midpoint"] = "nearest",
    ) -> Agg:
        """The exact quantile at level ``quantile`` in [0, 1], per group (an
        :class:`Agg`); null for a group with no present value. ``interpolation``
        picks the result between two ranks, ``"nearest"`` as in Polars. The
        group state keeps every present value (8 bytes each), spilled across
        groups under the memory budget; :meth:`approx_quantile` is the bounded
        memory sketch."""
        check_quantile(quantile, interpolation)
        return Agg("quantile", self, param=float(quantile), interpolation=interpolation)

    def median(self) -> Agg:
        """The exact median per group (an :class:`Agg`): :meth:`quantile` at
        0.5, whose two interpolations of the middle ranks agree."""
        return self.quantile(0.5, "linear")

    def n_unique(self) -> Agg:
        """The number of distinct present values per group (an :class:`Agg`):
        :meth:`distinct`, a KMV sketch that is exact below 1024 values."""
        return self.distinct()

    def len(self) -> Agg:
        """The row count per group, nulls included (an :class:`Agg`)."""
        return Agg("count", self)

    def null_count(self) -> Agg:
        """The number of null values per group (an :class:`Agg`)."""
        name = f"null_count_{self.name}" if isinstance(self, Col) else "null_count"
        return Agg("sum", self.is_null().cast("int64"), name)

    def approx_quantile(self, q: float) -> Agg:
        """An approximate quantile at level ``q`` in [0, 1], per group, from a
        DDSketch: mergeable in bounded memory, exact only to the sketch's
        relative accuracy."""
        if not 0.0 <= q <= 1.0:
            raise ValueError(f"approx_quantile: q must be in [0, 1], not {q!r}")
        return Agg("pct", self, param=q)

    def sketch_quantile(self, q: float) -> Agg:
        """The ``q`` quantile in [0, 1] of the merge of a group's stored
        DDSketches (base64 text, such as ``dur.sketch``), clamped to the
        merged minimum and maximum. Null cells, text that is not a sketch and
        sketches of another accuracy than the default are skipped; a group
        with none left is null."""
        if not 0.0 <= q <= 1.0:
            raise ValueError(f"sketch_quantile: q must be in [0, 1], not {q!r}")
        return Agg("sketch_quantile", self, param=q)

    def percentile(
        self,
        p: float,
        interpolation: Literal["linear", "nearest", "lower", "higher", "midpoint"] = "linear",
    ) -> Agg:
        """The exact percentile ``p`` in [0, 100], per group (numpy-style,
        linear interpolation by default); ``F.dur.percentile(99)`` is
        ``F.dur.quantile(0.99, "linear")``."""
        if not 0.0 <= p <= 100.0:
            raise ValueError(f"percentile: p must be in [0, 100], not {p!r}")
        check_quantile(p / 100.0, interpolation)
        return Agg("quantile", self, param=p / 100.0, interpolation=interpolation)

    def busy(self, resolution: int | float | str | None = None) -> Agg:
        """Trace occupancy (``TraceViewer.agg`` only): the time at least one
        event was active. ``resolution`` snaps interval edges to that grid (a
        number is microseconds, a string such as ``"1ms"`` is converted);
        unset is the exact union."""
        return _occupancy("busy", self, resolution)

    def concurrency(self, resolution: int | float | str | None = None) -> Agg:
        """Trace occupancy: average parallelism, summed duration over
        :meth:`busy` time; see :meth:`busy` for ``resolution``."""
        return _occupancy("concurrency", self, resolution)

    def utilization(self, resolution: int | float | str | None = None) -> Agg:
        """Trace occupancy: :meth:`busy` time over the makespan; see
        :meth:`busy` for ``resolution``."""
        return _occupancy("utilization", self, resolution)

    def active(self, resolution: int | float | str | None = None) -> Agg:
        """Trace occupancy: the peak number of events active at once; see
        :meth:`busy` for ``resolution``."""
        return _occupancy("active", self, resolution)

    def set_union(self, typed: bool = False) -> Agg:
        """The distinct values of this expression per group. By default one
        text cell: the values sorted as text and joined with ``\\x1e``; a string
        holding that byte is refused. With ``typed=True`` a list column of the
        values in their own type (``string``, ``bool``, ``int64``, ``uint64`` or
        ``float64``), ascending (numbers by value), nulls dropped, an empty
        group an empty list, and no limit on the bytes of a string."""
        return Agg("set_union", self, param=1.0 if typed else 0.0)

    def hist(self) -> Agg:
        """The DDSketch histogram per group: a ``list<struct{lo, hi, count}>``
        column (mergeable, relative-error buckets)."""
        return Agg("hist", self)

    def argmax(self, by: Expr) -> Agg:
        """The String repr of this expression at the row maximizing ``by``,
        per group (an :class:`Agg`)."""
        return Agg("argmax", self, by=by)

    def argmin(self, by: Expr) -> Agg:
        """The String repr of this expression at the row minimizing ``by``,
        per group. Several ``argmin``/``argmax`` aggregates naming the same
        ``by`` all report the same winning row."""
        return Agg("argmin", self, by=by)

    def arg_max(self) -> Agg:
        """The position within its group (from 0, in input order) of the
        first greatest present value, per group (an :class:`Agg`): null for a
        group with no present value, a NaN wins only in a group with no number
        (then the first NaN), and a tie keeps the first row, as polars. Integers and floats are compared exactly in
        their own type; a text column raises. Over the whole frame (no group)
        it is the row position. ``argmax(by)`` is the other form: the repr of
        this expression at the row maximizing ``by``."""
        return Agg("arg_max", self)

    def arg_min(self) -> Agg:
        """As :meth:`arg_max` for the least present value."""
        return Agg("arg_min", self)

    def distinct(self, k: int = 0) -> Agg:
        """Approximate distinct count per group (a KMV sketch of ``k`` hashes,
        1024 by default), exact below ``k`` distinct values."""
        return Agg("distinct", self, param=float(k))

    def list_sorted(self, by: Expr) -> Agg:
        """This expression's reprs per group as a ``list<string>``, ordered by
        ``by`` ascending with the repr breaking ties."""
        return Agg("list_sorted", self, by=by)

    def topk(self, by: Expr, k: int = 0) -> Agg:
        """The reprs at the ``k`` largest ``by`` values per group (8 by
        default), a ``list<string>`` in descending ``by`` order."""
        return Agg("topk", self, param=float(k), by=by)

    def bottomk(self, by: Expr, k: int = 0) -> Agg:
        """The reprs at the ``k`` smallest ``by`` values per group (8 by
        default), a ``list<string>`` in ascending ``by`` order."""
        return Agg("bottomk", self, param=float(k), by=by)

    def approx_topk(self, k: int = 0) -> Agg:
        """Approximate heavy hitters per group (SpaceSaving with ``k``
        counters, 8 by default): a ``list<struct{value, count}>`` column by
        descending count."""
        return Agg("approx_topk", self, param=float(k))

    def sample(self, k: int = 0) -> Agg:
        """A deterministic, mergeable bottom-``k``-by-hash sample of this
        expression's distinct reprs per group (8 by default), a sorted
        ``list<string>``."""
        return Agg("sample", self, param=float(k))

    def corr(self, by: Expr) -> Agg:
        """Pearson correlation of this expression against ``by`` per group."""
        return Agg("corr", self, by=by)

    def covar_pop(self, by: Expr) -> Agg:
        """Population covariance of this expression against ``by`` per group."""
        return Agg("covar_pop", self, by=by)

    def covar_samp(self, by: Expr) -> Agg:
        """Sample covariance of this expression against ``by`` per group."""
        return Agg("covar_samp", self, by=by)

    def regr_slope(self, by: Expr) -> Agg:
        """Least-squares slope of this expression on ``by`` per group."""
        return Agg("regr_slope", self, by=by)

    def regr_intercept(self, by: Expr) -> Agg:
        """Least-squares intercept of this expression on ``by`` per group."""
        return Agg("regr_intercept", self, by=by)

    def regr_r2(self, by: Expr) -> Agg:
        """Coefficient of determination of this expression on ``by`` per
        group."""
        return Agg("regr_r2", self, by=by)

    if TYPE_CHECKING:
        # Prims/unary math (dispatched via __getattr__) and aggs (installed by
        # setattr below), declared so consumers get precise types not Any.
        # BEGIN GENERATED from api.def by scripts/gen_pyi.py
        def cum_prod(self) -> Expr: ...
        def cum_count(self) -> Expr: ...
        def diff(self) -> Expr: ...
        def pct_change(self) -> Expr: ...
        def sin(self) -> Expr: ...
        def cos(self) -> Expr: ...
        def tan(self) -> Expr: ...
        def cot(self) -> Expr: ...
        def arcsin(self) -> Expr: ...
        def arccos(self) -> Expr: ...
        def arctan(self) -> Expr: ...
        def sinh(self) -> Expr: ...
        def cosh(self) -> Expr: ...
        def tanh(self) -> Expr: ...
        def arcsinh(self) -> Expr: ...
        def arccosh(self) -> Expr: ...
        def arctanh(self) -> Expr: ...
        def cbrt(self) -> Expr: ...
        def log10(self) -> Expr: ...
        def erf(self) -> Expr: ...
        def erfc(self) -> Expr: ...
        def bitwise_count_ones(self) -> Expr: ...
        def bitwise_count_zeros(self) -> Expr: ...
        def bitwise_leading_ones(self) -> Expr: ...
        def bitwise_leading_zeros(self) -> Expr: ...
        def bitwise_trailing_ones(self) -> Expr: ...
        def bitwise_trailing_zeros(self) -> Expr: ...
        def is_nan(self) -> Expr: ...
        def is_finite(self) -> Expr: ...
        def is_infinite(self) -> Expr: ...
        def is_unique(self) -> Expr: ...
        def is_duplicated(self) -> Expr: ...
        def drop_nulls(self) -> Expr: ...
        def drop_nans(self) -> Expr: ...
        def reverse(self) -> Expr: ...
        def interpolate(self) -> Expr: ...
        # END GENERATED
        def ilog2(self) -> Expr: ...
        def bit_width(self) -> Expr: ...
        def popcount(self) -> Expr: ...
        def clz(self) -> Expr: ...
        def ctz(self) -> Expr: ...
        def mix64(self) -> Expr: ...
        def abs(self) -> Expr: ...
        def round(self) -> Expr: ...
        def floor(self) -> Expr: ...
        def ceil(self) -> Expr: ...
        def log(self) -> Expr: ...
        def sqrt(self) -> Expr: ...
        def exp(self) -> Expr: ...
        def log1p(self) -> Expr: ...
        def expm1(self) -> Expr: ...
        def sign(self) -> Expr: ...
        def negate(self) -> Expr: ...
        def trunc(self) -> Expr: ...
        def sum(self) -> Agg: ...
        def min(self) -> Agg: ...
        def max(self) -> Agg: ...
        def mean(self) -> Agg: ...
        def var(self) -> Agg: ...
        def std(self) -> Agg: ...
        def skew(self, bias: bool = True) -> Agg: ...
        def kurt(self, bias: bool = True) -> Agg: ...
        def first(self) -> Agg: ...
        def last(self) -> Agg: ...
        def sumsq(self) -> Agg: ...
        def bitwise_and(self) -> Agg: ...
        def bitwise_or(self) -> Agg: ...
        def bitwise_xor(self) -> Agg: ...
        def prod(self) -> Agg: ...

    # Runtime prim-method seam; hidden from the checker so unknown attributes
    # are type errors, not Any (the real methods are declared above).
    if not TYPE_CHECKING:

        def __getattr__(self, name: builtins.str) -> Callable[[], Expr]:
            # F.dur.ilog2()/F.hhash.mix64() - int bit primitive; F.dur.floor()/
            # .log()/... - FP unary math. Both are unary methods.
            if name in PRIM_CODES:
                return lambda: Prim(name, self)
            if name in _UNARY_CODES:
                return lambda: Unary(name, self)
            raise AttributeError(name)

    # BEGIN GENERATED aliases from _aliases.py by scripts/gen_pyi.py
    if TYPE_CHECKING:
        isnull = isna
        notnull = notna
        isin = is_in
        astype = cast
        cumsum = cum_sum
        cumprod = cum_prod
        cummax = cum_max
        cummin = cum_min
        ffill = forward_fill
        bfill = backward_fill
        startswith = starts_with
        endswith = ends_with
        pass
    # END GENERATED aliases


def _expr_method(name: str, op: str, doc: str) -> Callable[[Expr], Expr]:
    def method(self: Expr) -> Expr:
        return self._op(op)

    method.__name__ = name
    method.__qualname__ = f"Expr.{name}"
    method.__doc__ = doc
    return method


def _install_api_methods() -> None:
    """The Expr methods that are one registry op each: the rows of the native
    API table (api.def) with ``on_expr``, so the table is their only source."""
    for cls, name, op, args, _result, _rule, on_expr, _keywords, _err, doc in _ext._api_rows():
        if on_expr:
            if cls != "Series" or args != "NONE":
                raise RuntimeError(f"api.def row {cls}.{name}: Expr takes only no-argument rows")
            setattr(Expr, name, _expr_method(name, op, doc))


_install_api_methods()


_install_aliases("Expr", Expr)


def wrap(x: object) -> Expr:
    if isinstance(x, Expr):
        return x
    if isinstance(x, Agg):
        return AggLeaf(x)
    if isinstance(x, bool) or not isinstance(x, (int, float)):
        raise TypeError(f"expected a column expr or a number, got {x!r}")
    return Lit(x)


class Col(Expr):
    """A field / column reference. Its name drives both column lookup for
    :meth:`Expr.apply` and the field name in a pushed-down predicate."""

    def __init__(self, name: str) -> None:
        self.name = name

    def any(self) -> _AnyCol:
        """The same field as ``any(name)``: a predicate on it holds when any
        element of the array holds. It filters in the trace scan
        (``TraceViewer.duql``); ``apply`` cannot evaluate it."""
        return _AnyCol(f"any({self.name})")


class _AnyCol(Col):
    """``any(name)``: renders to duql; no column to evaluate."""


class Named:
    """An expression under an output name: ``col("a").alias("b")``. What
    ``select`` and ``with_columns`` take beside plain names."""

    __slots__ = ("name", "expr")

    def __init__(self, name: str, expr: Expr) -> None:
        self.name = name
        self.expr = expr


#: One item of a ``select``: a column name, a bare column expression (its own
#: name), or a named expression.
Selector = Union[str, "Expr", Named]
#: An argument of ``select``: a selector, an aggregate, or a list of selectors.
SelectItem = Union[Selector, "Agg", Sequence[Selector]]


def resolve_selectors(
    items: Sequence[SelectItem],
) -> list[tuple[str, Expr | None]]:
    """Flatten ``select`` arguments to ``(output name, expression or None)``
    pairs: a name or a bare column keeps itself (no expression to compute), a
    named expression computes under its name. A computed expression without
    a name is refused: the engine names every column."""
    out: list[tuple[str, Expr | None]] = []
    for item in items:
        if isinstance(item, str):
            out.append((item, None))
        elif isinstance(item, Named):
            out.append((item.name, item.expr))
        elif isinstance(item, Col):
            out.append((item.name, None))
        elif isinstance(item, Expr):
            raise TypeError("select: a computed expression needs .alias(name)")
        elif isinstance(item, (list, tuple)):
            out.extend(resolve_selectors(item))
        else:
            raise TypeError(f"select: takes names or expressions, got {type(item).__name__}")
    return out


class Lit(Expr):
    def __init__(self, value: Scalar | str) -> None:
        self.value = value


class Bin(Expr):
    def __init__(self, op: str, left: Expr, right: Expr) -> None:
        self.op = op
        self.left = left
        self.right = right


class Prim(Expr):
    def __init__(self, name: str, arg: Expr) -> None:
        self.name = name
        self.arg = arg


class Unary(Expr):
    def __init__(self, name: str, arg: Expr) -> None:
        self.name = name
        self.arg = arg


class Clip(Expr):
    def __init__(self, arg: Expr, lo: int | float, hi: int | float) -> None:
        self.arg = arg
        self.lo = lo
        self.hi = hi


class Cast(Expr):
    def __init__(self, type_id: int, arg: Expr) -> None:
        self.type_id = type_id
        self.arg = arg


class Fillna(Expr):
    def __init__(self, arg: Expr, fill: int | float | builtins.str, nan: bool = False) -> None:
        self.arg = arg
        self.fill = fill
        # Also replace the NaN of a float column (pandas fillna).
        self.nan = nan


class Cmp(Expr):
    def __init__(self, op: str, left: Expr, rhs: object) -> None:
        if isinstance(rhs, Lit):
            rhs = rhs.value
        self.op = op
        self.left = left
        self.rhs = rhs

    def _dsl(self) -> str:
        if not isinstance(self.left, Col) or isinstance(self.rhs, Expr):
            raise TypeError(_NOT_PUSHABLE)
        return f"{self.left.name} {_CMP_SYMBOL[self.op]} {_format_value(self.rhs)}"


class Logical(Expr):
    def __init__(self, op: str, left: Expr, right: Expr | None) -> None:
        self.op = op
        self.left = left
        self.right = right

    def _dsl(self) -> str:
        if self.op == "not" or self.right is None:
            return f"not ({self.left._dsl()})"
        return f"({self.left._dsl()} {self.op} {self.right._dsl()})"


def _field_name(arg: Expr) -> str:
    if not isinstance(arg, Col):
        raise TypeError(_NOT_PUSHABLE)
    return arg.name


def _operand(arg: Expr) -> str:
    if isinstance(arg, Col):
        return arg.name
    if isinstance(arg, Lit):
        return _format_value(arg.value)
    raise TypeError(_NOT_PUSHABLE)


def _escape_like(s: str) -> str:
    out = []
    for ch in s:
        if ch in "%_\\":
            out.append("\\")
        out.append(ch)
    return "".join(out)


class _StrPred(Expr):
    """``arg <op> pattern`` for op in _STR_PRED_CODES: a Bool mask through the
    engine's string kernels. On a bare field it also serializes to
    duql: contains / starts_with / ends_with as an escaped LIKE, like as LIKE,
    regex as ``~``; fullmatch has no duql form."""

    def __init__(self, op: str, arg: Expr, pattern: str) -> None:
        self.op = op
        self.arg = arg
        self.pattern = pattern

    def _dsl(self) -> str:
        field = _field_name(self.arg)
        p = self.pattern
        if self.op in ("contains", "starts_with", "ends_with"):
            body = _escape_like(p)
            head = "" if self.op == "starts_with" else "%"
            tail = "" if self.op == "ends_with" else "%"
            text = f"{field} like {_format_value(head + body + tail)}"
            # A duql like pattern has no escape character unless it names one.
            return text if body == p else text + " escape " + _format_value("\\")
        if self.op == "like":
            return f"{field} like {_format_value(p)}"
        if self.op == "regex":
            return f"{field} ~ {_format_value(p)}"
        raise TypeError(_NOT_PUSHABLE)


class _StrPredCol(Expr):
    def __init__(self, op: str, arg: Expr, needle: Expr) -> None:
        self.op = op
        self.arg = arg
        self.needle = needle

    def _dsl(self) -> str:
        return f"{self.op}({_operand(self.arg)}, {_operand(self.needle)})"


class _StrReplaceCol(Expr):
    def __init__(self, arg: Expr, old: Expr, new: Expr) -> None:
        self.arg = arg
        self.old = old
        self.new = new

    def _dsl(self) -> str:
        return f"replace({_operand(self.arg)}, {_operand(self.old)}, {_operand(self.new)})"


class _IContains(Expr):
    """Case-insensitive substring: ``"sub" in field`` in the DSL, and
    ``lower(arg) contains lower(sub)`` in memory."""

    def __init__(self, arg: Expr, needle: str) -> None:
        self.arg = arg
        self.needle = needle

    def _dsl(self) -> str:
        return f"{_format_value(self.needle)} in {_field_name(self.arg)}"

    def _lowered(self) -> Expr:
        return _StrPred("contains", _StrMap("lower", self.arg), self.needle.lower())


class _ILike(Expr):
    """Case-insensitive LIKE: ``field ilike p`` in the DSL, and
    ``lower(arg) like lower(p)`` in memory."""

    def __init__(self, arg: Expr, pattern: str) -> None:
        self.arg = arg
        self.pattern = pattern

    def _dsl(self) -> str:
        return f"{_field_name(self.arg)} ilike {_format_value(self.pattern)}"

    def _lowered(self) -> Expr:
        return _StrPred("like", _StrMap("lower", self.arg), self.pattern.lower())


class _IRegex(Expr):
    """Case-insensitive regex search: ``field ~* p``. Filter-only."""

    def __init__(self, arg: Expr, pattern: str) -> None:
        self.arg = arg
        self.pattern = pattern

    def _dsl(self) -> str:
        return f"{_field_name(self.arg)} ~* {_format_value(self.pattern)}"


class _IsIn(Expr):
    """``arg in [...]`` / ``arg not in [...]``: a Bool mask in memory, and on a
    bare field the DSL membership form."""

    def __init__(self, arg: Expr, values: Sequence[Value], negate: bool) -> None:
        vals = list(values)
        if any(isinstance(v, bool) for v in vals):
            raise TypeError("is_in values must be ints or strings, not bools")
        if vals and not (
            all(isinstance(v, int) for v in vals) or all(isinstance(v, str) for v in vals)
        ):
            raise TypeError("is_in values must be all ints or all strings")
        self.arg = arg
        self.values = vals
        self.negated = negate

    def _dsl(self) -> str:
        items = ", ".join(_format_value(v) for v in self.values)
        kw = "not in" if self.negated else "in"
        return f"{_field_name(self.arg)} {kw} [{items}]"


class _StrMap(Expr):
    def __init__(self, op: str, arg: Expr) -> None:
        self.op = op
        self.arg = arg


def _fill_char(fill: str) -> str:
    if len(fill.encode()) != 1:
        raise ValueError("the fill character must be one ASCII character")
    return fill


class _StrFn(Expr):
    """A string method of the ``_STR_FN_CODES`` family: ``arg`` (and for
    ``cat`` the column ``other``) plus up to two strings and two integers."""

    def __init__(
        self,
        name: str,
        arg: Expr,
        other: Expr | None = None,
        text: str = "",
        text2: str = "",
        i0: int = 0,
        i1: int = 0,
    ) -> None:
        self.name = name
        self.arg = arg
        self.other = other
        self.text = text
        self.text2 = text2
        self.i0 = i0
        self.i1 = i1


class _StrLen(Expr):
    def __init__(self, arg: Expr, chars: bool) -> None:
        self.arg = arg
        self.chars = chars


class _StrFind(Expr):
    def __init__(self, arg: Expr, needle: str, null_on_miss: bool = False) -> None:
        self.arg = arg
        self.needle = needle
        self.null_on_miss = null_on_miss


class _StrReplace(Expr):
    def __init__(self, arg: Expr, old: str, new: str, all: bool) -> None:
        self.arg = arg
        self.old = old
        self.new = new
        self.all = all


class _StrSlice(Expr):
    def __init__(self, arg: Expr, start: int, length: int) -> None:
        self.arg = arg
        self.start = start
        self.length = length


def as_expr(x: object) -> Expr:
    if isinstance(x, Expr):
        return x
    raise TypeError("& / | combine expressions, not plain values")


_DUQL_ESCAPES = {"\\": "\\\\", '"': '\\"', "\n": "\\n", "\t": "\\t", "\r": "\\r"}


def duql_string(v: str) -> str:
    r"""``v`` as a duql string literal that reads back as exactly ``v``: double
    quotes, with ``\\``, ``\"``, ``\n``, ``\t``, ``\r`` and ``\u00XX`` for the
    other control characters."""
    out = []
    for ch in v:
        esc = _DUQL_ESCAPES.get(ch)
        if esc is not None:
            out.append(esc)
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append(f"\\u{ord(ch):04x}")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def _format_value(v: object) -> str:
    if isinstance(v, str):
        return duql_string(v)
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


def col(name: str) -> Expr:
    """A column / field reference by name."""
    return Col(name)


# typing.get_type_hints evaluates the annotation, and Python 3.9 has no `|` on
# a Union alias.
def lit(value: Union[Scalar, str]) -> Expr:  # noqa: UP007
    """A scalar literal (kept distinct from a column name, Polars-style)."""
    return Lit(value)


class AggLeaf(Expr):
    """An aggregate standing in a row or group expression (``g["v"] -
    g["v"].mean()``, ``g["v"].sum() > 10``): the group-by evaluates the
    aggregate first, then :func:`split_aggs` rewrites the leaf into a column
    reference over the joined result."""

    def __init__(self, agg: Agg) -> None:
        self.agg = agg


class ColumnOpNode(Expr):
    """A registered column op over ``arg`` (see ``_expr_ops``)."""

    def __init__(self, arg: Expr, spec: ColumnOp) -> None:
        self.arg = arg
        self.spec = spec


def _no_expand(name: str, expand: bool) -> None:
    if expand:
        raise NotImplementedError(
            f"{name}(expand=True) makes a column per piece, which an expression cannot; "
            "use Series.str or take the pieces with .get(i)"
        )


class _ExprStr:
    """``col.str``: the string ops as expression nodes where the evaluator
    has them, column ops otherwise."""

    __slots__ = ("_e",)

    def __init__(self, e: Expr) -> None:
        self._e = e

    def contains(
        self,
        pat: str | Expr,
        case: bool = True,
        regex: bool = True,
        *,
        literal: bool | None = None,
    ) -> Expr:
        """A regex search by default (the pandas and Polars default), the
        substring predicate with ``regex=False`` or ``literal=True``; the
        keywords of ``Series.str.contains``. An expression ``pat`` is always a
        substring test."""
        if literal is not None:
            regex = not literal
        if regex and not isinstance(pat, Expr):
            return self._e.regex(pat if case else "(?i)" + pat)
        return self._e.contains(pat, case)

    def starts_with(self, prefix: str | Expr) -> Expr:
        return self._e.starts_with(prefix)

    def ends_with(self, suffix: str | Expr) -> Expr:
        return self._e.ends_with(suffix)

    def to_lowercase(self) -> Expr:
        return self._e.lower()

    def to_uppercase(self) -> Expr:
        return self._e.upper()

    def strip_chars(self) -> Expr:
        return self._e.strip()

    def strip_chars_start(self) -> Expr:
        return self._e.lstrip()

    def strip_chars_end(self) -> Expr:
        return self._e.rstrip()

    def len_bytes(self) -> Expr:
        return self._e.len_bytes()

    def len_chars(self) -> Expr:
        return self._e.len_chars()

    def find(self, sub: str) -> Expr:
        return self._e.find(sub)

    def replace(self, pat: str, repl: str, n: int = -1) -> Expr:
        """Literal replace: every occurrence (``n=-1``) or the first
        (``n=1``), as ``Series.str.replace``."""
        if n == -1:
            return self._e.replace_all(pat, repl)
        if n == 1:
            return self._e.replace(pat, repl)
        raise NotImplementedError("replace() supports n=-1 (all) or n=1 (first)")

    def replace_all(self, pat: str | Expr, repl: str | Expr, *, literal: bool = False) -> Expr:
        """Replace every match of the regex ``pat`` (the Polars default;
        ``repl`` may use ``$n`` / ``${name}``) or, with ``literal=True`` or an
        expression argument, every occurrence of ``pat``."""
        if literal or isinstance(pat, Expr) or isinstance(repl, Expr):
            return self._e.replace_all(pat, repl)
        return self._e.regex_replace(pat, repl)

    def head(self, n: int) -> Expr:
        """The first ``n`` bytes of each value (polars ``head``)."""
        return self._e.slice(0, n)

    def tail(self, n: int) -> Expr:
        """The last ``n`` bytes of each value (polars ``tail``)."""
        return self._e.slice(-n, n)

    def slice(self, start: int = 0, stop: int | None = None) -> Expr:
        """Byte substring ``[start, stop)``, as ``Series.str.slice``."""
        return self._e.slice(start, -1 if stop is None else max(stop - start, 0))

    def pad_start(self, width: int, fill: str = " ") -> Expr:
        return self._e._op("dftu.series.str_pad_start", a=int(width), text=fill)

    def pad_end(self, width: int, fill: str = " ") -> Expr:
        return self._e._op("dftu.series.str_pad_end", a=int(width), text=fill)

    def zfill(self, width: int) -> Expr:
        return self._e._op("dftu.series.str_zfill", a=int(width))

    def count_matches(self, pat: str) -> Expr:
        return self._e._op("dftu.series.str_count", text=pat)

    def strip_prefix(self, prefix: str) -> Expr:
        return self._e._op("dftu.series.str_remove_prefix", text=prefix)

    def strip_suffix(self, suffix: str) -> Expr:
        return self._e._op("dftu.series.str_remove_suffix", text=suffix)

    def to_titlecase(self) -> Expr:
        return self._e._op("dftu.series.str_case", a=1)

    def regex_replace(self, pat: str, to: str) -> Expr:
        return self._e.regex_replace(pat, to)

    def extract(self, pat: str, group: int = 1) -> Expr:
        return self._e._op("dftu.series.str_extract", a=int(group), text=pat)

    # The pandas names of the methods above, and of the expression's own
    # string methods.
    def startswith(self, pat: str | Expr) -> Expr:
        return self.starts_with(pat)

    def endswith(self, pat: str | Expr) -> Expr:
        return self.ends_with(pat)

    def len(self) -> Expr:
        return self.len_chars()

    def isascii(self) -> Expr:
        return self._e.isascii()

    def encode(self, encoding: str = "utf-8", errors: str = "strict") -> Expr:
        return self._e.encode(encoding)

    def decode(self, encoding: str = "utf-8", errors: str = "null") -> Expr:
        """Each binary value read as UTF-8 text, null where it is not valid.
        ``errors="strict"`` (the ``Series.str.decode`` default) would raise at
        evaluation, which a lazy expression cannot do: it raises
        ``NotImplementedError``; the default here is ``"null"``."""
        if errors not in ("strict", "null"):
            raise ValueError(f"decode: errors must be 'strict' or 'null', not {errors!r}")
        if errors == "strict":
            raise NotImplementedError("decode(errors='strict') has no lazy form; use errors='null'")
        return self._e.decode(encoding)

    def capitalize(self) -> Expr:
        return self._e.capitalize()

    def casefold(self) -> Expr:
        return self._e.casefold()

    def cat(self, others: Expr | str | None = None, sep: str = "") -> Expr:
        if isinstance(others, str):
            others = lit(others)
        return self._e.cat(others, sep)

    def center(self, width: int, fillchar: str = " ") -> Expr:
        return self._e.center(width, fillchar)

    def count(self, pat: str) -> Expr:
        return self.count_matches(pat)

    def findall(self, pat: str) -> Expr:
        return self._e.findall(pat)

    def normalize(self, form: NormForm) -> Expr:
        return self._e.normalize(form)

    def translate(self, table: Mapping[int | str, str | int | None]) -> Expr:
        return self._e.translate(table)

    def wrap(
        self,
        width: int,
        expand_tabs: bool = True,
        replace_whitespace: bool = True,
        drop_whitespace: bool = True,
        break_long_words: bool = True,
        break_on_hyphens: bool = True,
    ) -> Expr:
        return self._e.wrap(
            width,
            expand_tabs,
            replace_whitespace,
            drop_whitespace,
            break_long_words,
            break_on_hyphens,
        )

    def fullmatch(self, pat: str) -> Expr:
        return self._e.fullmatch(pat)

    def get(self, i: int) -> Expr:
        return self._e.get(i)

    def get_dummies(self, sep: str = "|") -> Expr:
        return self._e.get_dummies(sep)

    def index(self, sub: str) -> Expr:
        return self._e.index(sub)

    def isalnum(self) -> Expr:
        return self._e.isalnum()

    def isalpha(self) -> Expr:
        return self._e.isalpha()

    def isdecimal(self) -> Expr:
        return self._e.isdecimal()

    def isdigit(self) -> Expr:
        return self._e.isdigit()

    def islower(self) -> Expr:
        return self._e.islower()

    def isnumeric(self) -> Expr:
        return self._e.isnumeric()

    def isspace(self) -> Expr:
        return self._e.isspace()

    def istitle(self) -> Expr:
        return self._e.istitle()

    def isupper(self) -> Expr:
        return self._e.isupper()

    def join(self, sep: str) -> Expr:
        return self._e.join(sep)

    def ljust(self, width: int, fillchar: str = " ") -> Expr:
        return self._e.ljust(width, fillchar)

    def lower(self) -> Expr:
        return self._e.lower()

    def lstrip(self) -> Expr:
        return self._e.lstrip()

    def match(self, pat: str) -> Expr:
        return self._e.match(pat)

    def pad(self, width: int, side: Literal["left", "right"] = "left", fillchar: str = " ") -> Expr:
        return self._e.pad(width, side, fillchar)

    def partition(self, sep: str = " ", expand: bool = False) -> Expr:
        """A 3-element list column. ``expand=True`` would be three columns,
        which an expression cannot make: it raises ``NotImplementedError``
        (``Series.str.partition`` expands by default)."""
        _no_expand("partition", expand)
        return self._e.partition(sep)

    def removeprefix(self, prefix: str) -> Expr:
        return self._e.removeprefix(prefix)

    def removesuffix(self, suffix: str) -> Expr:
        return self._e.removesuffix(suffix)

    def repeat(self, repeats: int) -> Expr:
        return self._e.repeat(repeats)

    def rfind(self, sub: str) -> Expr:
        return self._e.rfind(sub)

    def rindex(self, sub: str) -> Expr:
        return self._e.rindex(sub)

    def rjust(self, width: int, fillchar: str = " ") -> Expr:
        return self._e.rjust(width, fillchar)

    def rpartition(self, sep: str = " ", expand: bool = False) -> Expr:
        _no_expand("rpartition", expand)
        return self._e.rpartition(sep)

    def rsplit(self, pat: str, expand: bool = False) -> Expr:
        _no_expand("rsplit", expand)
        return self._e.rsplit(pat)

    def rstrip(self) -> Expr:
        return self._e.rstrip()

    def slice_replace(self, start: int = 0, stop: int | None = None, repl: str = "") -> Expr:
        return self._e.slice_replace(start, stop, repl)

    def split(self, pat: str, expand: bool = False) -> Expr:
        _no_expand("split", expand)
        return self._e.split(pat)

    def strip(self) -> Expr:
        return self._e.strip()

    def swapcase(self) -> Expr:
        return self._e.swapcase()

    def title(self) -> Expr:
        return self._e.title()

    def upper(self) -> Expr:
        return self._e.upper()


class _ExprDt:
    """``col.dt``: the calendar parts and bucket rounding as column ops."""

    __slots__ = ("_e", "_unit", "_given")

    def __init__(self, e: Expr, unit: str | None = None) -> None:
        self._e = e
        self._given = unit
        self._unit = unit or "us"

    def __call__(self, unit: str) -> _ExprDt:
        return _ExprDt(self._e, unit)

    def _part(self, code: int) -> Expr:
        return self._e._op("dftu.series.dt_part", a=code, b=DT_UNIT_CODES[self._unit])

    def strftime(self, fmt: str) -> Expr:
        return self._e._op("dftu.series.dt_format", a=DT_UNIT_CODES[self._unit], text=fmt)

    def year(self) -> Expr:
        return self._part(0)

    def month(self) -> Expr:
        return self._part(1)

    def day(self) -> Expr:
        return self._part(2)

    def hour(self) -> Expr:
        return self._part(3)

    def minute(self) -> Expr:
        return self._part(4)

    def second(self) -> Expr:
        return self._part(5)

    def millisecond(self) -> Expr:
        return self._part(6)

    def microsecond(self) -> Expr:
        return self._part(7)

    def nanosecond(self) -> Expr:
        return self._part(8)

    def weekday(self) -> Expr:
        """Monday 1 .. Sunday 7, as polars; ``dayofweek`` is the pandas
        count from 0."""
        return self._part(9) + 1

    def dayofweek(self) -> Expr:
        """Monday is 0, as the pandas property (``day_of_week`` likewise)."""
        return self._part(9)

    day_of_week = dayofweek

    def ordinal_day(self) -> Expr:
        return self._part(10)

    def dayofyear(self) -> Expr:
        """As :meth:`ordinal_day` (the pandas name, ``day_of_year`` likewise)."""
        return self._part(10)

    day_of_year = dayofyear

    def days_in_month(self) -> Expr:
        return self._part(13)

    daysinmonth = days_in_month

    def is_month_start(self) -> Expr:
        return self._part(2) == 1

    def is_month_end(self) -> Expr:
        return (self._part(2) - self._part(13)) == 0

    def is_year_start(self) -> Expr:
        return self._part(10) == 1

    def is_year_end(self) -> Expr:
        return (self._part(1) == 12) & (self._part(2) == 31)

    def is_quarter_start(self) -> Expr:
        # The month is the first of its quarter: month - 1 == 3 * (quarter - 1).
        first_month = (self._part(1) - 1) - (self._part(11) - 1) * 3
        return (self._part(2) == 1) & (first_month == 0)

    def is_quarter_end(self) -> Expr:
        return self.is_month_end() & ((self._part(1) - self._part(11) * 3) == 0)

    def quarter(self) -> Expr:
        return self._part(11)

    def is_leap_year(self) -> Expr:
        return self._part(12) == 1

    def week(self) -> Expr:
        return self._part(14)

    def iso_year(self) -> Expr:
        return self._part(15)

    def epoch(self, time_unit: str = "us") -> Expr:
        """Each instant as an integer count of ``time_unit`` (``"ns"``, ``"us"``,
        ``"ms"``, ``"s"`` or ``"d"``) since the epoch, from the column's own
        unit; a coarser unit floors."""

        if time_unit not in EPOCH_NS:
            raise ValueError("epoch: time_unit must be ns, us, ms, s or d")
        return self._e._op("dftu.series.cast", text=time_unit, kind="epoch", unit=self._given)

    def timestamp(self, time_unit: str = "us") -> Expr:
        """The polars name of :meth:`epoch`."""
        return self.epoch(time_unit)

    def to_string(self, format: str) -> Expr:
        """The polars name of :meth:`strftime`."""
        return self.strftime(format)

    def truncate(self, every: int | str) -> Expr:
        """Each instant floored to a multiple of ``every`` (a pandas offset
        string or a count of the column's units); ``floor`` is the pandas
        name, ``ceil`` and ``round`` likewise."""
        return self._round(every, 0)

    def floor(self, freq: int | str) -> Expr:
        """As :meth:`truncate` (the pandas name)."""
        return self._round(freq, 0)

    def ceil(self, freq: int | str) -> Expr:
        """Each instant ceiled to a multiple of ``freq``."""
        return self._round(freq, 1)

    def round(self, freq: int | str) -> Expr:
        return self._round(freq, 2)

    def normalize(self) -> Expr:
        """Each instant at midnight of its day."""
        return self._round("1d", 0)

    def cast_time_unit(self, time_unit: str) -> Expr:
        """The column in ``time_unit`` (``"ms"``, ``"us"`` or ``"ns"``; polars
        has no second unit), the instant kept: a coarser unit floors a Timestamp toward
        negative infinity and truncates a Duration toward zero, as polars. A
        value that does not fit 64 bits in a finer unit raises when the plan
        runs, where polars wraps."""
        if time_unit not in ("ns", "us", "ms"):
            raise ValueError(f"`time_unit` must be one of {{'ns', 'us', 'ms'}}, got {time_unit}")
        return self._e._op("dftu.series.dt_as_unit", a=UNIT_CODES[time_unit], b=1)

    def date(self) -> Expr:
        """The calendar day of each Timestamp (its wall day in the column's
        zone) as a Date32 column."""
        return self._e._op("dftu.series.dt_date")

    def time(self) -> Expr:
        """The time of day of each Timestamp (its wall time in the column's
        zone), in the column's unit (Time32 for s and ms, Time64 for us and
        ns)."""
        return self._e._op("dftu.series.dt_time")

    def total_seconds(self) -> Expr:
        """Each value as seconds (Float64), for a Duration or an Int64 read
        in the accessor's unit. Computed from nanoseconds, so the last bit of
        a Float64 can differ from ``Series.dt.total_seconds`` on a coarser
        unit."""
        return self.epoch("ns").cast("float64") / 1_000_000_000

    def tz_localize(
        self,
        tz: str | None,
        ambiguous: str = "raise",
        nonexistent: str | datetime.timedelta = "raise",
    ) -> Expr:
        """A naive Timestamp column read as wall times in ``tz`` (``None``
        turns an aware column into its wall times); ``ambiguous`` is
        ``"raise"``, ``"infer"``, ``"NaT"``, ``"earliest"``, ``"latest"`` or
        ``"null"``, ``nonexistent`` as :meth:`Series.dt.tz_localize`. The
        zone is checked here; a column that is already aware raises when the
        plan runs."""
        from ._time_index import _AMBIGUOUS, _nonexistent_code, check_zone

        check_zone(tz)
        if tz is None:
            return self._e._op("dftu.series.tz_localize", text="", a=0)
        mode = _AMBIGUOUS.get(ambiguous) if isinstance(ambiguous, str) else None
        if mode is None:
            raise ValueError(
                f"'{ambiguous}' is not valid for the ambiguous argument: "
                "'raise', 'infer', 'NaT' or a boolean array"
            )
        code, shift_ticks = _nonexistent_code(nonexistent, "ns")
        if not -(1 << 47) < shift_ticks < (1 << 47):
            raise ValueError(
                "the nonexistent shift of a lazy tz_localize must lie within +-2**47 "
                "nanoseconds (about 39 hours)"
            )
        packed = mode + (code << 8) + (shift_ticks << 16)
        return self._e._op("dftu.series.tz_localize", text=tz, a=packed)

    def tz_convert(self, tz: str | None) -> Expr:
        """The same instants shown in zone ``tz`` (``None`` for the naive UTC
        column); no tick moves. A naive column is not refused here, as
        ``Series.dt.tz_convert`` does."""
        from ._time_index import check_zone

        check_zone(tz)
        return self._e._op("dftu.series.with_timezone", text=tz or "")

    def replace_time_zone(
        self,
        time_zone: str | None,
        *,
        use_earliest: bool | None = None,
        ambiguous: str = "raise",
        non_existent: str = "raise",
    ) -> Expr:
        """The polars name of :meth:`tz_localize`."""
        if non_existent not in ("raise", "null"):
            raise ValueError(
                f"`non_existent` must be one of {{'raise', 'null'}}, got {non_existent}"
            )
        if use_earliest is not None:
            ambiguous = "earliest" if use_earliest else "latest"
        if ambiguous not in ("raise", "earliest", "latest", "null"):
            raise ValueError(
                f"`ambiguous` must be one of {{'raise', 'earliest', 'latest', 'null'}}, "
                f"got {ambiguous}"
            )
        return self.tz_localize(time_zone, ambiguous, "NaT" if non_existent == "null" else "raise")

    def convert_time_zone(self, time_zone: str) -> Expr:
        """The polars name of :meth:`tz_convert`."""
        return self.tz_convert(time_zone)

    def _round(self, every: int | str, mode: int) -> Expr:

        # A rule is a count of the unit of the column it runs on, known only
        # then; reject a rule that no unit could read now.
        count = rule_to_units(every, "ns")
        if isinstance(every, str):
            return self._e._op("dftu.series.dt_round", b=mode, text=every, unit=self._given)
        return self._e._op("dftu.series.dt_round", a=count, b=mode)


# The namespaces, bound after the class body so the names `str` and `dt` do
# not shadow the builtin type inside it.
setattr(
    Expr,
    "str",
    property(lambda self: _ExprStr(self), doc="The string namespace (polars ``col.str.*``)."),
)


setattr(
    Expr,
    "dt",
    property(
        lambda self: _ExprDt(self),
        doc="The temporal namespace (polars ``col.dt.*``); the parts read the column's own unit, an Int64 as microseconds.",
    ),
)


class IsNull(Expr):
    """The Bool mask of the null (or the present) rows of ``arg``."""

    def __init__(self, arg: Expr, null: bool, na: bool = False) -> None:
        self.arg = arg
        self.null = null
        # The pandas reading: a NaN of a float column counts as missing.
        self.na = na


class Select(Expr):
    """``cond ? a : b`` per row; a null condition takes ``b``."""

    def __init__(self, cond: Expr, a: Expr, b: Expr) -> None:
        self.cond = cond
        self.a = a
        self.b = b


class _Udf(Expr):
    """A call of a registered scalar UDF over ``args``; ``handle`` is the native
    UDF the call holds, so the plugin that registered it stays loaded."""

    def __init__(self, name: str, handle: _ext._UdfHandle, args: tuple[Expr, ...]) -> None:
        self.name = name
        self.handle = handle
        self.args = args


class Udf:
    """A registered scalar UDF, called over expressions: ``udf["f"](col("x"))``
    is the expression that calls ``f``. The handle keeps the UDF, and the plugin
    library that holds it, alive while it or an expression made from it lives.

    A :class:`Udf` that ``@dft.udf`` made owns its registration: the name stays
    registered until :meth:`close` (or the end of a ``with dft.udf.scope():``
    block), never because the object was garbage collected, so a decorated
    function keeps working while only its name is referenced. An expression
    that was built before :meth:`close` keeps running; a call on a closed
    :class:`Udf` raises ``RuntimeError``."""

    __slots__ = ("_name", "_handle", "_owning", "_closed", "_func", "_origin")

    def __init__(
        self,
        name: str,
        handle: _ext._UdfHandle,
        *,
        owning: bool = False,
        func: Callable[..., Any] | None = None,
        origin: str = "",
    ) -> None:
        self._name = name
        self._handle = handle
        self._owning = owning
        self._closed = False
        self._func = func
        self._origin = origin

    @property
    def name(self) -> str:
        return self._name

    @property
    def is_owning(self) -> bool:
        """Whether this object registered the UDF and :meth:`close` removes it."""
        return self._owning

    @property
    def is_closed(self) -> bool:
        return self._closed

    @property
    def func(self) -> Callable[..., Any] | None:
        """The Python function behind a ``@dft.udf`` UDF, or ``None``."""
        return self._func

    def close(self) -> None:
        """Remove the name from the registry. Expressions made before keep
        running. A second call, and a call on a handle that does not own its
        registration, do nothing."""
        if not self._owning or self._closed:
            return
        self._closed = True
        if _OWNED_UDFS.get(self._name) is self:
            del _OWNED_UDFS[self._name]
            _ext.udf_unregister(self._name)

    def __enter__(self) -> Udf:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __call__(self, *args: Value | Expr) -> Expr:
        if self._closed:
            raise RuntimeError(f"udf {self._name!r} is closed")
        return _Udf(self._name, self._handle, tuple(wrap(a) for a in args))

    def __repr__(self) -> str:
        return f"Udf({self._name!r})"


# The UDFs that @dft.udf registered and not yet closed, by name. They are held
# strongly: only close() or the end of a scope removes one.
_OWNED_UDFS: dict[str, Udf] = {}


_UDF_SCOPES: list[list[Udf]] = []


_UDF_EXIT_HOOK = False


_BATCH_DTYPES = frozenset(d.name.lower() for d in DType if d.is_numeric) | {"bool"}


def _close_udfs_at_exit() -> None:
    for owned in list(_OWNED_UDFS.values()):
        owned.close()
    _ext.udf_lane_close_at_exit()


def _udf_origin(fn: Callable[..., Any]) -> str:
    code = getattr(fn, "__code__", None)
    if code is None:
        return getattr(fn, "__name__", "a callable")
    return f"{code.co_filename}:{code.co_firstlineno}"


def _alias_args(hint: object) -> tuple[Any, ...]:
    # numpy on Python 3.8 builds npt.NDArray with its own alias class, which
    # typing.get_args does not know, so read __args__ as well.
    args = typing.get_args(hint)
    return args if args else tuple(getattr(hint, "__args__", ()))


def _batch_result_dtype(fn: Callable[..., Any], returns: object) -> str:
    """The result dtype name of a batch UDF from ``returns=`` or the return
    annotation (``np.ndarray`` needs ``returns=``; ``npt.NDArray[np.float64]`` names it)."""
    fn_name = getattr(fn, "__name__", "a callable")
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - numpy is a hard need here
        raise RuntimeError("@dft.udf(batch=True) needs numpy") from exc
    if returns is None:
        try:
            hint = typing.get_type_hints(fn).get("return")
        except Exception:
            hint = getattr(fn, "__annotations__", {}).get("return")
        args = _alias_args(hint)
        if len(args) == 1:
            returns = args[0]
        elif len(args) == 2:
            inner = _alias_args(args[1])
            if inner:
                returns = inner[0]
    if returns is None:
        raise TypeError(
            f"udf {fn_name!r} needs a result dtype: pass returns=np.float64 (or another "
            "dtype) or annotate the return as npt.NDArray[np.float64]"
        )
    if not isinstance(returns, (type, str, np.dtype)):
        raise TypeError(f"udf {fn_name!r}: {returns!r} is not a numpy dtype")
    try:
        name: str = np.dtype(returns).name
    except TypeError as exc:
        raise TypeError(f"udf {fn_name!r}: {returns!r} is not a numpy dtype") from exc
    if name not in _BATCH_DTYPES:
        raise TypeError(
            f"udf {fn_name!r} returns {name}, which a udf cannot give; "
            f"the result is one of {sorted(_BATCH_DTYPES)}"
        )
    return name


class _UdfRegistry(Mapping[str, Udf]):
    """The registered scalar UDFs by name: ``udf[name]``, ``udf.get(name)``,
    ``name in udf``, iteration and ``len``. ``udf["f"]`` raises ``KeyError`` here,
    not when an expression runs, for an unknown name.

    The object is also the decorator that registers a Python function as a scalar
    UDF, in one of two forms:

    - ``@dft.udf`` on a typed scalar function compiles it to native code (the
      per-row subset of ``dftracer.utils.jit``); no GIL is taken when it runs.
      A body outside the subset is a located :class:`~dftracer.utils.jit.JitError`.
    - ``@dft.udf(batch=True)`` keeps it as Python: it is called once per batch with
      read-only numpy views of the argument columns (a bool column is a new
      array) and returns an array of the batch length. The GIL is held while it
      runs; every such call runs on one dedicated lane thread, so the engine's
      workers never wait on the GIL themselves.

    Either way the result is an owning :class:`Udf`. ``with dft.udf.scope():``
    closes the UDFs registered inside it when the block ends."""

    def __getitem__(self, name: str) -> Udf:
        if not isinstance(name, str):
            raise KeyError(name)
        owned = _OWNED_UDFS.get(name)
        if owned is not None:
            return owned
        try:
            return Udf(name, _ext.udf_lookup(name))
        except LookupError:
            raise KeyError(name) from None

    def __iter__(self) -> Iterator[str]:
        return iter(_ext.udf_names())

    def __len__(self) -> int:
        return len(_ext.udf_names())

    def __repr__(self) -> str:
        return f"udf({list(self)!r})"

    @overload
    def __call__(self, fn: Callable[..., Any], /) -> Udf: ...
    @overload
    def __call__(
        self,
        *,
        batch: bool = False,
        name: str | None = None,
        returns: object = None,
        volatile: bool = False,
    ) -> Callable[[Callable[..., Any]], Udf]: ...
    def __call__(
        self,
        fn: Callable[..., Any] | None = None,
        *,
        batch: bool = False,
        name: str | None = None,
        returns: object = None,
        volatile: bool = False,
    ) -> Udf | Callable[[Callable[..., Any]], Udf]:
        """Register ``fn`` as a scalar UDF; see the class doc. ``name`` defaults
        to the function name, ``returns`` is the result dtype of a batch UDF
        when its annotation does not name it, ``volatile=True`` says the result
        can differ for the same arguments."""
        if fn is None:
            return lambda f: self._register(f, batch, name, returns, volatile)
        return self._register(fn, batch, name, returns, volatile)

    def _register(
        self,
        fn: Callable[..., Any],
        batch: bool,
        name: str | None,
        returns: object,
        volatile: bool,
    ) -> Udf:
        global _UDF_EXIT_HOOK
        if not callable(fn):
            raise TypeError("@dft.udf decorates a function")
        udf_name = name if name is not None else getattr(fn, "__name__", "")
        if not isinstance(udf_name, str) or not udf_name:
            raise ValueError("a udf needs a name: pass name=")
        origin = _udf_origin(fn)
        if udf_name in self:
            held = _OWNED_UDFS.get(udf_name)
            where = (
                f"defined at {held._origin}" if held is not None else "from a plugin or native code"
            )
            raise ValueError(
                f"udf {udf_name!r} defined at {origin} clashes with the udf {udf_name!r} that is "
                f"already registered ({where}); close it first or choose another name"
            )
        if batch:
            import inspect

            params = list(inspect.signature(fn).parameters.values())
            if not params or any(
                p.kind not in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)
                or p.default is not p.empty
                for p in params
            ):
                raise TypeError(
                    f"udf {udf_name!r}: a batch udf takes one or more positional column arguments"
                )
            dtype = _batch_result_dtype(fn, returns)
            if not _UDF_EXIT_HOOK:
                atexit.register(_close_udfs_at_exit)
                _UDF_EXIT_HOOK = True
            _ext.udf_register_batch(udf_name, fn, len(params), dtype, volatile)
        else:
            if returns is not None:
                raise TypeError("returns= is only for @dft.udf(batch=True)")
            from .jit._udf import compile_udf

            _ext.udf_register_library(compile_udf(fn, udf_name, volatile))
        owned = Udf(udf_name, _ext.udf_lookup(udf_name), owning=True, func=fn, origin=origin)
        _OWNED_UDFS[udf_name] = owned
        if _UDF_SCOPES:
            _UDF_SCOPES[-1].append(owned)
        return owned

    def scope(self) -> ContextManager[None]:
        """``with dft.udf.scope():`` closes the UDFs that ``@dft.udf`` registered
        inside the block when it ends, in reverse order. Scopes nest. The scope is
        process wide, not per thread."""
        return _udf_scope()


@contextlib.contextmanager
def _udf_scope() -> Generator[None, None, None]:
    mine: list[Udf] = []
    _UDF_SCOPES.append(mine)
    try:
        yield
    finally:
        # By identity: two empty scopes compare equal, and remove() would take
        # the outer one.
        for i in range(len(_UDF_SCOPES) - 1, -1, -1):
            if _UDF_SCOPES[i] is mine:
                del _UDF_SCOPES[i]
                break
        for owned in reversed(mine):
            owned.close()


udf: _UdfRegistry = _UdfRegistry()


def call(name: str, *args: Expr) -> Expr:
    """``udf[name](*args)``: the call of a registered scalar UDF. Raises
    ``KeyError`` for an unknown name."""
    return udf[name](*args)


class _When:
    """``when(cond).then(a).otherwise(b)``, the polars spelling of a select."""

    __slots__ = ("_cond",)

    def __init__(self, cond: Expr) -> None:
        self._cond = cond

    def then(self, value: Expr | int | float) -> _Then:
        return _Then(self._cond, wrap(value))


class _Then:
    __slots__ = ("_cond", "_value")

    def __init__(self, cond: Expr, value: Expr) -> None:
        self._cond = cond
        self._value = value

    def otherwise(self, value: Expr | int | float) -> Expr:
        return Select(self._cond, self._value, wrap(value))


def where(table: Frame, pred: Expr | str) -> Frame:
    """Keep the rows where ``pred`` is true.

    ``pred`` is either an expression (numeric comparisons / ``& | ~``),
    evaluated on the DataFrame engine to a boolean mask, or a duql
    **string** (e.g. ``"dur > 100 and cat == 'io'"``), parsed by the duql
    library and evaluated as a SIMD mask. A native ``DataFrame`` in stays fully
    columnar (returns a filtered ``DataFrame``); a ``pyarrow.Table`` in returns
    a filtered ``pyarrow.Table``."""
    from ._eval import Columnar

    if isinstance(table, _ext.DataFrame):
        if isinstance(pred, str):
            return _ext.DataFrame.query(table, pred)
        return _ext.DataFrame.filter(table, Columnar(pred).apply(table))
    # A pyarrow Table in gives one out: the rows are kept natively and the
    # result goes back through the Arrow stream, with no copy either way.
    import pyarrow as pa

    frame = _arrow_native("_dataframe_from_arrow")(table)
    if isinstance(pred, str):
        kept = _ext.DataFrame.query(frame, pred)
    else:
        kept = _ext.DataFrame.filter(frame, Columnar(pred).apply(frame))
    result = pa.table(kept)
    # the native round trip makes every field nullable and drops its metadata
    fields = [
        pa.field(out.name, out.type, src.nullable, src.metadata)
        for out, src in zip(result.schema, table.schema)
    ]
    return result.cast(pa.schema(fields, metadata=table.schema.metadata))


def eval_many(exprs: list[Expr | Columnar], source: Source) -> list[Series]:
    """Evaluate several value expressions in ONE compiled pass over `source`.

    CSE spans all of them, so a subexpression shared across outputs is computed
    once. Returns one :class:`Series` per input expression, aligned to `exprs`.
    `source` is a native vec batch or a pyarrow Table, as for :meth:`Columnar.apply`."""
    from ._eval import Columnar, _import_vec_columns

    cs = [e if isinstance(e, Columnar) else Columnar(e) for e in exprs]
    union: list[str] = []
    seen = set()
    for c in cs:
        for name in c.columns:
            if name not in seen:
                seen.add(name)
                union.append(name)
    if not union:  # all-constant expressions: no shared program to build
        return [c.apply(source) for c in cs]
    cols = _import_vec_columns(union, source)
    inputs = [cols[name] for name in union]
    index = {name: i for i, name in enumerate(union)}
    asts: list[list[tuple[object, ...]]] = []
    for c in cs:
        ast: list[tuple[object, ...]] = []
        emit_ast(c._expr, index.__getitem__, ast)
        asts.append(ast)
    return list(_ext.vec_eval_many(asts, inputs))


def when(cond: Expr) -> _When:
    """Start a conditional: ``when(F.dur > 10).then(1).otherwise(0)``."""
    return _When(as_expr(cond))


def if_else(cond: Expr, a: Expr | int | float, b: Expr | int | float) -> Expr:
    """``a`` where ``cond`` holds, else ``b``, per row (numpy ``where`` over
    expressions; the frame filter keeps the name ``where``)."""
    return Select(as_expr(cond), wrap(a), wrap(b))


class _FAccessor:
    """The ``F`` entry point. ``F.dur`` / ``F("args.level")`` / ``F["args.level"]``
    all build a field-leaf :class:`Expr`; use the call/subscript forms for a
    nested or non-identifier field name."""

    any: Wildcard

    def __call__(self, name: str) -> Expr:
        return Col(name)

    def __getattr__(self, name: str) -> Expr:
        return Col(name)

    def __getitem__(self, name: str) -> Expr:
        return Col(name)


class Wildcard(Expr):
    """``F.any``: the numeric-args wildcard for ``TraceViewer.agg``. ``.mean()``
    aggregates every numeric ``args.*`` field as a per-group mean (fields
    discovered at scan time); ``.count()`` is the group row count. The engine's
    wildcard path computes only the mean, so other reductions raise in
    ``TraceViewer.agg`` - name a field for those."""

    def count(self) -> Agg:
        return Agg("count", self)


F = _FAccessor()


#: ``F.any`` - the numeric-args wildcard (see :class:`Wildcard`).
F.any = Wildcard()


# Serialize `expr` to a post-order AST, resolving column names to input indices
# via `resolve` (a name -> int callable). Structural only - the C++ compiler does
# type inference, CSE, and lowering, so every consumer gets the same engine.
def emit_ast(expr: Expr, resolve: Callable[[str], int], ast: list[tuple[object, ...]]) -> None:
    if isinstance(expr, Lit):
        if isinstance(expr.value, str):
            ast.append((_AST_LIT_S, expr.value))
        elif isinstance(expr.value, float):
            ast.append((_AST_LIT_F, float(expr.value)))
        else:
            ast.append((_AST_LIT_I, int(expr.value)))
    elif isinstance(expr, Col):
        ast.append((_AST_COL, resolve(expr.name)))
    elif isinstance(expr, Bin):
        emit_ast(expr.left, resolve, ast)
        emit_ast(expr.right, resolve, ast)
        ast.append((_AST_BIN, _BIN_OP_CODE[expr.op]))
    elif isinstance(expr, Prim):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_PRIM, PRIM_CODES[expr.name]))
    elif isinstance(expr, Unary):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_UNARY, _UNARY_CODES[expr.name]))
    elif isinstance(expr, Clip):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_CLIP, expr.lo, expr.hi))
    elif isinstance(expr, Cast):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_CAST, expr.type_id))
    elif isinstance(expr, Fillna):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_FILLNA, expr.fill, 1 if expr.nan else 0))
    elif isinstance(expr, Cmp) and isinstance(expr.rhs, Expr):
        # Column against column: the engine compares the two row by row, null
        # where either side is null.
        emit_ast(expr.left, resolve, ast)
        emit_ast(expr.rhs, resolve, ast)
        ast.append((_AST_CMP_EXPR, CMP_CODES[expr.op]))
    elif isinstance(expr, Cmp):
        if isinstance(expr.rhs, bool) or not isinstance(expr.rhs, (int, float, str)):
            raise TypeError(
                "in-memory comparison needs a number or a string; a bool "
                "comparison is filter-only (push it down with .filter()/.duql())"
            )
        emit_ast(expr.left, resolve, ast)
        ast.append((_AST_CMP, CMP_CODES[expr.op], expr.rhs))
    elif isinstance(expr, Logical):
        emit_ast(expr.left, resolve, ast)
        if expr.op == "not" or expr.right is None:
            ast.append((_AST_NOT,))
        else:
            emit_ast(expr.right, resolve, ast)
            ast.append((_AST_LOGICAL, LOGICAL_CODES[expr.op]))
    elif isinstance(expr, _StrPred):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_PRED, _STR_PRED_CODES[expr.op], expr.pattern))
    elif isinstance(expr, _StrPredCol):
        emit_ast(expr.arg, resolve, ast)
        emit_ast(expr.needle, resolve, ast)
        ast.append((_AST_STR_PRED_COL, _STR_PRED_CODES[expr.op]))
    elif isinstance(expr, _StrReplaceCol):
        emit_ast(expr.arg, resolve, ast)
        emit_ast(expr.old, resolve, ast)
        emit_ast(expr.new, resolve, ast)
        ast.append((_AST_STR_REPLACE_COL,))
    elif isinstance(expr, (_IContains, _ILike)):
        emit_ast(expr._lowered(), resolve, ast)
    elif isinstance(expr, _IsIn):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_IS_IN, list(expr.values)))
        if expr.negated:
            ast.append((_AST_NOT,))
    elif isinstance(expr, _Udf):
        for arg in expr.args:
            emit_ast(arg, resolve, ast)
        ast.append((_AST_UDF, expr.handle, len(expr.args)))
    elif isinstance(expr, _StrMap):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_MAP, _STR_MAP_CODES[expr.op]))
    elif isinstance(expr, _StrFn):
        emit_ast(expr.arg, resolve, ast)
        if expr.other is not None:
            emit_ast(expr.other, resolve, ast)
        ast.append(
            (
                _AST_STR_FN,
                _STR_FN_CODES[expr.name],
                expr.text,
                expr.text2,
                expr.i0,
                expr.i1,
                1 if expr.other is None else 2,
            )
        )
    elif isinstance(expr, _StrLen):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_LEN, 1 if expr.chars else 0))
    elif isinstance(expr, _StrFind):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_FIND, expr.needle, 1 if expr.null_on_miss else 0))
    elif isinstance(expr, _StrReplace):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_REPLACE, expr.old, expr.new, 1 if expr.all else 0))
    elif isinstance(expr, _StrSlice):
        emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_SLICE, expr.start, expr.length))
    elif isinstance(expr, Select):
        emit_ast(expr.cond, resolve, ast)
        emit_ast(expr.a, resolve, ast)
        emit_ast(expr.b, resolve, ast)
        ast.append((_AST_SELECT,))
    elif isinstance(expr, IsNull):
        emit_ast(expr.arg, resolve, ast)
        mode = (2 if expr.null else 3) if expr.na else (1 if expr.null else 0)
        ast.append((_AST_IS_NULL, mode))
    elif isinstance(expr, _IRegex):
        raise TypeError(_PREDICATE_ONLY)
    elif isinstance(expr, ColumnOpNode):
        raise TypeError(
            f"{expr.spec.op}: a column op reads the whole column and is not an "
            "evaluator node; Expr.apply and the plan lower it first"
        )
    elif isinstance(expr, AggLeaf):
        raise TypeError(
            "an aggregate inside a row expression evaluates through "
            "GroupBy.apply / filter, which aggregates first"
        )
    else:
        raise TypeError(f"unsupported node {type(expr).__name__}")


class Agg:
    """An aggregate over a column expression - ``F.dur.mean()``,
    ``(F.a + F.b).sum()``, ``count()``. Rename the output with ``.alias(name)``.
    Feed to ``DataFrame.group_by(...).agg(...)``."""

    def __init__(
        self,
        op: str,
        value: Expr | None,
        out: str | None = None,
        param: float = 0.0,
        by: Expr | None = None,
        interpolation: str = "linear",
    ) -> None:
        self.op = op
        self.value = value
        self._out = out
        self.param = param
        self.by = by
        self.interpolation = interpolation

    def alias(self, name: str) -> Agg:
        return Agg(self.op, self.value, name, self.param, self.by, self.interpolation)

    def over(self, keys: str | Sequence[str]) -> Expr:
        """This aggregate per group of ``keys``, broadcast back onto every
        row of its group (polars ``col("x").sum().over("k")``, the pandas
        ``transform``): a group-by joined back on the keys, in input order."""

        spec = ColumnOp(
            "__agg_over__", over=[keys] if isinstance(keys, str) else list(keys), agg=self
        )
        return ColumnOpNode(Lit(0), spec)

    # Arithmetic and comparisons over aggregates build an Expr whose leaves
    # are the aggregates (GroupBy.apply / filter evaluate it per group).
    def __add__(self, other: object) -> Expr:
        return AggLeaf(self) + other

    def __radd__(self, other: object) -> Expr:
        return wrap(other) + AggLeaf(self)

    def __sub__(self, other: object) -> Expr:
        return AggLeaf(self) - other

    def __rsub__(self, other: object) -> Expr:
        return wrap(other) - AggLeaf(self)

    def __mul__(self, other: object) -> Expr:
        return AggLeaf(self) * other

    def __rmul__(self, other: object) -> Expr:
        return wrap(other) * AggLeaf(self)

    def __truediv__(self, other: object) -> Expr:
        return AggLeaf(self) / other

    def __rtruediv__(self, other: object) -> Expr:
        return wrap(other) / AggLeaf(self)

    def _cmp(self, op: str, other: object) -> Expr:
        # Against another aggregate or an expression: (self - other) <op> 0.
        if isinstance(other, (Agg, Expr)):
            return Cmp(op, AggLeaf(self) - other, 0)
        if isinstance(other, bool) or not isinstance(other, (int, float)):
            raise TypeError(f"an aggregate compares with a number or an aggregate, not {other!r}")
        return Cmp(op, AggLeaf(self), other)

    def __gt__(self, other: object) -> Expr:
        return self._cmp("gt", other)

    def __ge__(self, other: object) -> Expr:
        return self._cmp("ge", other)

    def __lt__(self, other: object) -> Expr:
        return self._cmp("lt", other)

    def __le__(self, other: object) -> Expr:
        return self._cmp("le", other)

    def __eq__(self, other: object) -> Expr:
        return self._cmp("eq", other)

    def __ne__(self, other: object) -> Expr:
        return self._cmp("ne", other)

    if TYPE_CHECKING:

        def __hash__(self) -> NoReturn: ...
    else:
        __hash__ = None

    @property
    def out(self) -> str:
        if self._out is not None:
            return self._out
        if self.value is None:
            return "count"
        if isinstance(self.value, Col):
            if self.op == "pct":
                return f"p{int(round(self.param * 100))}_{self.value.name}"
            if self.op == "quantile":
                return f"p{int(round(self.param * 100))}_{self.value.name}"
            op = "count" if self.op == "count_valid" else self.op
            return f"{op}_{self.value.name}"
        return self.op

    def _spec(self, names: list[str]) -> tuple[object, ...]:
        code = AGG_CODE[self.op]
        if self.value is None:
            return (code, None, self.out, self.param)
        ast: list[tuple[object, ...]] = []
        emit_ast(self.value, names.index, ast)
        if self.op == "quantile":
            return (
                code,
                ast,
                self.out,
                self.param,
                None,
                None,
                check_quantile(self.param, self.interpolation),
            )
        if self.by is None:
            return (code, ast, self.out, self.param)
        by_ast: list[tuple[object, ...]] = []
        emit_ast(self.by, names.index, by_ast)
        return (code, ast, self.out, self.param, by_ast)


def _occupancy(op: str, value: Expr, resolution: int | float | str | None) -> Agg:
    grid = 0 if resolution is None else int(round(coerce_duration(resolution, 1e6, "resolution")))
    return Agg(op, value, param=float(grid))


def _make_agg_method(op: str) -> Callable[[Expr], Agg]:
    def method(self: Expr) -> Agg:
        return Agg(op, self)

    method.__name__ = op
    method.__doc__ = f"The {op} of this expression per group (an :class:`Agg`)."
    return method


for _op in (
    "sum",
    "min",
    "max",
    "mean",
    "var",
    "std",
    "first",
    "last",
    "sumsq",
    "prod",
    "bitwise_and",
    "bitwise_or",
    "bitwise_xor",
):
    setattr(Expr, _op, _make_agg_method(_op))


def _expr_skew(self: Expr, bias: bool = True) -> Agg:
    """The skewness of this expression per group (an :class:`Agg`): the
    population one by default (polars), the pandas sample estimator with
    ``bias=False``."""
    return Agg("skew" if bias else "skew_unbiased", self)


def _expr_kurt(self: Expr, bias: bool = True) -> Agg:
    """The excess kurtosis of this expression per group (an :class:`Agg`): the
    population one by default (polars), the pandas sample estimator with
    ``bias=False``."""
    return Agg("kurt" if bias else "kurt_unbiased", self)


setattr(Expr, "skew", _expr_skew)


setattr(Expr, "kurt", _expr_kurt)


def collect_columns(expr: Expr) -> list[str]:
    seen: list[str] = []

    def walk(e: Expr) -> None:
        if isinstance(e, AggLeaf):
            return
        if isinstance(e, _AnyCol):
            raise DFTUtilsValueError(
                f"{e.name} filters in the trace scan (TraceViewer.duql); "
                "apply cannot evaluate any()"
            )
        if isinstance(e, Col):
            if e.name not in seen:
                seen.append(e.name)
        elif isinstance(e, Bin):
            walk(e.left)
            walk(e.right)
        elif isinstance(e, STRING_NODES + (Prim, Unary, Clip, Cast, Fillna, IsNull)):
            walk(e.arg)
            if isinstance(e, _StrFn) and e.other is not None:
                walk(e.other)
            elif isinstance(e, _StrPredCol):
                walk(e.needle)
            elif isinstance(e, _StrReplaceCol):
                walk(e.old)
                walk(e.new)
        elif isinstance(e, Cmp):
            walk(e.left)
            if isinstance(e.rhs, Expr):
                walk(e.rhs)
        elif isinstance(e, Logical):
            walk(e.left)
            if e.right is not None:
                walk(e.right)
        elif isinstance(e, Select):
            walk(e.cond)
            walk(e.a)
            walk(e.b)
        elif isinstance(e, _Udf):
            for arg in e.args:
                walk(arg)
        elif isinstance(e, ColumnOpNode):
            walk(e.arg)
            if e.spec.column2 is not None:
                walk(e.spec.column2)
            for k in e.spec.over or []:
                if k not in seen:
                    seen.append(k)
        elif isinstance(e, _IRegex):
            raise TypeError(_PREDICATE_ONLY)

    walk(expr)
    return seen


# Every node with a single `arg` operand that the engine's string kernels run.
STRING_NODES = (
    _StrPred,
    _StrPredCol,
    _StrReplaceCol,
    _IContains,
    _ILike,
    _IsIn,
    _StrMap,
    _StrLen,
    _StrFind,
    _StrReplace,
    _StrSlice,
    _StrFn,
)


def has_column_op(expr: Expr) -> bool:
    if isinstance(expr, ColumnOpNode):
        return True
    return any(has_column_op(v) for v in vars(expr).values() if isinstance(v, Expr))


def split_column_ops(expr: Expr) -> tuple[Expr, list[tuple[str, Expr, ColumnOp]]]:
    """The expression with every column op replaced by a column reference
    ``__dftu_cop_<i>__``, and the ops so named in evaluation order (an op's
    argument has its own ops replaced first, so each entry's argument is a
    plain expression over columns, the earlier temporaries included)."""
    ops: list[tuple[str, Expr, ColumnOp]] = []

    def rewrite(e: Expr) -> Expr:
        if isinstance(e, (Col, Lit)):
            return e
        if isinstance(e, ColumnOpNode):
            arg = rewrite(e.arg)
            spec = e.spec
            column2 = spec.column2
            if column2 is not None:
                spec = copy.copy(spec)
                spec.column2 = rewrite(column2)
            name = f"__dftu_cop_{len(ops)}__"
            ops.append((name, arg, spec))
            return Col(name)
        node = copy.copy(e)
        for key, value in list(vars(node).items()):
            if isinstance(value, Expr):
                setattr(node, key, rewrite(value))
        return node

    return rewrite(expr), ops
