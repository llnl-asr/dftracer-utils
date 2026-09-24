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
  ``.ends_with()``), membership (``.is_in()`` / ``.not_in()``), ``resolved.*``
  virtual fields, and ``& | ~``. A predicate over a bare field serializes to
  the query DSL (``str(expr)`` / ``.to_query()``) that ``TraceViewer.filter()``
  / ``.query()`` push down to the index.
- **String values** - ``.lower()``, ``.upper()``, ``.strip()``, ``.len_bytes()``,
  ``.find()``, ``.replace()``, ``.slice()`` and friends.

Every predicate and string op also evaluates in memory to a Series via
``.apply()`` and runs inside a ``LazyFrame`` plan, through the engine's own
string kernels. The two exceptions are ``iregex`` (filter-only) and a bool
comparison.
"""

from __future__ import annotations

import math
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Dict,
    List,
    Literal,
    NamedTuple,
    Optional,
    Sequence,
    Tuple,
    Union,
)

from . import dftracer_utils_ext as _ext
from .dataframe import DataFrame
from .enums import DType
from .series import Series, _unwrap

if TYPE_CHECKING:
    import pyarrow as pa  # ty: ignore[unresolved-import]

    from ._expr_ops import ColumnOp
    from .lazyframe import LazyFrame, LazyGroupBy

# A source of columns for in-memory evaluation: a wrapped or native DataFrame, a
# {name: Series} mapping, or a pyarrow Table imported at the Arrow boundary.
_Source = Union["DataFrame", "_ext._DataFrame", Dict[str, "Series"], "pa.Table"]
# where(): a frame in, the same kind of frame out.
_Frame = Union["DataFrame", "_ext._DataFrame", "pa.Table"]

__all__ = [
    "col",
    "lit",
    "columnar",
    "eval_many",
    "Expr",
    "ColumnExpr",
    "Field",
    "F",
    "resolved",
    "Value",
    "where",
    "when",
    "if_else",
    "Agg",
    "Named",
    "GroupBy",
    "count",
]

_Scalar = Union[int, float]

#: A scalar the filter DSL can compare a field against.
Value = Union[str, int, float, bool]

# TypeId codes the DSL requests, off DType so the ordinals live in one place.
_INT64 = int(DType.INT64)
_UINT64 = int(DType.UINT64)
_FLOAT32 = int(DType.FLOAT32)
_FLOAT64 = int(DType.FLOAT64)
_BOOL = int(DType.BOOL)
_FLOAT_TYPES = (_FLOAT32, _FLOAT64)

# TypeId codes the .cast() method accepts.
_CAST_CODES = {"int64": _INT64, "uint64": _UINT64, "float64": _FLOAT64, "bool": _BOOL}

# vec::Prim codes (kernels/prims.h). Exposed as methods, e.g. F.dur.ilog2().
_PRIM_CODES = {
    "ilog2": 0,
    "bit_width": 1,
    "popcount": 2,
    "clz": 3,
    "ctz": 4,
    "mix64": 5,
}

# DFTU_VEC_CMP_* and DFTU_VEC_LOGICAL_* codes (vec/abi.h).
_CMP_CODES = {"gt": 0, "ge": 1, "lt": 2, "le": 3, "eq": 4, "ne": 5}
_CMP_SYMBOL = {"gt": ">", "ge": ">=", "lt": "<", "le": "<=", "eq": "==", "ne": "!="}
_LOGICAL_CODES = {"and": 0, "or": 1}

# Resolved virtual-field leaf names accepted after the resolved./r. prefix.
_RESOLVED_FIELDS = frozenset({"fpath", "cwd", "hostname", "host", "exec", "cmd"})

# vec_eval opcodes (mirror the Op enum in columnar_eval.cpp).
# Post-order AST node opcodes for vec_eval (mirror columnar_eval.cpp). This is a
# pure structural serialization - no type logic; the C++ compiler does type
# inference, CSE, and lowering, so C/C++ consumers get the same engine.
_AST_COL, _AST_LIT_I, _AST_LIT_F, _AST_BIN = 0, 1, 2, 3
_AST_PRIM, _AST_CMP, _AST_LOGICAL, _AST_NOT, _AST_CAST = 4, 5, 6, 7, 8
_AST_UNARY, _AST_CLIP, _AST_FILLNA = 9, 10, 11
_AST_STR_PRED, _AST_STR_MAP, _AST_STR_LEN, _AST_STR_FIND = 12, 13, 14, 15
_AST_STR_REPLACE, _AST_STR_SLICE, _AST_IS_IN, _AST_SELECT = 16, 17, 18, 19
_AST_IS_NULL = 20

# StrPredOp / StrMapOp codes (dataframe/types.h).
_STR_PRED_CODES = {
    "contains": 0,
    "starts_with": 1,
    "ends_with": 2,
    "like": 3,
    "fullmatch": 4,
    "regex": 5,
}
_STR_MAP_CODES = {"lower": 0, "upper": 1, "strip": 2, "lstrip": 3, "rstrip": 4}

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
}
# Unary ops whose result is a Bool mask (not the input's numeric type).
_UNARY_BOOL = frozenset({"is_nan", "is_finite", "is_infinite"})
# Unary ops that widen to float.
_UNARY_FLOAT = frozenset({"log", "sqrt", "exp"})
# BinaryOp codes (vec::BinaryOp).
_BIN_OP_CODE = {"+": 0, "-": 1, "*": 2, "/": 3}

# vec::AggOp codes (agg.h), mirrored by the C ABI DFTU_AGG_* and the C++ builders.
_AGG_CODE = {
    "count": 0,
    "sum": 1,
    "min": 2,
    "max": 3,
    "mean": 4,
    "var": 5,
    "std": 6,
    "skew": 7,
    "kurt": 8,
    "first": 9,
    "last": 10,
    "pct": 11,
    "hist": 12,
    "argmax": 13,
    "sumsq": 14,
    "set_union": 15,
    "count_valid": 20,
    "argmin": 21,
    "bit_or": 22,
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
}

_NOT_PUSHABLE = (
    "not an index-pushable predicate; compute it with .apply() or filter the materialized frame"
)
_PREDICATE_ONLY = (
    "a case-insensitive regex (iregex) is filter-only and has no in-memory "
    ".apply() form; push it down with TraceViewer.filter()/.query(), or use "
    "regex() on lower()"
)


class Expr:
    """A lazy expression. Build values with ``+ - * /`` and the numeric prims,
    or predicates with comparisons / string-match / membership / ``& | ~``.
    Evaluate a value or a numeric-comparison mask in memory with :meth:`apply`;
    serialize a pure predicate for index pushdown with :meth:`to_query`."""

    def __add__(self, other: object) -> "Expr":
        return _Bin("+", self, _wrap(other))

    def __sub__(self, other: object) -> "Expr":
        return _Bin("-", self, _wrap(other))

    def __mul__(self, other: object) -> "Expr":
        return _Bin("*", self, _wrap(other))

    def __truediv__(self, other: object) -> "Expr":
        return _Bin("/", self, _wrap(other))

    def __radd__(self, other: object) -> "Expr":
        return _Bin("+", _wrap(other), self)

    def __rsub__(self, other: object) -> "Expr":
        return _Bin("-", _wrap(other), self)

    def __rmul__(self, other: object) -> "Expr":
        return _Bin("*", _wrap(other), self)

    # Comparisons build predicate Exprs. Against a number, or == / != against a
    # string, they also evaluate in memory to a boolean mask; against a bool
    # they are filter-only.
    def __gt__(self, other: Value) -> "Expr":
        return _Cmp("gt", self, other)

    def __ge__(self, other: Value) -> "Expr":
        return _Cmp("ge", self, other)

    def __lt__(self, other: Value) -> "Expr":
        return _Cmp("lt", self, other)

    def __le__(self, other: Value) -> "Expr":
        return _Cmp("le", self, other)

    def __eq__(self, other: Value) -> "Expr":  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return _Cmp("eq", self, other)

    def __ne__(self, other: Value) -> "Expr":  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return _Cmp("ne", self, other)

    def eq(self, other: Value) -> "Expr":
        """Equality predicate (the method form of ``==``)."""
        return _Cmp("eq", self, other)

    def ne(self, other: Value) -> "Expr":
        """Inequality predicate (the method form of ``!=``)."""
        return _Cmp("ne", self, other)

    def count(self) -> "Agg":
        """Group row count as an :class:`Agg` (the field is ignored, matching the
        engine's count)."""
        return Agg("count", None)

    __hash__ = None  # type: ignore[assignment]  # comparisons build exprs, not bools

    # A column has no single truth value; `if expr:` or `and` / `or` on one
    # would otherwise take one branch for every row. pandas and numpy refuse
    # this the same way.
    def __bool__(self) -> bool:
        raise TypeError(
            "the truth value of a column expression is ambiguous; use & | ~ to "
            "combine predicates, or is_between()/clip() in place of a branch"
        )

    # Combine predicates: &, |, ~ (Python and/or cannot be overloaded).
    def __and__(self, other: object) -> "Expr":
        return _Logical("and", self, _as_expr(other))

    def __or__(self, other: object) -> "Expr":
        return _Logical("or", self, _as_expr(other))

    def __invert__(self) -> "Expr":
        return _Logical("not", self, None)

    def apply(self, source: _Source) -> Series:
        """Evaluate this expression on ``source`` and return a
        :class:`~dftracer.utils.Series`.

        A value expression yields a value Series; a numeric comparison yields a
        boolean-mask Series. ``source`` is a native ``DataFrame`` (e.g. from
        ``view.collect()``), a ``{name: Series}`` mapping, or a
        ``pyarrow.Table``. Filter-only predicates (string match, membership,
        string/bool comparison) raise; push those down with
        ``TraceViewer.filter()`` instead. See also :meth:`DataFrame.apply` for
        the frame-first spelling ``df.apply(expr)``."""
        from ._expr_ops import eval_with_column_ops, has_column_op

        if has_column_op(self):
            return eval_with_column_ops(self, source)
        return Columnar(self).apply(source)

    def to_query(self) -> str:
        """Serialize this predicate to an index-pushable query-DSL string.

        Raises ``TypeError`` if the expression mixes value ops (arithmetic or
        numeric prims) into the predicate, i.e. it is not pushable; evaluate
        those in memory with :meth:`apply` instead."""
        return self._dsl()

    def _dsl(self) -> str:
        raise TypeError(_NOT_PUSHABLE)

    def __str__(self) -> str:
        return self._dsl()

    def clip(self, lo: Union[int, float], hi: Union[int, float]) -> "Expr":
        """Clamp each value to ``[lo, hi]`` (elementwise, SIMD)."""
        return _Clip(self, lo, hi)

    def cast(self, dtype: "Literal['int64', 'uint64', 'float64', 'bool']") -> "Expr":
        """Cast each value to ``dtype``."""
        return _Cast(_CAST_CODES[dtype], self)

    def fillna(self, fill: Union[int, float]) -> "Expr":
        """Replace nulls with ``fill`` (elementwise)."""
        return _Fillna(self, fill)

    def is_between(self, lo: Union[int, float], hi: Union[int, float]) -> "Expr":
        """Bool mask of ``lo <= x <= hi`` (desugars to two compares)."""
        return (self >= lo) & (self <= hi)

    def where(self, cond: "Expr", other: "Union[Expr, int, float]") -> "Expr":
        """Per row, this value where ``cond`` holds, else ``other`` (pandas
        ``Series.where``; a null condition takes ``other``)."""
        return _Select(_as_expr(cond), self, _wrap(other))

    def mask(self, cond: "Expr", other: "Union[Expr, int, float]") -> "Expr":
        """Per row, ``other`` where ``cond`` holds, else this value (pandas
        ``Series.mask``)."""
        return _Select(_as_expr(cond), _wrap(other), self)

    def is_null(self) -> "Expr":
        """Bool mask of the null rows (pandas ``isna``, polars ``is_null``)."""
        return _IsNull(self, True)

    def is_not_null(self) -> "Expr":
        """Bool mask of the present rows (pandas ``notna``)."""
        return _IsNull(self, False)

    isna = is_null
    notna = is_not_null

    # -- the column ops: the whole column at once (polars spellings) ----------
    def _op(
        self,
        op: str,
        a: Union[int, float] = 0,
        b: Union[int, float] = 0,
        text: Optional[str] = None,
        kind: Optional[str] = None,
        n: int = 0,
    ) -> "Expr":
        from ._expr_ops import ColumnOp

        return _ColumnOpNode(self, ColumnOp(op, None, a, b, text, None, kind, n))

    def cum_sum(self) -> "Expr":
        """The running sum (``cum_prod`` / ``cum_max`` / ``cum_min`` /
        ``cum_count`` likewise): a column op, the whole column at once; in a
        plan a breaker step. ``.over(keys)`` makes it group-wise."""
        return self._op("dftu.series.cumsum")

    def cum_prod(self) -> "Expr":
        return self._op("dftu.series.cum_prod")

    def cum_max(self) -> "Expr":
        return self._op("dftu.series.cummax")

    def cum_min(self) -> "Expr":
        return self._op("dftu.series.cummin")

    def cum_count(self) -> "Expr":
        return self._op("dftu.series.cum_count")

    def shift(self, n: int = 1) -> "Expr":
        return self._op("dftu.series.shift", a=int(n), n=int(n))

    def diff(self) -> "Expr":
        return self._op("dftu.series.diff")

    def pct_change(self) -> "Expr":
        return self._op("dftu.series.pct_change")

    def rank(self, method: str = "average", descending: bool = False) -> "Expr":
        from ._expr_ops import _RANK_METHODS

        if method not in _RANK_METHODS:
            raise ValueError(f"rank: unknown method {method!r}")
        return self._op("dftu.series.rank", a=_RANK_METHODS[method], b=1 if descending else 0)

    def forward_fill(self) -> "Expr":
        return self._op("dftu.series.ffill")

    def backward_fill(self) -> "Expr":
        return self._op("dftu.series.bfill")

    def fill_null(self, value: Union[int, float]) -> "Expr":
        return self.fillna(value)

    def interpolate(self) -> "Expr":
        return self._op("dftu.series.interpolate")

    def rolling_sum(self, window_size: int) -> "Expr":
        """The trailing window of ``window_size`` rows (``rolling_mean`` /
        ``_min`` / ``_max`` / ``_var`` / ``_std`` / ``_median`` /
        ``_quantile`` likewise), null until it is full."""
        return self._rolling("sum", window_size)

    def rolling_mean(self, window_size: int) -> "Expr":
        return self._rolling("mean", window_size)

    def rolling_min(self, window_size: int) -> "Expr":
        return self._rolling("min", window_size)

    def rolling_max(self, window_size: int) -> "Expr":
        return self._rolling("max", window_size)

    def _rolling(self, op: str, window_size: int) -> "Expr":
        from ._expr_ops import _ROLLING_OPS

        if window_size < 1:
            raise ValueError("rolling: window_size must be at least 1")
        return self._op(
            "dftu.series.rolling",
            a=int(window_size),
            b=_ROLLING_OPS[op],
            kind=f"rolling_{op}",
            n=int(window_size),
        )

    def rolling_var(self, window_size: int) -> "Expr":
        return self._op("dftu.series.rolling_var", a=int(window_size))

    def rolling_std(self, window_size: int) -> "Expr":
        return self._op("dftu.series.rolling_std", a=int(window_size))

    def rolling_median(self, window_size: int) -> "Expr":
        return self._op("dftu.series.rolling_median", a=int(window_size))

    def rolling_quantile(self, quantile: float, window_size: int) -> "Expr":
        return self._op("dftu.series.rolling_quantile", a=int(window_size), b=float(quantile))

    def ewm_mean(self, alpha: float) -> "Expr":
        return self._op("dftu.series.ewm_mean", a=float(alpha))

    def ewm_std(self, alpha: float) -> "Expr":
        return self._op("dftu.series.ewm_std", a=float(alpha))

    def sort(self, descending: bool = False) -> "Expr":
        """This column's values in order (the other columns do not move, as
        polars ``col.sort()`` inside ``with_columns``)."""
        return self._op("dftu.series.sort", a=1 if descending else 0)

    def arg_sort(self, descending: bool = False) -> "Expr":
        return self._op("dftu.series.argsort", a=1 if descending else 0)

    def reverse(self) -> "Expr":
        return self._op("dftu.series.reverse")

    def is_duplicated(self) -> "Expr":
        return self._op("dftu.series.is_duplicated")

    def is_unique(self) -> "Expr":
        return self._op("dftu.series.is_unique")

    def is_nan(self) -> "Expr":
        return self._op("dftu.series.is_nan")

    def is_finite(self) -> "Expr":
        return self._op("dftu.series.is_finite")

    def is_infinite(self) -> "Expr":
        return self._op("dftu.series.is_infinite")

    def hash(self) -> "Expr":
        """A 64-bit hash of each value (``fnv1a`` over its repr)."""
        return self._op("dftu.hash.fnv1a")

    def log10(self) -> "Expr":
        return self.log() / math.log(10.0)  # type: ignore[attr-defined]

    def log1p(self) -> "Expr":
        return (self + 1).log()  # type: ignore[attr-defined]

    def over(self, keys: "Union[str, Sequence[str]]") -> "Expr":
        """The group-wise form of the column op this expression is (polars
        ``over``): ``col("x").cum_sum().over("k")`` is the engine's group
        transform, one value per input row in input order. An aggregate's
        ``over`` is spelled ``col("x").sum().over("k")`` on the aggregate."""
        from ._expr_ops import _TRANSFORM_KINDS

        if not isinstance(self, _ColumnOpNode):
            raise TypeError(
                "over: only a column op (cum_sum, shift, rank, rolling_*, ...) takes over()"
            )
        spec = self.spec
        kind = spec.kind or _TRANSFORM_KINDS.get(spec.op)
        if kind is None:
            raise TypeError(f"over: {spec.op} has no group-wise form")
        import copy

        spec = copy.copy(spec)
        spec.over = [keys] if isinstance(keys, str) else list(keys)
        spec.kind = kind
        return _ColumnOpNode(self.arg, spec)

    def alias(self, name: str) -> "Named":
        """This expression under an output name, for ``select`` /
        ``with_columns`` (the polars spelling)."""
        return Named(name, self)

    # String predicates (Bool masks). Each runs in memory through the engine's
    # string kernels and, on a bare field, pushes down as a query.
    def contains(self, sub: str, case: bool = True) -> "Expr":
        """Substring predicate. Case-sensitive like pandas / polars; ``case=False``
        folds both sides to lowercase first (the query DSL's ``"sub" in field``)."""
        if case:
            return _StrPred("contains", self, sub)
        return _IContains(self, sub)

    def starts_with(self, prefix: str) -> "Expr":
        """Prefix predicate."""
        return _StrPred("starts_with", self, prefix)

    def ends_with(self, suffix: str) -> "Expr":
        """Suffix predicate."""
        return _StrPred("ends_with", self, suffix)

    def like(self, pattern: str) -> "Expr":
        """SQL LIKE: ``%`` matches any run, ``_`` any single char, ``\\`` escapes."""
        return _StrPred("like", self, pattern)

    def ilike(self, pattern: str) -> "Expr":
        """Case-insensitive LIKE (both sides lowercased)."""
        return _ILike(self, pattern)

    def regex(self, pattern: str) -> "Expr":
        """ECMAScript regex found anywhere in the value (a search, like the
        query DSL's ``~``). See :meth:`fullmatch` for a whole-string match."""
        return _StrPred("regex", self, pattern)

    def iregex(self, pattern: str) -> "Expr":
        """Case-insensitive regex search. Filter-only: it has no in-memory form."""
        return _IRegex(self, pattern)

    def fullmatch(self, pattern: str) -> "Expr":
        """ECMAScript regex matching the whole value (pandas ``str.fullmatch``).
        In-memory only: the query DSL has no anchored form."""
        return _StrPred("fullmatch", self, pattern)

    def is_in(self, values: Sequence[Value]) -> "Expr":
        """Membership predicate: the value is one of ``values`` (all ints or all
        strings)."""
        return _IsIn(self, values, negate=False)

    def not_in(self, values: Sequence[Value]) -> "Expr":
        """Exclusion predicate: the value is none of ``values``."""
        return _IsIn(self, values, negate=True)

    # String maps.
    def lower(self) -> "Expr":
        """ASCII lowercase."""
        return _StrMap("lower", self)

    def upper(self) -> "Expr":
        """ASCII uppercase."""
        return _StrMap("upper", self)

    def strip(self) -> "Expr":
        """Drop leading and trailing whitespace."""
        return _StrMap("strip", self)

    def lstrip(self) -> "Expr":
        """Drop leading whitespace."""
        return _StrMap("lstrip", self)

    def rstrip(self) -> "Expr":
        """Drop trailing whitespace."""
        return _StrMap("rstrip", self)

    def len_bytes(self) -> "Expr":
        """Per-row byte length (Int64)."""
        return _StrLen(self, chars=False)

    def len_chars(self) -> "Expr":
        """Per-row UTF-8 codepoint count (Int64)."""
        return _StrLen(self, chars=True)

    def find(self, needle: str) -> "Expr":
        """Byte index of the first ``needle``, or -1 (Int64)."""
        return _StrFind(self, needle)

    def replace(self, old: str, new: str) -> "Expr":
        """Replace the first ``old`` with ``new`` in each value."""
        return _StrReplace(self, old, new, all=False)

    def replace_all(self, old: str, new: str) -> "Expr":
        """Replace every ``old`` with ``new`` in each value."""
        return _StrReplace(self, old, new, all=True)

    def slice(self, start: int, length: int) -> "Expr":
        """The byte substring ``[start, start + length)`` of each value."""
        return _StrSlice(self, start, length)

    def quantile(self, q: float) -> "Agg":
        """A DDSketch quantile at level ``q`` in [0, 1], per group (an
        :class:`Agg`). Mergeable, so the parallel/distributed path is exact to
        the sketch's relative accuracy."""
        return Agg("pct", self, param=q)

    def percentile(self, p: float) -> "Agg":
        """A percentile ``p`` in [0, 100], per group (numpy-style);
        ``F.dur.percentile(99)`` is ``F.dur.quantile(0.99)``."""
        return Agg("pct", self, param=p / 100.0)

    def busy(self, resolution: "Union[int, float, str, None]" = None) -> "Agg":
        """Trace occupancy (``TraceViewer.agg`` only): the time at least one
        event was active. ``resolution`` snaps interval edges to that grid (a
        number is microseconds, a string such as ``"1ms"`` is converted);
        unset is the exact union."""
        return _occupancy("busy", self, resolution)

    def concurrency(self, resolution: "Union[int, float, str, None]" = None) -> "Agg":
        """Trace occupancy: average parallelism, summed duration over
        :meth:`busy` time; see :meth:`busy` for ``resolution``."""
        return _occupancy("concurrency", self, resolution)

    def utilization(self, resolution: "Union[int, float, str, None]" = None) -> "Agg":
        """Trace occupancy: :meth:`busy` time over the makespan; see
        :meth:`busy` for ``resolution``."""
        return _occupancy("utilization", self, resolution)

    def active(self, resolution: "Union[int, float, str, None]" = None) -> "Agg":
        """Trace occupancy: the peak number of events active at once; see
        :meth:`busy` for ``resolution``."""
        return _occupancy("active", self, resolution)

    def hist(self) -> "Agg":
        """The DDSketch histogram per group: a ``list<struct{lo, hi, count}>``
        column (mergeable, relative-error buckets)."""
        return Agg("hist", self)

    def argmax(self, by: "Expr") -> "Agg":
        """The String repr of this expression at the row maximizing ``by``,
        per group (an :class:`Agg`)."""
        return Agg("argmax", self, by=by)

    def argmin(self, by: "Expr") -> "Agg":
        """The String repr of this expression at the row minimizing ``by``,
        per group. Several ``argmin``/``argmax`` aggregates naming the same
        ``by`` all report the same winning row."""
        return Agg("argmin", self, by=by)

    def distinct(self, k: int = 0) -> "Agg":
        """Approximate distinct count per group (a KMV sketch of ``k`` hashes,
        1024 by default), exact below ``k`` distinct values."""
        return Agg("distinct", self, param=float(k))

    def list_sorted(self, by: "Expr") -> "Agg":
        """This expression's reprs per group as a ``list<string>``, ordered by
        ``by`` ascending with the repr breaking ties."""
        return Agg("list_sorted", self, by=by)

    def topk(self, by: "Expr", k: int = 0) -> "Agg":
        """The reprs at the ``k`` largest ``by`` values per group (8 by
        default), a ``list<string>`` in descending ``by`` order."""
        return Agg("topk", self, param=float(k), by=by)

    def bottomk(self, by: "Expr", k: int = 0) -> "Agg":
        """The reprs at the ``k`` smallest ``by`` values per group (8 by
        default), a ``list<string>`` in ascending ``by`` order."""
        return Agg("bottomk", self, param=float(k), by=by)

    def approx_topk(self, k: int = 0) -> "Agg":
        """Approximate heavy hitters per group (SpaceSaving with ``k``
        counters, 8 by default): a ``list<struct{value, count}>`` column by
        descending count."""
        return Agg("approx_topk", self, param=float(k))

    def sample(self, k: int = 0) -> "Agg":
        """A deterministic, mergeable bottom-``k``-by-hash sample of this
        expression's distinct reprs per group (8 by default), a sorted
        ``list<string>``."""
        return Agg("sample", self, param=float(k))

    def corr(self, by: "Expr") -> "Agg":
        """Pearson correlation of this expression against ``by`` per group."""
        return Agg("corr", self, by=by)

    def covar_pop(self, by: "Expr") -> "Agg":
        """Population covariance of this expression against ``by`` per group."""
        return Agg("covar_pop", self, by=by)

    def covar_samp(self, by: "Expr") -> "Agg":
        """Sample covariance of this expression against ``by`` per group."""
        return Agg("covar_samp", self, by=by)

    def regr_slope(self, by: "Expr") -> "Agg":
        """Least-squares slope of this expression on ``by`` per group."""
        return Agg("regr_slope", self, by=by)

    def regr_intercept(self, by: "Expr") -> "Agg":
        """Least-squares intercept of this expression on ``by`` per group."""
        return Agg("regr_intercept", self, by=by)

    def regr_r2(self, by: "Expr") -> "Agg":
        """Coefficient of determination of this expression on ``by`` per
        group."""
        return Agg("regr_r2", self, by=by)

    if TYPE_CHECKING:
        # Prims/unary math (dispatched via __getattr__) and aggs (installed by
        # setattr below), declared so consumers get precise types not Any.
        def ilog2(self) -> "Expr": ...
        def bit_width(self) -> "Expr": ...
        def popcount(self) -> "Expr": ...
        def clz(self) -> "Expr": ...
        def ctz(self) -> "Expr": ...
        def mix64(self) -> "Expr": ...
        def abs(self) -> "Expr": ...
        def round(self) -> "Expr": ...
        def floor(self) -> "Expr": ...
        def ceil(self) -> "Expr": ...
        def log(self) -> "Expr": ...
        def sqrt(self) -> "Expr": ...
        def exp(self) -> "Expr": ...
        def sign(self) -> "Expr": ...
        def negate(self) -> "Expr": ...
        def trunc(self) -> "Expr": ...
        def is_nan(self) -> "Expr": ...
        def is_finite(self) -> "Expr": ...
        def is_infinite(self) -> "Expr": ...
        def sum(self) -> "Agg": ...
        def min(self) -> "Agg": ...
        def max(self) -> "Agg": ...
        def mean(self) -> "Agg": ...
        def var(self) -> "Agg": ...
        def std(self) -> "Agg": ...
        def skew(self) -> "Agg": ...
        def kurt(self) -> "Agg": ...
        def first(self) -> "Agg": ...
        def last(self) -> "Agg": ...
        def sumsq(self) -> "Agg": ...
        def set_union(self) -> "Agg": ...
        def bit_or(self) -> "Agg": ...
        def prod(self) -> "Agg": ...

    # Runtime prim-method seam; hidden from the checker so unknown attributes
    # are type errors, not Any (the real methods are declared above).
    if not TYPE_CHECKING:

        def __getattr__(self, name):
            # F.dur.ilog2()/F.hhash.mix64() - int bit primitive; F.dur.floor()/
            # .log()/... - FP unary math. Both are unary methods.
            if name in _PRIM_CODES:
                return lambda: _Prim(name, self)
            if name in _UNARY_CODES:
                return lambda: _Unary(name, self)
            raise AttributeError(name)


ColumnExpr = Expr


def _wrap(x: object) -> Expr:
    if isinstance(x, Expr):
        return x
    if isinstance(x, Agg):
        return _AggLeaf(x)
    if isinstance(x, bool) or not isinstance(x, (int, float)):
        raise TypeError(f"expected a column expr or a number, got {x!r}")
    return _Lit(x)


class _Col(Expr):
    """A field / column reference. Its name drives both column lookup for
    :meth:`Expr.apply` and the field name in a pushed-down predicate."""

    def __init__(self, name: str) -> None:
        self.name = name


#: Back-compat alias: ``Field("dur")`` is the field leaf, same as ``F.dur``.
Field = _Col


class Named:
    """An expression under an output name: ``col("a").alias("b")``. What
    ``select`` and ``with_columns`` take beside plain names."""

    __slots__ = ("name", "expr")

    def __init__(self, name: str, expr: "Expr") -> None:
        self.name = name
        self.expr = expr


#: One item of a ``select``: a column name, a bare column expression (its own
#: name), or a named expression.
Selector = Union[str, "Expr", Named]


def select_aggs(items: Sequence[object]) -> "List[Agg]":
    """The aggregates of a ``select(col("a").sum(), ...)``, named as polars
    names them: an unaliased aggregate of a bare column is output under that
    column's name; two outputs under one name are refused."""
    out: List[Agg] = []
    seen: set = set()
    for item in items:
        if not isinstance(item, Agg):
            raise TypeError("select: mix of aggregates and columns")
        if item._out is None and isinstance(item.value, _Col):
            item = item.alias(item.value.name)
        if item.out in seen:
            raise ValueError(f"select: two outputs named {item.out!r}; use .alias(name)")
        seen.add(item.out)
        out.append(item)
    return out


def resolve_selectors(items: Sequence[object]) -> "List[Tuple[str, Optional[Expr]]]":
    """Flatten ``select`` arguments to ``(output name, expression or None)``
    pairs: a name or a bare column keeps itself (no expression to compute), a
    named expression computes under its name. A computed expression without
    a name is refused: the engine names every column."""
    out: "List[Tuple[str, Optional[Expr]]]" = []
    for item in items:
        if isinstance(item, str):
            out.append((item, None))
        elif isinstance(item, Named):
            out.append((item.name, item.expr))
        elif isinstance(item, _Col):
            out.append((item.name, None))
        elif isinstance(item, Expr):
            raise TypeError("select: a computed expression needs .alias(name)")
        elif isinstance(item, (list, tuple)):
            out.extend(resolve_selectors(item))
        else:
            raise TypeError(f"select: takes names or expressions, got {type(item).__name__}")
    return out


class _Lit(Expr):
    def __init__(self, value: _Scalar) -> None:
        self.value = value


class _Bin(Expr):
    def __init__(self, op: str, left: Expr, right: Expr) -> None:
        self.op = op
        self.left = left
        self.right = right


class _Prim(Expr):
    def __init__(self, name: str, arg: Expr) -> None:
        self.name = name
        self.arg = arg


class _Unary(Expr):
    def __init__(self, name: str, arg: Expr) -> None:
        self.name = name
        self.arg = arg


class _Clip(Expr):
    def __init__(self, arg: Expr, lo: "Union[int, float]", hi: "Union[int, float]") -> None:
        self.arg = arg
        self.lo = lo
        self.hi = hi


class _Cast(Expr):
    def __init__(self, type_id: int, arg: Expr) -> None:
        self.type_id = type_id
        self.arg = arg


class _Fillna(Expr):
    def __init__(self, arg: Expr, fill: "Union[int, float]") -> None:
        self.arg = arg
        self.fill = fill


class _Cmp(Expr):
    def __init__(self, op: str, left: Expr, rhs: object) -> None:
        if isinstance(rhs, _Lit):
            rhs = rhs.value
        if isinstance(rhs, Expr):
            raise TypeError("comparison right side must be a scalar value, not an expression")
        self.op = op
        self.left = left
        self.rhs = rhs

    def _dsl(self) -> str:
        if not isinstance(self.left, _Col):
            raise TypeError(_NOT_PUSHABLE)
        return f"{self.left.name} {_CMP_SYMBOL[self.op]} {_format_value(self.rhs)}"


class _Logical(Expr):
    def __init__(self, op: str, left: Expr, right: "Expr | None") -> None:
        self.op = op
        self.left = left
        self.right = right

    def _dsl(self) -> str:
        if self.op == "not" or self.right is None:
            return f"not ({self.left._dsl()})"
        return f"({self.left._dsl()} {self.op} {self.right._dsl()})"


def _field_name(arg: Expr) -> str:
    if not isinstance(arg, _Col):
        raise TypeError(_NOT_PUSHABLE)
    return arg.name


def _escape_like(s: str) -> str:
    out = []
    for ch in s:
        if ch in "%_\\":
            out.append("\\")
        out.append(ch)
    return "".join(out)


class _StrPred(Expr):
    """``arg <op> pattern`` for op in _STR_PRED_CODES: a Bool mask through the
    engine's string kernels. On a bare field it also serializes to the query
    DSL: contains / starts_with / ends_with as an escaped LIKE, like as LIKE,
    regex as ``~``; fullmatch has no DSL form."""

    def __init__(self, op: str, arg: Expr, pattern: str) -> None:
        self.op = op
        self.arg = arg
        self.pattern = pattern

    def _dsl(self) -> str:
        field = _field_name(self.arg)
        p = self.pattern
        if self.op == "contains":
            return f"{field} like {_format_value('%' + _escape_like(p) + '%')}"
        if self.op == "starts_with":
            return f"{field} like {_format_value(_escape_like(p) + '%')}"
        if self.op == "ends_with":
            return f"{field} like {_format_value('%' + _escape_like(p))}"
        if self.op == "like":
            return f"{field} like {_format_value(p)}"
        if self.op == "regex":
            return f"{field} ~ {_format_value(p)}"
        raise TypeError(_NOT_PUSHABLE)


class _IContains(Expr):
    """Case-insensitive substring: ``"sub" in field`` in the DSL, and
    ``lower(arg) contains lower(sub)`` in memory."""

    def __init__(self, arg: Expr, sub: str) -> None:
        self.arg = arg
        self.sub = sub

    def _dsl(self) -> str:
        return f"{_format_value(self.sub)} in {_field_name(self.arg)}"

    def _lowered(self) -> Expr:
        return _StrPred("contains", _StrMap("lower", self.arg), self.sub.lower())


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


class _StrLen(Expr):
    def __init__(self, arg: Expr, chars: bool) -> None:
        self.arg = arg
        self.chars = chars


class _StrFind(Expr):
    def __init__(self, arg: Expr, needle: str) -> None:
        self.arg = arg
        self.needle = needle


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


def _as_expr(x: object) -> Expr:
    if isinstance(x, Expr):
        return x
    raise TypeError("& / | combine expressions, not plain values")


def _format_value(v: object) -> str:
    if isinstance(v, str):
        escaped = v.replace('"', '\\"')
        return f'"{escaped}"'
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


# Import Arrow data into the native columnar engine (the boundary for the DSL).
def _series_from_arrow(array: "pa.Array") -> "_ext._Series":
    return _ext._series_from_arrow(array)


def _dataframe_from_arrow(table: "pa.Table") -> "_ext._DataFrame":
    return _ext._dataframe_from_arrow(table)


def col(name: str) -> Expr:
    """A column / field reference by name."""
    return _Col(name)


def lit(value: _Scalar) -> Expr:
    """A scalar literal (kept distinct from a column name, Polars-style)."""
    return _Lit(value)


class _AggLeaf(Expr):
    """An aggregate standing in a row or group expression (``g["v"] -
    g["v"].mean()``, ``g["v"].sum() > 10``): the group-by evaluates the
    aggregate first, then :func:`split_aggs` rewrites the leaf into a column
    reference over the joined result."""

    def __init__(self, agg: "Agg") -> None:
        self.agg = agg


def split_aggs(expr: Expr) -> "Tuple[Expr, List[Agg]]":
    """The expression with every aggregate leaf replaced by a column
    reference ``__dftu_agg_<i>__``, and the aggregates so aliased, in order."""
    import copy

    aggs: List[Agg] = []

    def rewrite(e: Expr) -> Expr:
        if isinstance(e, _AggLeaf):
            aggs.append(e.agg.alias(f"__dftu_agg_{len(aggs)}__"))
            return _Col(aggs[-1].out)
        if isinstance(e, _Col) or isinstance(e, _Lit):
            return e
        node = copy.copy(e)
        for key, value in list(vars(node).items()):
            if isinstance(value, Expr):
                setattr(node, key, rewrite(value))
        return node

    return rewrite(expr), aggs


def has_agg_leaf(expr: Expr) -> bool:
    if isinstance(expr, _AggLeaf):
        return True
    return any(has_agg_leaf(v) for v in vars(expr).values() if isinstance(v, Expr))


class _ColumnOpNode(Expr):
    """A registered column op over ``arg`` (see ``_expr_ops``)."""

    def __init__(self, arg: Expr, spec: "ColumnOp") -> None:
        self.arg = arg
        self.spec = spec


class _ExprStr:
    """``col.str``: the string ops as expression nodes where the evaluator
    has them, column ops otherwise."""

    __slots__ = ("_e",)

    def __init__(self, e: Expr) -> None:
        self._e = e

    def contains(self, pattern: str) -> Expr:
        return self._e.contains(pattern)

    def starts_with(self, prefix: str) -> Expr:
        return self._e.starts_with(prefix)

    def ends_with(self, suffix: str) -> Expr:
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

    def find(self, needle: str) -> Expr:
        return self._e.find(needle)

    def replace(self, old: str, new: str) -> Expr:
        return self._e.replace(old, new)

    def replace_all(self, old: str, new: str) -> Expr:
        return self._e.replace_all(old, new)

    def slice(self, offset: int, length: int) -> Expr:
        return self._e.slice(offset, length)

    def pad_start(self, length: int, fill_char: str = " ") -> Expr:
        return self._e._op("dftu.series.str_pad_start", a=int(length), text=fill_char)

    def pad_end(self, length: int, fill_char: str = " ") -> Expr:
        return self._e._op("dftu.series.str_pad_end", a=int(length), text=fill_char)

    def zfill(self, length: int) -> Expr:
        return self._e._op("dftu.series.str_zfill", a=int(length))

    def count_matches(self, pattern: str) -> Expr:
        return self._e._op("dftu.series.str_count", text=pattern)

    def strip_prefix(self, prefix: str) -> Expr:
        return self._e._op("dftu.series.str_remove_prefix", text=prefix)

    def strip_suffix(self, suffix: str) -> Expr:
        return self._e._op("dftu.series.str_remove_suffix", text=suffix)

    def to_titlecase(self) -> Expr:
        return self._e._op("dftu.series.str_case", a=2)

    def extract(self, pattern: str, group_index: int = 1) -> Expr:
        return self._e._op("dftu.series.str_extract", a=int(group_index), text=pattern)


class _ExprDt:
    """``col.dt``: the calendar parts and bucket rounding as column ops."""

    __slots__ = ("_e", "_unit")

    def __init__(self, e: Expr, unit: str = "us") -> None:
        self._e = e
        self._unit = unit

    def __call__(self, unit: str) -> "_ExprDt":
        return _ExprDt(self._e, unit)

    def _part(self, code: int) -> Expr:
        from .series import _TIME_UNITS

        return self._e._op("dftu.series.dt_part", a=code, b=_TIME_UNITS[self._unit])

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
        """Monday 1 .. Sunday 7, as polars."""
        return self._part(9) + 1

    def ordinal_day(self) -> Expr:
        return self._part(10)

    def quarter(self) -> Expr:
        return self._part(11)

    def is_leap_year(self) -> Expr:
        return self._part(12) == 1

    def week(self) -> Expr:
        return self._part(14)

    def iso_year(self) -> Expr:
        return self._part(15)

    def epoch(self, time_unit: str = "us") -> Expr:
        return self._e.cast("int64")

    def truncate(self, every: "Union[int, str]") -> Expr:
        """Each instant floored to a multiple of ``every`` (a pandas offset
        string or a count of the column's units); ``round`` likewise."""
        return self._e._op("dftu.series.dt_round", a=self._every(every), b=0)

    def round(self, every: "Union[int, str]") -> Expr:
        return self._e._op("dftu.series.dt_round", a=self._every(every), b=2)

    def _every(self, every: "Union[int, str]") -> int:
        from .indexing import rule_to_units

        return rule_to_units(every, self._unit)


# The namespaces, bound after the class body so the names `str` and `dt` do
# not shadow the builtin type inside it.
Expr.str = property(lambda self: _ExprStr(self), doc="The string namespace (polars ``col.str.*``).")  # type: ignore[attr-defined]
Expr.dt = property(  # type: ignore[attr-defined]
    lambda self: _ExprDt(self),
    doc="The temporal namespace (polars ``col.dt.*``); the parts read the column's own unit, an Int64 as microseconds.",
)


class _IsNull(Expr):
    """The Bool mask of the null (or the present) rows of ``arg``."""

    def __init__(self, arg: Expr, null: bool) -> None:
        self.arg = arg
        self.null = null


class _Select(Expr):
    """``cond ? a : b`` per row; a null condition takes ``b``."""

    def __init__(self, cond: Expr, a: Expr, b: Expr) -> None:
        self.cond = cond
        self.a = a
        self.b = b


class _When:
    """``when(cond).then(a).otherwise(b)``, the polars spelling of a select."""

    __slots__ = ("_cond",)

    def __init__(self, cond: Expr) -> None:
        self._cond = cond

    def then(self, value: "Union[Expr, int, float]") -> "_Then":
        return _Then(self._cond, _wrap(value))


class _Then:
    __slots__ = ("_cond", "_value")

    def __init__(self, cond: Expr, value: Expr) -> None:
        self._cond = cond
        self._value = value

    def otherwise(self, value: "Union[Expr, int, float]") -> Expr:
        return _Select(self._cond, self._value, _wrap(value))


def when(cond: Expr) -> _When:
    """Start a conditional: ``when(F.dur > 10).then(1).otherwise(0)``."""
    return _When(_as_expr(cond))


def if_else(cond: Expr, a: "Union[Expr, int, float]", b: "Union[Expr, int, float]") -> Expr:
    """``a`` where ``cond`` holds, else ``b``, per row (numpy ``where`` over
    expressions; the frame filter keeps the name ``where``)."""
    return _Select(_as_expr(cond), _wrap(a), _wrap(b))


class _FAccessor:
    """The ``F`` entry point. ``F.dur`` / ``F("args.level")`` / ``F["args.level"]``
    all build a field-leaf :class:`Expr`; use the call/subscript forms for a
    nested or non-identifier field name."""

    def __call__(self, name: str) -> Expr:
        return _Col(name)

    def __getattr__(self, name: str) -> Expr:
        return _Col(name)

    def __getitem__(self, name: str) -> Expr:
        return _Col(name)


class _Wildcard(Expr):
    """``F.any``: the numeric-args wildcard for ``TraceViewer.agg``. ``.mean()``
    aggregates every numeric ``args.*`` field as a per-group mean (fields
    discovered at scan time); ``.count()`` is the group row count. The engine's
    wildcard path computes only the mean, so other reductions raise in
    ``TraceViewer.agg`` - name a field for those."""

    def count(self) -> "Agg":
        return Agg("count", self)


F = _FAccessor()
#: ``F.any`` - the numeric-args wildcard (see :class:`_Wildcard`).
F.any = _Wildcard()  # type: ignore[attr-defined]


def resolved(name: str) -> Expr:
    """A resolved virtual field (``resolved.<name>``), which the engine rewrites
    to a concrete hash lookup against the index. Known names: fpath, cwd,
    hostname (alias host), exec, cmd."""
    if name not in _RESOLVED_FIELDS:
        raise ValueError(
            f"unknown resolved field {name!r}; expected one of {sorted(_RESOLVED_FIELDS)}"
        )
    return _Col("resolved." + name)


def columnar(expr: Expr) -> "Columnar":
    """Wrap a value expression; call ``apply(source)`` to evaluate it on the
    DataFrame engine and get a native ``Series``."""
    if not isinstance(expr, Expr):
        raise TypeError("columnar() expects an expression (col()/F./lit())")
    return Columnar(expr)


def where(table: _Frame, pred: "Expr | str") -> _Frame:
    """Keep the rows where ``pred`` is true.

    ``pred`` is either an expression (numeric comparisons / ``& | ~``),
    evaluated on the DataFrame engine to a boolean mask, or a query DSL
    **string** (e.g. ``"dur > 100 and cat == 'io'"``), parsed by the query
    library and evaluated as a SIMD mask. A native ``DataFrame`` in stays fully
    columnar (returns a filtered ``DataFrame``); a ``pyarrow.Table`` in returns
    a filtered ``pyarrow.Table``."""
    native = _unwrap(table)  # a wrapped DataFrame -> its native handle
    if isinstance(pred, str):
        if isinstance(native, _ext._DataFrame):
            return DataFrame(native.query(pred))
        import pyarrow as pa

        return pa.table(_ext._dataframe_from_arrow(native).query(pred))
    mask = columnar(pred).apply(native)  # a wrapped Series
    if isinstance(native, _ext._DataFrame):
        return DataFrame(native.filter(_unwrap(mask)))
    import pyarrow as pa

    return native.filter(pa.array(_unwrap(mask)))


class Columnar:
    """A value expression ready to evaluate on the DataFrame engine."""

    def __init__(self, expr: Expr) -> None:
        self._expr = expr
        self._columns = _collect_columns(expr)

    @property
    def columns(self) -> List[str]:
        """Input column names this expression reads."""
        return list(self._columns)

    def apply(self, source: _Source) -> Series:
        """Evaluate on the DataFrame engine and return a native ``Series``.

        ``source`` is either a native ``DataFrame`` (e.g. ``view.collect()``) or
        a ``{name: Series}`` mapping - kept entirely columnar with no input
        Arrow; or a ``pyarrow.Table``, whose columns are imported into the engine
        at the boundary. The result is a ``Series``; call ``.to_arrow()`` /
        ``.to_pandas()`` only if you want Arrow.

        Python serializes the expression tree to a post-order AST and hands it
        to the native engine (``_ext.vec_eval``), which does all the compilation
        - type inference, CSE, lowering - and fused chunked + parallel
        evaluation. A pure-constant expression (no columns) broadcasts on the
        per-node path."""
        cols = self._to_vec_columns(source)
        if self._columns:
            ast: List[tuple] = []
            self._serialize(self._expr, ast)
            inputs = [cols[name] for name in self._columns]
            return Series(_ext.vec_eval(ast, inputs))
        # Constant expression (no columns): broadcast to the row count.
        out_float = _is_float(self._expr, cols)
        result = _eval(self._expr, cols, out_float)
        if isinstance(result, _ScalarVal):
            return Series(_broadcast_scalar(result.value, out_float, cols))
        return Series(result.value)

    # Serialize `expr` to a post-order AST (a list of (op, args...) tuples) that
    # the C++ engine rebuilds and compiles. Structural only - no type logic.
    def _serialize(self, expr: Expr, ast: List[tuple]) -> None:
        _emit_ast(expr, self._columns.index, ast)

    def _to_vec_columns(self, source: _Source) -> Dict[str, "_ext._Series"]:
        return _import_vec_columns(self._columns, source)


def _unwrap_source_columns(source: object) -> Dict[str, Series]:
    """Every column of ``source`` (a frame, a native batch, a ``{name:
    Series}`` mapping or a pyarrow Table) as wrapped Series, by name."""
    native = _unwrap(source)
    if isinstance(native, dict):
        return {k: (v if isinstance(v, Series) else Series(v)) for k, v in native.items()}
    if isinstance(native, _ext._DataFrame):
        return {name: Series(native[name]) for name in native.column_names}
    import pyarrow as pa

    out: Dict[str, Series] = {}
    for name in native.column_names:
        arr = native.column(name)
        if isinstance(arr, pa.ChunkedArray):
            arr = arr.combine_chunks()
        out[name] = Series(_ext._series_from_arrow(arr))
    return out


def _import_vec_columns(names: List[str], source: _Source) -> Dict[str, "_ext._Series"]:
    """Resolve `names` to native vec columns from `source` (a native batch or a
    pyarrow Table), importing at the Arrow boundary only when needed."""
    source = _unwrap(source)  # a wrapped DataFrame -> its native handle
    # A native batch (_DataFrame or a {name: Series} dict) is used directly.
    if isinstance(source, (dict, _ext._DataFrame)):
        return {name: _unwrap(source[name]) for name in names}
    # A pyarrow Table: import each needed column into vec at the boundary.
    import pyarrow as pa

    cols: Dict[str, "_ext._Series"] = {}
    for name in names:
        arr = source.column(name)
        if isinstance(arr, pa.ChunkedArray):
            arr = arr.combine_chunks()
        cols[name] = _ext._series_from_arrow(arr)
    return cols


def eval_many(exprs: "List[Expr | Columnar]", source: _Source) -> "List[Series]":
    """Evaluate several value expressions in ONE compiled pass over `source`.

    CSE spans all of them, so a subexpression shared across outputs is computed
    once. Returns one :class:`Series` per input expression, aligned to `exprs`.
    `source` is a native vec batch or a pyarrow Table, as for :meth:`Columnar.apply`."""
    cs = [e if isinstance(e, Columnar) else columnar(e) for e in exprs]
    union: List[str] = []
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
    asts: List[List[tuple]] = []
    for c in cs:
        ast: List[tuple] = []
        _emit_ast(c._expr, index.__getitem__, ast)
        asts.append(ast)
    return [Series(s) for s in _ext.vec_eval_many(asts, inputs)]


def _broadcast_scalar(
    value: _Scalar, out_float: bool, cols: Dict[str, "_ext._Series"]
) -> "_ext._Series":
    # No constant-column kernel; derive one from any input column so a pure
    # constant expression stays in vec: (col * 0) + value, in the target type.
    if not cols:
        raise ValueError("a constant columnar expression needs at least one column")
    base = next(iter(cols.values())).cast(_FLOAT64 if out_float else _INT64)
    return base.mul_scalar(0).add_scalar(value)


# Serialize `expr` to a post-order AST, resolving column names to input indices
# via `resolve` (a name -> int callable). Structural only - the C++ compiler does
# type inference, CSE, and lowering, so every consumer gets the same engine.
def _emit_ast(expr: Expr, resolve: Callable[[str], int], ast: List[tuple]) -> None:
    if isinstance(expr, _Lit):
        if isinstance(expr.value, float):
            ast.append((_AST_LIT_F, float(expr.value)))
        else:
            ast.append((_AST_LIT_I, int(expr.value)))
    elif isinstance(expr, _Col):
        ast.append((_AST_COL, resolve(expr.name)))
    elif isinstance(expr, _Bin):
        _emit_ast(expr.left, resolve, ast)
        _emit_ast(expr.right, resolve, ast)
        ast.append((_AST_BIN, _BIN_OP_CODE[expr.op]))
    elif isinstance(expr, _Prim):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_PRIM, _PRIM_CODES[expr.name]))
    elif isinstance(expr, _Unary):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_UNARY, _UNARY_CODES[expr.name]))
    elif isinstance(expr, _Clip):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_CLIP, expr.lo, expr.hi))
    elif isinstance(expr, _Cast):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_CAST, expr.type_id))
    elif isinstance(expr, _Fillna):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_FILLNA, expr.fill))
    elif isinstance(expr, _Cmp):
        if isinstance(expr.rhs, bool) or not isinstance(expr.rhs, (int, float, str)):
            raise TypeError(
                "in-memory comparison needs a number or a string; a bool "
                "comparison is filter-only (push it down with .filter()/.query())"
            )
        if isinstance(expr.rhs, str) and expr.op not in ("eq", "ne"):
            raise TypeError("an ordered comparison against a string is not supported; use == or !=")
        _emit_ast(expr.left, resolve, ast)
        ast.append((_AST_CMP, _CMP_CODES[expr.op], expr.rhs))
    elif isinstance(expr, _Logical):
        _emit_ast(expr.left, resolve, ast)
        if expr.op == "not" or expr.right is None:
            ast.append((_AST_NOT,))
        else:
            _emit_ast(expr.right, resolve, ast)
            ast.append((_AST_LOGICAL, _LOGICAL_CODES[expr.op]))
    elif isinstance(expr, _StrPred):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_PRED, _STR_PRED_CODES[expr.op], expr.pattern))
    elif isinstance(expr, (_IContains, _ILike)):
        _emit_ast(expr._lowered(), resolve, ast)
    elif isinstance(expr, _IsIn):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_IS_IN, list(expr.values)))
        if expr.negated:
            ast.append((_AST_NOT,))
    elif isinstance(expr, _StrMap):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_MAP, _STR_MAP_CODES[expr.op]))
    elif isinstance(expr, _StrLen):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_LEN, 1 if expr.chars else 0))
    elif isinstance(expr, _StrFind):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_FIND, expr.needle))
    elif isinstance(expr, _StrReplace):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_REPLACE, expr.old, expr.new, 1 if expr.all else 0))
    elif isinstance(expr, _StrSlice):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_STR_SLICE, expr.start, expr.length))
    elif isinstance(expr, _Select):
        _emit_ast(expr.cond, resolve, ast)
        _emit_ast(expr.a, resolve, ast)
        _emit_ast(expr.b, resolve, ast)
        ast.append((_AST_SELECT,))
    elif isinstance(expr, _IsNull):
        _emit_ast(expr.arg, resolve, ast)
        ast.append((_AST_IS_NULL, 1 if expr.null else 0))
    elif isinstance(expr, _IRegex):
        raise TypeError(_PREDICATE_ONLY)
    elif isinstance(expr, _ColumnOpNode):
        raise TypeError(
            f"{expr.spec.op}: a column op reads the whole column and is not an "
            "evaluator node; Expr.apply and the plan lower it first"
        )
    elif isinstance(expr, _AggLeaf):
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
        value: "Expr | None",
        out: "str | None" = None,
        param: float = 0.0,
        by: "Expr | None" = None,
    ) -> None:
        self.op = op
        self.value = value
        self._out = out
        self.param = param
        self.by = by

    def alias(self, name: str) -> "Agg":
        return Agg(self.op, self.value, name, self.param, self.by)

    def over(self, keys: "Union[str, Sequence[str]]") -> Expr:
        """This aggregate per group of ``keys``, broadcast back onto every
        row of its group (polars ``col("x").sum().over("k")``, the pandas
        ``transform``): a group-by joined back on the keys, in input order."""
        from ._expr_ops import ColumnOp

        spec = ColumnOp(
            "__agg_over__", over=[keys] if isinstance(keys, str) else list(keys), agg=self
        )
        return _ColumnOpNode(_Lit(0), spec)

    # Arithmetic and comparisons over aggregates build an Expr whose leaves
    # are the aggregates (GroupBy.apply / filter evaluate it per group).
    def __add__(self, other: object) -> Expr:
        return _AggLeaf(self) + other

    def __radd__(self, other: object) -> Expr:
        return _wrap(other) + _AggLeaf(self)

    def __sub__(self, other: object) -> Expr:
        return _AggLeaf(self) - other

    def __rsub__(self, other: object) -> Expr:
        return _wrap(other) - _AggLeaf(self)

    def __mul__(self, other: object) -> Expr:
        return _AggLeaf(self) * other

    def __rmul__(self, other: object) -> Expr:
        return _wrap(other) * _AggLeaf(self)

    def __truediv__(self, other: object) -> Expr:
        return _AggLeaf(self) / other

    def __rtruediv__(self, other: object) -> Expr:
        return _wrap(other) / _AggLeaf(self)

    def _cmp(self, op: str, other: object) -> Expr:
        # Against another aggregate or an expression: (self - other) <op> 0.
        if isinstance(other, (Agg, Expr)):
            return _Cmp(op, _AggLeaf(self) - other, 0)
        if isinstance(other, bool) or not isinstance(other, (int, float)):
            raise TypeError(f"an aggregate compares with a number or an aggregate, not {other!r}")
        return _Cmp(op, _AggLeaf(self), other)

    def __gt__(self, other: object) -> Expr:
        return self._cmp("gt", other)

    def __ge__(self, other: object) -> Expr:
        return self._cmp("ge", other)

    def __lt__(self, other: object) -> Expr:
        return self._cmp("lt", other)

    def __le__(self, other: object) -> Expr:
        return self._cmp("le", other)

    def __eq__(self, other: object) -> Expr:  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._cmp("eq", other)

    def __ne__(self, other: object) -> Expr:  # type: ignore[override]  # ty: ignore[invalid-method-override]
        return self._cmp("ne", other)

    __hash__ = None  # type: ignore[assignment]

    @property
    def out(self) -> str:
        if self._out is not None:
            return self._out
        if self.value is None:
            return "count"
        if isinstance(self.value, _Col):
            if self.op == "pct":
                return f"p{int(round(self.param * 100))}_{self.value.name}"
            return f"{self.op}_{self.value.name}"
        return self.op

    def _spec(self, names: List[str]) -> tuple:
        code = _AGG_CODE[self.op]
        if self.value is None:
            return (code, None, self.out, self.param)
        ast: List[tuple] = []
        _emit_ast(self.value, names.index, ast)
        if self.by is None:
            return (code, ast, self.out, self.param)
        by_ast: List[tuple] = []
        _emit_ast(self.by, names.index, by_ast)
        return (code, ast, self.out, self.param, by_ast)


def _occupancy(op: str, value: Expr, resolution: "Union[int, float, str, None]") -> Agg:
    from ._units import coerce_duration

    grid = 0 if resolution is None else int(round(coerce_duration(resolution, 1e6, "resolution")))
    return Agg(op, value, param=float(grid))


def count() -> Agg:
    """The group row count (``count()`` -> column ``count``)."""
    return Agg("count", None)


def _make_agg_method(op: str) -> "Callable[[Expr], Agg]":
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
    "skew",
    "kurt",
    "first",
    "last",
    "sumsq",
    "set_union",
    "bit_or",
    "prod",
):
    setattr(Expr, _op, _make_agg_method(_op))


def _to_agg(spec: object) -> Agg:
    if isinstance(spec, Agg):
        return spec
    if isinstance(spec, str):
        if ":" in spec:
            op, column = spec.split(":", 1)
            if ":" in column:
                column, out = column.split(":", 1)
                return Agg(op, _Col(column), out)
            return Agg(op, _Col(column), f"{op}_{column}")
        return Agg(spec, None, spec)  # e.g. "count"
    raise TypeError(f"agg spec must be an Agg or string, got {type(spec).__name__}")


# The pandas aliases for the engine's one-column aggregates, as ``agg("x")``,
# ``agg(["x", ...])``, ``agg({"col": "x"})`` and ``agg(out=("col", "x"))``
# name them. The pandas names whose engine aggregate is a sketch (median,
# quantile -> pct; nunique -> distinct) are NOT aliased: pandas gives the exact
# value and the sketch an approximate one, so those are spelled out as
# ``col("x").quantile(0.5)`` / ``.distinct()`` by the caller who accepts that,
# or taken exactly through ``GroupBy.median() / quantile(q) / nunique()``.
_PANDAS_AGG_NAMES = {"count": "count_valid", "size": "count"}
_PANDAS_AGG_APPROXIMATE = {
    "median": "quantile(0.5)",
    "quantile": "quantile(q)",
    "nunique": "distinct()",
}


def _agg_op_name(name: str) -> str:
    if name in _PANDAS_AGG_APPROXIMATE:
        raise ValueError(
            f"agg({name!r}): the engine's {_PANDAS_AGG_APPROXIMATE[name]} is a sketch "
            f"(approximate), not the exact pandas value; call col(...).{_PANDAS_AGG_APPROXIMATE[name]} "
            f"to use it knowingly, or the exact group-by method .{name}()"
        )
    return _PANDAS_AGG_NAMES.get(name, name)


def _named_agg(column: str, name: str, out: str) -> Agg:
    """``(column, name)`` as an :class:`Agg` under ``out``, pandas dict / tuple
    style: ``name`` is an engine aggregate or one of its pandas aliases."""
    op = _agg_op_name(name)
    if op == "count":
        return Agg("count", None, out)
    return Agg(op, _Col(column), out)


def _agg_specs_from_pandas_forms(
    frame: object,
    keys: Sequence[str],
    func: object,
    named: "Dict[str, object]",
) -> "List[Union[str, Agg]]":
    """Lower the pandas ``agg`` argument forms to engine specs, flat names.
    ``frame`` is a DataFrame or LazyFrame (either wrapper or native handle);
    only its ``reduce_specs`` is used.

    - a string: broadcast over every eligible non-key column, each under its
      own name (``agg("sum")``);
    - a list of strings: every (column, name) pair as ``<column>_<name>``;
    - a dict ``{column: name | [names]}`` as ``<column>_<name>`` (a single
      name keeps the column's own name, as pandas does);
    - keywords ``out=("column", name)`` (also ``pd.NamedAgg``);
    - :class:`Agg` expressions and ``"op:column"`` strings pass through.
    """
    if func is None and not named:
        raise TypeError("agg() needs at least one aggregate")
    native = getattr(frame, "_native", frame)
    specs: "List[Union[str, Agg]]" = []
    if isinstance(func, str):
        specs.extend(native.reduce_specs(_agg_op_name(func), list(keys)))
    elif isinstance(func, Agg):
        specs.append(func)
    elif isinstance(func, dict):
        for column, names in func.items():
            if isinstance(names, (str, Agg)):
                if isinstance(names, Agg):
                    specs.append(names)
                else:
                    specs.append(_named_agg(column, names, column))
            else:
                for name in names:
                    specs.append(_named_agg(column, name, f"{column}_{name}"))
    elif isinstance(func, (list, tuple)):
        for item in func:
            if isinstance(item, Agg) or (isinstance(item, str) and ":" in item):
                specs.append(item)
                continue
            if not isinstance(item, str):
                raise TypeError(f"agg() list items must be names or Agg, got {type(item).__name__}")
            for spec in native.reduce_specs(_agg_op_name(item), list(keys)):
                column = spec.split(":")[1]
                specs.append(_named_agg(column, item, f"{column}_{item}"))
    elif func is not None:
        raise TypeError(f"agg() takes a name, a list, a dict or an Agg, got {type(func).__name__}")
    for out, spec in named.items():
        if isinstance(spec, Agg):
            specs.append(spec.alias(out))
        elif isinstance(spec, tuple) and len(spec) == 2 and isinstance(spec[1], str):
            specs.append(_named_agg(str(spec[0]), spec[1], out))
        elif hasattr(spec, "column") and hasattr(spec, "aggfunc"):
            specs.append(_named_agg(str(spec.column), str(spec.aggfunc), out))
        else:
            raise TypeError(f"agg() keyword {out!r} must be (column, name) or an Agg")
    return specs


class GroupBy:
    """A lazy group-by over a native ``DataFrame`` (zero or more key columns;
    none makes the whole frame one group). ``.agg(...)`` takes aggregate
    expressions (``col("x").sum()``, ``count()``), ``"op:column"`` strings, or
    the pandas forms (a name, a list of names, a ``{column: name}`` dict,
    ``out=("column", name)`` keywords); ``.sum()`` and its family broadcast one
    aggregate over every eligible non-key column."""

    def __init__(
        self,
        batch: "Union[DataFrame, _ext._DataFrame]",
        keys: "Union[str, Sequence[str], None]" = None,
    ) -> None:
        self._batch = batch
        self._keys: List[str] = (
            [] if keys is None else [keys] if isinstance(keys, str) else list(keys)
        )

    @property
    def keys(self) -> List[str]:
        return list(self._keys)

    def __getitem__(self, columns: "Union[str, Sequence[str]]") -> "GroupBy":
        """``groupby(k)["v"]`` / ``groupby(k)[["v", "w"]]``: the same groups
        over only those value columns (the pandas column selection). With
        one column, a list of aggregate names labels the outputs by name
        alone, as pandas does."""
        frame = self._batch if isinstance(self._batch, DataFrame) else DataFrame(self._batch)
        picked = [columns] if isinstance(columns, str) else list(columns)
        for c in picked:
            if c not in frame:
                raise KeyError(f"groupby: no column named {c!r}")
        out = GroupBy(
            frame.select(self._keys + [c for c in picked if c not in self._keys]), self._keys
        )
        out._single = picked[0] if len(picked) == 1 else None
        return out

    _single: "Optional[str]" = None

    def agg(self, *specs: object, **named: object) -> DataFrame:
        frame = self._batch if isinstance(self._batch, DataFrame) else DataFrame(self._batch)
        flat: "List[Union[str, Agg]]" = []
        for spec in specs:
            if isinstance(spec, Agg) or (isinstance(spec, str) and ":" in spec):
                flat.append(spec)
            else:
                flat.extend(_agg_specs_from_pandas_forms(frame, self._keys, spec, {}))
        if named:
            flat.extend(_agg_specs_from_pandas_forms(frame, self._keys, None, named))
        if not flat:
            raise TypeError("agg() needs at least one aggregate")
        native = _unwrap(self._batch)
        aggs = [_to_agg(s) for s in flat]
        # Plain aggregates over plain columns take the engine's group_by
        # directly; anything with an expression goes through the
        # expression group-by.
        plain = bool(self._keys) and all(
            a.by is None and a.param == 0.0 and (a.value is None or type(a.value) is _Col)
            for a in aggs
        )
        if plain:
            legacy = ["count" if a.value is None else f"{a.op}:{a.value.name}" for a in aggs]
            out = DataFrame(native.group_by(*self._keys, *legacy))
            engine_names = ["count" if a.value is None else f"{a.op}_{a.value.name}" for a in aggs]
            if engine_names != [a.out for a in aggs]:
                out = DataFrame(
                    {
                        **{k: out[k] for k in self._keys},
                        **{a.out: out[name] for a, name in zip(aggs, engine_names)},
                    }
                )
        else:
            names = list(native.column_names)
            out = DataFrame(native._group_agg_expr(self._keys, [a._spec(names) for a in aggs]))
        if self._single is not None and any(isinstance(sp, (list, tuple)) for sp in specs):
            prefix = f"{self._single}_"
            out = out.rename({c: c[len(prefix) :] for c in out.columns if c.startswith(prefix)})
        return out

    aggregate = agg

    def reduce(self, agg: str) -> DataFrame:
        """One engine aggregate by name over every eligible non-key column,
        each under its own name (the ``.sum()`` family's engine)."""
        native = _unwrap(self._batch)
        if not self._keys:
            return DataFrame(native.reduce(agg))
        return DataFrame(native.group_by(*self._keys, *native.reduce_specs(agg, self._keys)))

    # The pandas ``groupby(...).sum()`` family.
    def sum(self) -> DataFrame:
        return self.reduce("sum")

    def mean(self) -> DataFrame:
        return self.reduce("mean")

    def min(self) -> DataFrame:
        return self.reduce("min")

    def max(self) -> DataFrame:
        return self.reduce("max")

    def count(self) -> DataFrame:
        return self.reduce("count_valid")

    def var(self) -> DataFrame:
        return self.reduce("var")

    def std(self) -> DataFrame:
        return self.reduce("std")

    def skew(self) -> DataFrame:
        return self.reduce("skew")

    def kurt(self) -> DataFrame:
        return self.reduce("kurt")

    def first(self) -> DataFrame:
        return self.reduce("first")

    def last(self) -> DataFrame:
        return self.reduce("last")

    def size(self) -> DataFrame:
        """The row count per group, as one Int64 column ``size``."""
        return self.agg(Agg("count", None, "size"))

    # The group-wise transforms (cumsum, shift, rank, head, ...): the engine's
    # GroupBy::transform (dftu_dataframe_group_transform), one value per input
    # row in input order; the lazy group-by builds the same plan.
    def _transform(
        self, kind: str, n: int = 0, method: str = "average", ascending: bool = True
    ) -> DataFrame:
        return DataFrame(
            _unwrap(self._batch).group_transform(self._keys, kind, int(n), method, bool(ascending))
        )

    def cumsum(self) -> DataFrame:
        return self._transform("cumsum")

    def cummax(self) -> DataFrame:
        return self._transform("cummax")

    def cummin(self) -> DataFrame:
        return self._transform("cummin")

    def cumcount(self) -> DataFrame:
        return self._transform("cumcount")

    def shift(self, periods: int = 1) -> DataFrame:
        return self._transform("shift", periods)

    def diff(self) -> DataFrame:
        return self._transform("diff")

    def pct_change(self) -> DataFrame:
        return self._transform("pct_change")

    def rank(self, method: str = "average", ascending: bool = True) -> DataFrame:
        return self._transform("rank", 0, method, ascending)

    def ngroup(self) -> DataFrame:
        return self._transform("ngroup")

    def head(self, n: int = 5) -> DataFrame:
        return self._transform("head", n)

    def tail(self, n: int = 5) -> DataFrame:
        return self._transform("tail", n)

    def nth(self, n: "Union[int, Sequence[int]]") -> DataFrame:
        """The ``n``-th row of each group (0-based; negative from the end), or
        of each position in a list, in input order."""
        if isinstance(n, int):
            return self._transform("nth", n)
        return self.take(n)

    def cumprod(self) -> DataFrame:
        """Each numeric column's running product within its group (Float64)."""
        return self._transform("cumprod")

    def prod(self) -> DataFrame:
        return self.reduce("prod")

    product = prod

    def median(self) -> DataFrame:
        """The exact median of each numeric column per group (pandas
        ``GroupBy.median``): the plan of :func:`lazyframe.group_quantile`
        over this frame, collected."""
        return self.quantile(0.5)

    def quantile(self, q: float = 0.5) -> DataFrame:
        """The exact ``q`` quantile of each numeric column per group, linear
        interpolation (pandas ``GroupBy.quantile``)."""
        from .lazyframe import group_quantile

        frame = self._frame()
        return group_quantile(frame.lazy(), self._keys, self._value_columns(), float(q)).collect()

    def ffill(self) -> DataFrame:
        """Each non-key column's nulls filled from the nearest present value
        before them within the group (pandas ``GroupBy.ffill``); ``bfill``
        from after."""
        return self._transform("ffill")

    def bfill(self) -> DataFrame:
        return self._transform("bfill")

    def rolling(self, window: int) -> "_GroupRolling":
        """A trailing window of ``window`` rows within each group (pandas
        ``GroupBy.rolling``): ``.sum() .mean() .min() .max()`` are one engine
        transform each, null until the window holds ``window`` present values;
        ``.var() .std() .median() .quantile(q)`` run the Series rolling kernel
        over the group-sorted frame. One value per input row, in input
        order."""
        return _GroupRolling(self, int(window))

    def expanding(self) -> "_GroupSeriesWindow":
        """The expanding window within each group (pandas
        ``GroupBy.expanding``): ``.sum() .mean() .min() .max() .count() .var()
        .std()``, the Series kernel per group, one value per input row in
        input order."""
        return _GroupSeriesWindow(self, lambda s: s.expanding())

    def ewm(
        self,
        alpha: Optional[float] = None,
        *,
        span: Optional[float] = None,
        com: Optional[float] = None,
        halflife: Optional[float] = None,
    ) -> "_GroupSeriesWindow":
        """The exponentially weighted window within each group (pandas
        ``GroupBy.ewm``): ``.mean() .std()``, the Series kernel per group."""
        from .series import ewm_alpha

        a = ewm_alpha(alpha, span, com, halflife)
        return _GroupSeriesWindow(self, lambda s: s.ewm(a))

    def corr(self) -> DataFrame:
        """Per group, the Pearson correlation of every numeric column pair
        (pandas ``GroupBy.corr``, long form): the keys, ``a``, ``b`` and
        ``value``, one ``corr`` aggregate per pair."""
        return self._pairwise("corr")

    def cov(self) -> DataFrame:
        """Per group, the sample covariance of every numeric column pair
        (pandas ``GroupBy.cov``, long form as :meth:`corr`)."""
        return self._pairwise("covar_samp")

    def _pairwise(self, agg: str) -> DataFrame:
        cols = self._value_columns()
        specs = [Agg(agg, _Col(b), f"__dftu_{a}__{b}__", by=_Col(a)) for a in cols for b in cols]
        wide = self.agg(*specs)
        parts: List[DataFrame] = []
        for a in cols:
            for b in cols:
                one = wide.select(*self._keys, f"__dftu_{a}__{b}__").rename(
                    {f"__dftu_{a}__{b}__": "value"}
                )
                n = len(one)
                one = one.with_columns(a=Series.from_list([a] * n), b=Series.from_list([b] * n))
                parts.append(one.select(*self._keys, "a", "b", "value"))
        out = parts[0].concat(*parts[1:]) if len(parts) > 1 else parts[0]
        return out.sort_by_multi(self._keys + ["a", "b"]) if self._keys else out

    def take(self, indices: Sequence[int]) -> DataFrame:
        """The rows at ``indices`` within each group (0-based; negative from
        the end), in input order (pandas ``GroupBy.take``)."""
        frame = self._frame()
        row = "__dftu_group_row__"
        indexed = frame.with_row_index(row)
        picked = [GroupBy(indexed, self._keys).nth(int(i)) for i in indices]
        if not picked:
            return frame.head(0)
        out = picked[0].concat(*picked[1:]) if len(picked) > 1 else picked[0]
        return out.unique(row).sort_by(row).select(*frame.columns)

    def sample(self, n: int, seed: int = 0, *, random_state: Optional[int] = None) -> DataFrame:
        """A deterministic sample of ``n`` rows per group (fewer for a smaller
        group), in input order: the rows whose salted row hash ranks lowest
        within the group (pandas ``GroupBy.sample``; ``random_state`` is the
        pandas spelling of ``seed``)."""
        if random_state is not None:
            seed = random_state
        frame = self._frame()
        row, key = "__dftu_group_row__", "__dftu_hash__"
        indexed = frame.with_row_index(row)
        hashed = indexed.with_column(key, (_Col(row) + int(seed)).mix64().apply(indexed))
        ordered = hashed.sort_by_multi(self._keys + [key])
        picked = GroupBy(ordered, self._keys).head(int(n))
        return picked.sort_by(row).select(*frame.columns)

    def resample(
        self, rule: "Union[int, str]", on: Optional[str] = None, unit: str = "us"
    ) -> "_GroupResample":
        """Tumbling time windows of ``rule`` within each group (pandas
        ``GroupBy.resample``): the time column (the index column or ``on``)
        floored to its bucket joins the keys, and the aggregate family
        (``.sum()``, ``.agg(...)``, ``.size()``, ...) is one ``group_by`` over
        keys + bucket, rows by key then time."""
        from .dataframe import _single_index
        from .indexing import rule_to_units
        from .series import _DtAccessor

        frame = self._frame()
        time = on if on is not None else _single_index(frame._index, "resample")
        if time not in frame:
            raise KeyError(f"resample: no column named {time!r}")
        if time in self._keys:
            raise ValueError(f"resample: {time!r} is a group key")
        dt = _DtAccessor(self._col(frame, time), unit if unit != "us" else None)
        bucket = dt.floor(rule_to_units(rule, dt._unit))
        grouped = GroupBy(frame.with_column(time, bucket), self._keys + [time])
        return _GroupResample(grouped, self._keys + [time])

    # -- the rest of the pandas GroupBy surface, as compositions of group_by --
    def _frame(self) -> DataFrame:
        return self._batch if isinstance(self._batch, DataFrame) else DataFrame(self._batch)

    def _value_columns(self, agg: str = "sum") -> List[str]:
        return [s.split(":")[1] for s in _unwrap(self._batch).reduce_specs(agg, self._keys)]

    @staticmethod
    def _col(frame: DataFrame, name: str) -> Series:
        return Series(frame._native[name])

    @property
    def ngroups(self) -> int:
        return len(self.size())

    def get_group(self, key: object) -> DataFrame:
        """The rows of one group, by its key value (a tuple for several
        keys), in input order."""
        if not self._keys:
            raise ValueError("get_group: the group-by has no keys")
        out = self._frame().set_index(self._keys).loc[key, :]
        assert isinstance(out, DataFrame)
        return out

    @property
    def groups(self) -> Dict[object, List[int]]:
        """``{key: [row positions]}`` (``indices`` likewise)."""
        frame = self._frame()
        if not self._keys:
            return {(): list(range(len(frame)))}
        cols = [self._col(frame, k).to_list() for k in self._keys]
        out: Dict[object, List[int]] = {}
        for i in range(len(frame)):
            label = cols[0][i] if len(cols) == 1 else tuple(c[i] for c in cols)
            out.setdefault(label, []).append(i)
        return out

    indices = groups

    def pipe(self, func: Callable[..., object], *args: object, **kwargs: object) -> object:
        return func(self, *args, **kwargs)

    def nunique(self) -> DataFrame:
        """Distinct present values per group and non-key column, exact: a
        group-by over the keys plus the column, then a count per key."""
        frame = self._frame()
        out: Optional[DataFrame] = None
        for c in [n for n in frame.columns if n not in self._keys]:
            present = frame.filter(self._col(frame, c).notna())
            pairs = GroupBy(present, self._keys + [c]).size()
            counts = GroupBy(pairs, self._keys).agg(Agg("count", None, c))
            out = counts if out is None else out.join(counts, on=self._keys)
        if out is None:
            raise ValueError("nunique: no columns besides the keys")
        return out

    def sem(self) -> DataFrame:
        """Standard error of the mean per group: ``std / sqrt(count)``."""
        cols = self._value_columns("std")
        specs = [Agg("std", _Col(c), c) for c in cols]
        specs += [Agg("count_valid", _Col(c), f"__dftu_n_{c}__") for c in cols]
        out = self.agg(*specs)
        for c in cols:
            n = self._col(out, f"__dftu_n_{c}__").astype("float64").sqrt()
            out = out.with_column(c, self._col(out, c) / n)
        return out.select(*(self._keys + cols))

    def any(self) -> DataFrame:
        """Per group and numeric column: whether any value is non-zero
        (``all`` likewise)."""
        return self._truth("max")

    def all(self) -> DataFrame:
        return self._truth("min")

    def _truth(self, agg: str) -> DataFrame:
        frame = self._frame()
        cols = self._value_columns("sum")
        for c in cols:
            nonzero = self._col(frame, c).astype("float64").ne(0).astype("int64")
            frame = frame.with_column(c, nonzero)
        out = GroupBy(frame, self._keys).reduce(agg)
        for c in cols:
            out = out.with_column(c, self._col(out, c).ne(0))
        return out

    def idxmax(self) -> DataFrame:
        """Per group and numeric column: the row position of the maximum
        (``idxmin`` likewise); the index label when one index column is set."""
        return self._arg("argmax")

    def idxmin(self) -> DataFrame:
        return self._arg("argmin")

    def _arg(self, op: str) -> DataFrame:
        frame = self._frame()
        label = "__dftu_pos__"
        index = getattr(frame, "_index", None)
        if index is not None and len(index) == 1:
            label = index[0]
        else:
            frame = frame.with_row_index(label)
        cols = self._value_columns("sum")
        specs = [Agg(op, _Col(label), c, by=_Col(c)) for c in cols]
        out = GroupBy(frame, self._keys).agg(*specs)
        # The arg aggregates report the label's repr (a string); a numeric
        # label is parsed back to its type.
        dtype = self._col(frame, label).dtype
        if dtype != DType.STRING:
            for c in cols:
                out = out.with_column(c, self._col(out, c).astype(dtype))
        return out

    def ohlc(self) -> DataFrame:
        """Per group and numeric column: ``<c>_open`` / ``_high`` / ``_low`` /
        ``_close`` (first, max, min, last)."""
        specs = []
        for c in self._value_columns("sum"):
            specs += [
                Agg("first", _Col(c), f"{c}_open"),
                Agg("max", _Col(c), f"{c}_high"),
                Agg("min", _Col(c), f"{c}_low"),
                Agg("last", _Col(c), f"{c}_close"),
            ]
        return self.agg(*specs)

    def describe(self) -> DataFrame:
        """Per group and numeric column: ``<c>_count`` / ``_mean`` / ``_std``
        / ``_min`` / ``_25%`` / ``_50%`` / ``_75%`` / ``_max`` (the quantiles
        exact, through :meth:`quantile`)."""
        specs = []
        cols = self._value_columns("sum")
        for c in cols:
            for name in ("count_valid", "mean", "std", "min", "max"):
                out = f"{c}_{'count' if name == 'count_valid' else name}"
                specs.append(Agg(name, _Col(c), out))
        out = self.agg(*specs)
        for q in (0.25, 0.5, 0.75):
            quant = self.quantile(q)
            names = {c: f"{c}_{int(q * 100)}%" for c in cols}
            if self._keys:
                out = out.join(quant.rename(names), on=self._keys)
            else:
                out = out.with_columns(**{names[c]: self._col(quant, c) for c in cols})
        order = [*self._keys]
        for c in cols:
            order += [
                f"{c}_{s}" for s in ("count", "mean", "std", "min", "25%", "50%", "75%", "max")
            ]
        return out.select(*order)

    def value_counts(self, ascending: bool = False) -> DataFrame:
        """Distinct rows of the non-key columns within each group with their
        count, most frequent first."""
        frame = self._frame()
        cols = [c for c in frame.columns if c not in self._keys]
        for c in cols:  # pandas drops a row with a null value (dropna=True)
            frame = frame.filter(self._col(frame, c).notna())
        out = GroupBy(frame, self._keys + cols).size().rename({"size": "count"})
        return out.sort_values(
            self._keys + ["count"], ascending=[True] * len(self._keys) + [ascending]
        )

    def transform(self, func: str) -> DataFrame:
        """A group aggregate (``"mean"``, ``"sum"``, ...) broadcast back to
        every row of its group, one column per numeric non-key column, rows
        in input order."""
        frame = self._frame()
        agg = _PANDAS_AGG_NAMES.get(func, func)
        cols = self._value_columns(agg)
        row = "__dftu_row__"
        indexed = frame.with_row_index(row)
        specs = [Agg(agg, _Col(c), f"__dftu_t_{c}__") for c in cols]
        grouped = GroupBy(indexed, self._keys).agg(*specs)
        if not self._keys:
            out = indexed
            for c in cols:
                v = self._col(grouped, f"__dftu_t_{c}__")[0]
                assert isinstance(v, (int, float))
                broadcast = self._col(frame, c).full_like(v)
                out = out.with_column(c, broadcast)
            return out.select(*cols)
        joined = indexed.join(grouped, on=self._keys).sort_values(row)
        for c in cols:
            joined = joined.with_column(c, self._col(joined, f"__dftu_t_{c}__"))
        return joined.select(*cols)

    # -- user functions: traced into the engine, else run per group -----------
    def apply(self, func: "Callable[[Any], object]") -> "Union[DataFrame, Series]":
        """``func`` over each group (pandas ``GroupBy.apply``). It is first
        run once on a symbolic group whose columns are expressions, so a body
        like ``lambda g: g["v"].sum() / g["n"].sum()`` lowers to one group-by
        (a frame of the keys and ``value``), and ``lambda g: g["v"] -
        g["v"].mean()`` to a group-by joined back on the rows (a Series in
        input order). A body the trace cannot follow runs in Python per group
        with a warning: a scalar result per group gives the keys + ``value``
        frame, a Series or frame result the concatenation."""
        traced = _trace_group(func, self._frame().columns)
        if traced is not None:
            return self._lower_apply(traced)
        return self._apply_python(func)

    def filter(self, func: "Callable[[Any], object]") -> DataFrame:
        """The rows of the groups for which ``func`` holds (pandas
        ``GroupBy.filter``): ``lambda g: g["v"].sum() > 10`` lowers to one
        group-by and a join; a body the trace cannot follow runs in Python per
        group with a warning. Rows come back in input order."""
        traced = _trace_group(func, self._frame().columns, "GroupBy.filter")
        if traced is not None:
            return self._lower_filter(traced)
        return self._filter_python(func)

    def _group_values(self, expr: Expr) -> "Tuple[DataFrame, Expr]":
        """The per-group frame holding every aggregate leaf of ``expr`` (keys
        plus ``__dftu_agg_<i>__`` columns) and the expression rewritten over
        it."""
        rewritten, aggs = split_aggs(expr)
        if not aggs:
            raise TypeError("the group function must reduce (sum, mean, ...) at least once")
        return self.agg(*aggs), rewritten

    def _lower_apply(self, traced: object) -> "Union[DataFrame, Series]":
        frame = self._frame()
        if isinstance(traced, Agg):
            return self.agg(traced.alias("value"))
        if not isinstance(traced, Expr):
            raise TypeError(
                f"the group function returned {type(traced).__name__}; expected an "
                "aggregate or an expression over the group"
            )
        if not has_agg_leaf(traced):
            # A row expression with no aggregate: one value per row.
            return traced.apply(frame)
        grouped, rewritten = self._group_values(traced)
        row_cols = [c for c in _collect_columns(traced) if c in frame]
        if not row_cols:
            value = rewritten.apply(grouped)
            return grouped.with_column("value", value).select(*(self._keys + ["value"]))
        # Row leaves too: the aggregates broadcast back onto the rows.
        row = "__dftu_row__"
        indexed = frame.with_row_index(row)
        joined = indexed.join(grouped, on=self._keys).sort_values(row) if self._keys else indexed
        if not self._keys:
            for name in grouped.columns:
                v = self._col(grouped, name)[0]
                assert isinstance(v, (int, float))
                joined = joined.with_column(name, self._col(frame, row_cols[0]).full_like(v))
        return rewritten.apply(joined)

    def _lower_filter(self, traced: object) -> DataFrame:
        frame = self._frame()
        if isinstance(traced, Agg):
            traced = _AggLeaf(traced) != 0
        if not isinstance(traced, Expr) or not has_agg_leaf(traced):
            raise TypeError("filter: the group function must return a condition over aggregates")
        if [c for c in _collect_columns(traced) if c in frame]:
            raise TypeError("filter: the group function must reduce every column it reads")
        grouped, rewritten = self._group_values(traced)
        keep = grouped.filter(rewritten.apply(grouped))
        if not self._keys:
            return frame if len(keep) else frame.head(0)
        row = "__dftu_row__"
        indexed = frame.with_row_index(row)
        out = indexed.join(keep.select(*self._keys), on=self._keys).sort_values(row)
        return out.select(*frame.columns)

    def _groups_in_order(self) -> "List[Tuple[object, List[int]]]":
        return list(self.groups.items())

    def _apply_python(self, func: "Callable[[Any], object]") -> "Union[DataFrame, Series]":
        frame = self._frame()
        results: List[Tuple[object, object]] = []
        for label, rows in self._groups_in_order():
            results.append((label, func(frame.take(rows))))
        if not results:
            return frame.head(0)
        first = results[0][1]
        if isinstance(first, DataFrame):
            out = results[0][1]
            assert isinstance(out, DataFrame)
            for _, r in results[1:]:
                assert isinstance(r, DataFrame)
                out = out.concat(r)
            return out
        if isinstance(first, Series):
            values: List[object] = []
            for _, r in results:
                assert isinstance(r, Series)
                values.extend(r.to_list())
            return Series.from_list(values)
        keys: Dict[str, List[object]] = {k: [] for k in self._keys}
        for label, _ in results:
            parts = label if isinstance(label, tuple) else (label,)
            for k, v in zip(self._keys, parts):
                keys[k].append(v)
        return DataFrame({**keys, "value": [r for _, r in results]})

    def _filter_python(self, func: "Callable[[Any], object]") -> DataFrame:
        frame = self._frame()
        keep: List[int] = []
        for _, rows in self._groups_in_order():
            if func(frame.take(rows)):
                keep.extend(rows)
        return frame.take(sorted(keep))


class _GroupRolling:
    """``GroupBy.rolling(window)``: the pandas window object over a group-by,
    eager or lazy."""

    __slots__ = ("_grouped", "_window")

    def __init__(self, grouped: "Union[GroupBy, LazyGroupBy]", window: int) -> None:
        if window < 1:
            raise ValueError("rolling: the window must be at least 1")
        self._grouped = grouped
        self._window = window

    def sum(self) -> "Union[DataFrame, LazyFrame]":
        return self._grouped._transform("rolling_sum", self._window)

    def mean(self) -> "Union[DataFrame, LazyFrame]":
        return self._grouped._transform("rolling_mean", self._window)

    def min(self) -> "Union[DataFrame, LazyFrame]":
        return self._grouped._transform("rolling_min", self._window)

    def max(self) -> "Union[DataFrame, LazyFrame]":
        return self._grouped._transform("rolling_max", self._window)

    def var(self) -> DataFrame:
        return self._series_kernel(lambda s: s.rolling_var(self._window))

    def std(self) -> DataFrame:
        return self._series_kernel(lambda s: s.rolling_std(self._window))

    def median(self) -> DataFrame:
        return self._series_kernel(lambda s: s.rolling_median(self._window))

    def quantile(self, q: float) -> DataFrame:
        return self._series_kernel(lambda s: s.rolling_quantile(self._window, q))

    def _series_kernel(self, kernel: "Callable[[Series], Series]") -> DataFrame:
        # The Series rolling kernel over the frame sorted by group (stable, so
        # each group keeps its input order), then the rows back in input order.
        if not isinstance(self._grouped, GroupBy):
            raise TypeError("rolling: var / std / median / quantile need an eager group-by")
        g = self._grouped
        frame = g._frame()
        cols = g._value_columns()
        row = "__dftu_group_row__"
        ordered = frame.with_row_index(row).sort_by_multi(g._keys + [row])
        # The engine's rolling sum is null exactly where the window is short
        # of present values, so it masks the kernel's value.
        mask = GroupBy(ordered, g._keys)._transform("rolling_sum", self._window)
        out = ordered
        for c in cols:
            m = g._col(mask, c)
            out = out.with_column(c, kernel(g._col(ordered, c)) + (m - m))
        return out.sort_by(row).select(*cols)


class _GroupSeriesWindow:
    """``GroupBy.expanding()`` / ``ewm(alpha)``: the Series window per group
    (a stateful scan, so each group runs on its own rows), the values back
    in input order."""

    __slots__ = ("_grouped", "_window")

    def __init__(self, grouped: GroupBy, window: "Callable[[Series], object]") -> None:
        self._grouped = grouped
        self._window = window

    def _reduce(self, name: str) -> DataFrame:
        g = self._grouped
        frame = g._frame()
        cols = g._value_columns()
        row = "__dftu_group_row__"
        parts: List[DataFrame] = []
        for _, rows in g._groups_in_order():
            part = frame.take(rows).with_column(row, Series.from_list(rows))
            for c in cols:
                part = part.with_column(c, getattr(self._window(g._col(part, c)), name)())
            parts.append(part.select(row, *cols))
        if not parts:
            return frame.select(*cols)
        out = parts[0].concat(*parts[1:]) if len(parts) > 1 else parts[0]
        return out.sort_by(row).select(*cols)

    def sum(self) -> DataFrame:
        return self._reduce("sum")

    def mean(self) -> DataFrame:
        return self._reduce("mean")

    def min(self) -> DataFrame:
        return self._reduce("min")

    def max(self) -> DataFrame:
        return self._reduce("max")

    def count(self) -> DataFrame:
        return self._reduce("count")

    def var(self) -> DataFrame:
        return self._reduce("var")

    def std(self) -> DataFrame:
        return self._reduce("std")


class _GroupResample:
    """``GroupBy.resample(rule)``: the group-by over keys + time bucket, every
    result sorted by key then bucket and indexed by them (the pandas
    ``(key, time)`` index)."""

    __slots__ = ("_grouped", "_order")

    def __init__(self, grouped: "Union[GroupBy, LazyGroupBy]", order: List[str]) -> None:
        self._grouped = grouped
        self._order = order

    def __getattr__(self, name: str) -> object:
        attr = getattr(self._grouped, name)
        if not callable(attr):
            return attr

        def call(*args: object, **kwargs: object) -> object:
            out = attr(*args, **kwargs)
            names = out.columns if hasattr(out, "columns") else None
            if names is not None and all(c in names for c in self._order):
                out = out.sort_by_multi(self._order)
                out._index = list(self._order)
            return out

        return call


class _MissingColumn(KeyError):
    """A column the group function reads that the frame lacks: an error in
    the function, not a body the trace cannot follow."""


class _GroupTrace:
    """The symbolic group a traced group function runs on: a column read is
    the column expression, so reductions become aggregates."""

    __slots__ = ("_columns",)

    def __init__(self, columns: Sequence[str]) -> None:
        self._columns = list(columns)

    def __getitem__(self, name: str) -> Expr:
        if name not in self._columns:
            raise _MissingColumn(f"no column named {name!r}")
        return _Col(name)

    def __getattr__(self, name: str) -> Expr:
        if name.startswith("_") or name not in self._columns:
            raise AttributeError(name)
        return _Col(name)

    @property
    def columns(self) -> List[str]:
        return list(self._columns)

    def size(self) -> Agg:
        return Agg("count", None)

    def __len__(self) -> int:
        raise TypeError("len(group) is a Python value; use group.size() for the aggregate")


def _trace_group(
    func: "Callable[[Any], object]", columns: Sequence[str], what: str = "GroupBy.apply"
) -> Optional[object]:
    """Run ``func`` on a symbolic group; the aggregate / expression it built,
    or None (with the fallback warning logged) when the body cannot be
    followed."""
    from ._apply import _trace_errors, _uses_identity_test, _warn_fallback

    if _uses_identity_test(func):
        _warn_fallback(what, func, "`is` cannot be traced")
        return None
    try:
        traced = func(_GroupTrace(columns))
    except _MissingColumn:
        raise
    except _trace_errors() as e:
        _warn_fallback(what, func, f"{type(e).__name__}: {e}")
        return None
    if not isinstance(traced, (Agg, Expr)):
        _warn_fallback(
            what,
            func,
            f"returned {type(traced).__name__}, not an aggregate or expression",
        )
        return None
    return traced


def _collect_columns(expr: Expr) -> List[str]:
    seen: List[str] = []

    def walk(e: Expr) -> None:
        if isinstance(e, _AggLeaf):
            return
        if isinstance(e, _Col):
            if e.name not in seen:
                seen.append(e.name)
        elif isinstance(e, _Bin):
            walk(e.left)
            walk(e.right)
        elif isinstance(e, _STRING_NODES + (_Prim, _Unary, _Clip, _Cast, _Fillna, _IsNull)):
            walk(e.arg)
        elif isinstance(e, _Cmp):
            walk(e.left)
        elif isinstance(e, _Logical):
            walk(e.left)
            if e.right is not None:
                walk(e.right)
        elif isinstance(e, _Select):
            walk(e.cond)
            walk(e.a)
            walk(e.b)
        elif isinstance(e, _ColumnOpNode):
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
_STRING_NODES = (
    _StrPred,
    _IContains,
    _ILike,
    _IsIn,
    _StrMap,
    _StrLen,
    _StrFind,
    _StrReplace,
    _StrSlice,
)


def _is_float(expr: Expr, cols: Dict[str, "_ext._Series"]) -> bool:
    if isinstance(expr, _Lit):
        return isinstance(expr.value, float)
    if isinstance(expr, _Col):
        return cols[expr.name].type in _FLOAT_TYPES
    if isinstance(expr, _Bin):
        if expr.op == "/":
            return True
        return _is_float(expr.left, cols) or _is_float(expr.right, cols)
    if isinstance(expr, _Unary):
        # log/sqrt/exp widen to float; is_* yield bool; the rest keep the input.
        if expr.name in _UNARY_BOOL:
            return False
        return expr.name in _UNARY_FLOAT or _is_float(expr.arg, cols)
    if isinstance(expr, (_Clip, _Fillna)):
        return _is_float(expr.arg, cols)
    if isinstance(expr, _Cast):
        return expr.type_id in _FLOAT_TYPES
    if isinstance(expr, _Select):
        return _is_float(expr.a, cols) or _is_float(expr.b, cols)
    if isinstance(expr, _STRING_NODES + (_Prim, _Cmp, _Logical, _IsNull)):
        return False  # primitives -> int64; comparisons/logical/string -> not float
    raise TypeError(f"unsupported column expression node {type(expr).__name__}")


# Discriminated result (isinstance-narrowable): a scalar or a native _Series.
class _ScalarVal(NamedTuple):
    value: _Scalar


class _ColVal(NamedTuple):
    value: "_ext._Series"


_EvalResult = Union[_ScalarVal, _ColVal]


def _need_col(r: _EvalResult, message: str) -> "_ext._Series":
    if isinstance(r, _ColVal):
        return r.value
    raise TypeError(message)


def _eval(expr: Expr, cols: Dict[str, "_ext._Series"], out_float: bool) -> _EvalResult:
    if isinstance(expr, _Lit):
        return _ScalarVal(float(expr.value) if out_float else int(expr.value))
    if isinstance(expr, _Col):
        vc = cols[expr.name]
        if out_float and vc.type not in _FLOAT_TYPES:
            vc = vc.cast(_FLOAT64)
        return _ColVal(vc)
    if isinstance(expr, _Bin):
        left = _eval(expr.left, cols, out_float)
        right = _eval(expr.right, cols, out_float)
        if isinstance(left, _ScalarVal) and isinstance(right, _ScalarVal):
            return _ScalarVal(_fold(expr.op, left.value, right.value))
        return _ColVal(_binop(expr.op, left, right))
    if isinstance(expr, _Prim):
        # Primitives run on a 64-bit integer column; evaluate the argument as an
        # int expression and cast if it is not already i64/u64.
        v = _need_col(
            _eval(expr.arg, cols, out_float=False),
            f"columnar {expr.name}() needs a column argument",
        )
        if v.type not in (_INT64, _UINT64):
            v = v.cast(_INT64)
        return _ColVal(v.prim(_PRIM_CODES[expr.name]))
    if isinstance(expr, _Cmp):
        if isinstance(expr.rhs, bool) or not isinstance(expr.rhs, (int, float)):
            raise TypeError(
                "in-memory comparison needs a numeric value; a string/bool "
                "comparison is filter-only (push it down with .filter()/.query())"
            )
        v = _need_col(
            _eval(expr.left, cols, out_float=_is_float(expr.left, cols)),
            "columnar comparison needs a column on the left",
        )
        return _ColVal(v.compare(_CMP_CODES[expr.op], expr.rhs))
    if isinstance(expr, _Logical):
        lv = _need_col(_eval(expr.left, cols, out_float=False), "columnar logical needs a column")
        if expr.op == "not" or expr.right is None:
            return _ColVal(lv.logical_not())
        rv = _need_col(_eval(expr.right, cols, out_float=False), "columnar logical needs a column")
        return _ColVal(lv.logical(_LOGICAL_CODES[expr.op], rv))
    raise TypeError(f"unsupported column expression node {type(expr).__name__}")


def _fold(op: str, a: _Scalar, b: _Scalar) -> _Scalar:
    if op == "+":
        return a + b
    if op == "-":
        return a - b
    if op == "*":
        return a * b
    return a / b


_COL_OP = {"+": "add", "-": "sub", "*": "mul", "/": "div"}
_SCALAR_OP = {"+": "add_scalar", "-": "sub_scalar", "*": "mul_scalar", "/": "div_scalar"}


def _binop(op: str, left: _EvalResult, right: _EvalResult) -> "_ext._Series":
    if isinstance(left, _ColVal) and isinstance(right, _ColVal):
        return getattr(left.value, _COL_OP[op])(right.value)
    if isinstance(left, _ColVal):  # col op scalar
        return getattr(left.value, _SCALAR_OP[op])(right.value)
    if isinstance(right, _ColVal) and op in ("+", "*"):  # scalar op col (commutes)
        return getattr(right.value, _SCALAR_OP[op])(left.value)
    raise NotImplementedError(f"scalar {op} column needs a vec kernel (not yet available)")
