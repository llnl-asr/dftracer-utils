"""The column ops of the expression language: a registered ``dftu.series.*``
op applied to a value inside an expression, the polars spellings
(``col("x").cum_sum()``, ``.rolling_mean(3)``, ``.sort()``, ``.over("k")``,
``.str.*``, ``.dt.*``).

A column op reads the whole column at once, so it is not a node of the
chunked expression evaluator. Eagerly (``Expr.apply``) each one runs the
Series op on its argument's value and the result stands in as a column of
the rest of the expression; in a plan it is the ``dftu.frame.column_op``
step (a breaker), the argument first computed into a temporary column.
``over(keys)`` is the group-wise form: the engine's group transform for the
kinds it has, an aggregate joined back on the keys otherwise."""

from __future__ import annotations

import re
from typing import TYPE_CHECKING, Dict, List, Optional, Tuple, Union

from . import dftracer_utils_ext as _ext
from .series import Series

if TYPE_CHECKING:
    from .columnar import Agg, Expr
    from .lazyframe import LazyFrame

Value = Union[int, float]

# The group transform behind an ``over`` for the kinds the engine has, keyed
# by the column op's registry name.
_TRANSFORM_KINDS: Dict[str, str] = {
    "dftu.series.cumsum": "cumsum",
    "dftu.series.cum_prod": "cumprod",
    "dftu.series.cummax": "cummax",
    "dftu.series.cummin": "cummin",
    "dftu.series.cum_count": "cumcount",
    "dftu.series.shift": "shift",
    "dftu.series.diff": "diff",
    "dftu.series.pct_change": "pct_change",
    "dftu.series.rank": "rank",
    "dftu.series.ffill": "ffill",
    "dftu.series.bfill": "bfill",
}

_RANK_METHODS = {"average": 0, "min": 1, "dense": 2, "ordinal": 3, "first": 3, "max": 4}
_ROLLING_OPS = {"sum": 0, "mean": 1, "min": 2, "max": 3}


class ColumnOp:
    """One registered column op and its operands, as the expression language
    carries it: ``op`` is the registry name, ``column2`` a second column
    operand (an expression), ``a`` / ``b`` the scalar operands, ``text`` a
    string or char operand, in the op's signature order."""

    __slots__ = ("op", "column2", "a", "b", "text", "over", "kind", "n", "agg")

    def __init__(
        self,
        op: str,
        column2: "Optional[Expr]" = None,
        a: Value = 0,
        b: Value = 0,
        text: Optional[str] = None,
        over: Optional[List[str]] = None,
        kind: Optional[str] = None,
        n: int = 0,
        agg: "Optional[Agg]" = None,
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


def _tokens(op: str) -> Tuple[List[str], str]:
    info = _ext.op_info(op)
    m = re.match(r"\((.*)\) -> (\w+)", str(info["signature"]))
    assert m is not None
    return [t.strip() for t in m.group(1).split(",") if t.strip()], m.group(2)


def run_column_op(series: Series, spec: ColumnOp, series2: Optional[Series]) -> Series:
    """``spec`` over ``series`` through the registry, the operands placed by
    the op's signature tokens exactly as the lazy ``column_op`` step places
    them."""
    tokens, ret = _tokens(spec.op)
    if not tokens or tokens[0] != "series" or ret != "series":
        raise TypeError(f"{spec.op}: not a column -> column op")
    scalars = [spec.a, spec.b]
    args: List[object] = []
    used_text = False
    for tok in tokens[1:]:
        if tok == "series":
            if series2 is None:
                raise TypeError(f"{spec.op}: needs a second column")
            args.append(series2._native)
        elif tok in (
            "i64",
            "u64",
            "i32",
            "cmp",
            "prim",
            "logical",
            "dtype",
            "reduce",
            "rank",
            "rolling",
        ):
            if not scalars:
                raise TypeError(f"{spec.op}: needs a scalar operand")
            args.append(int(scalars.pop(0)))
        elif tok == "f64":
            if not scalars:
                raise TypeError(f"{spec.op}: needs a scalar operand")
            args.append(float(scalars.pop(0)))
        elif tok == "scalar":
            if not scalars:
                raise TypeError(f"{spec.op}: needs a scalar operand")
            args.append(scalars.pop(0))
        elif tok in ("str", "char"):
            if spec.text is None or used_text:
                raise TypeError(f"{spec.op}: needs a text operand")
            args.append(spec.text if tok == "str" else spec.text[0])
            used_text = True
        else:
            raise TypeError(f"{spec.op}: operand {tok!r} cannot ride a column op")
    out = _ext.op_run(spec.op, series._native, *args)
    if not isinstance(out, _ext._Series):
        raise TypeError(f"{spec.op}: not a column -> column op")
    return Series(out)


def has_column_op(expr: "Expr") -> bool:
    from .columnar import Expr, _ColumnOpNode

    if isinstance(expr, _ColumnOpNode):
        return True
    return any(has_column_op(v) for v in vars(expr).values() if isinstance(v, Expr))


def split_column_ops(expr: "Expr") -> "Tuple[Expr, List[Tuple[str, Expr, ColumnOp]]]":
    """The expression with every column op replaced by a column reference
    ``__dftu_cop_<i>__``, and the ops so named in evaluation order (an op's
    argument has its own ops replaced first, so each entry's argument is a
    plain expression over columns, the earlier temporaries included)."""
    import copy

    from .columnar import Expr, _Col, _ColumnOpNode, _Lit

    ops: List[Tuple[str, Expr, ColumnOp]] = []

    def rewrite(e: Expr) -> Expr:
        if isinstance(e, (_Col, _Lit)):
            return e
        if isinstance(e, _ColumnOpNode):
            arg = rewrite(e.arg)
            spec = e.spec
            column2 = spec.column2
            if column2 is not None:
                spec = copy.copy(spec)
                spec.column2 = rewrite(column2)
            name = f"__dftu_cop_{len(ops)}__"
            ops.append((name, arg, spec))
            return _Col(name)
        node = copy.copy(e)
        for key, value in list(vars(node).items()):
            if isinstance(value, Expr):
                setattr(node, key, rewrite(value))
        return node

    return rewrite(expr), ops


# ---- eager -----------------------------------------------------------------


def _as_columns(source: object) -> Dict[str, Series]:
    from .columnar import _unwrap_source_columns

    return _unwrap_source_columns(source)


def eval_with_column_ops(expr: "Expr", source: object) -> Series:
    """``Expr.apply`` for an expression holding column ops: each op's value
    joins the source under its temporary name, then the rest evaluates."""
    from .columnar import Columnar, _Col

    columns = _as_columns(source)
    rewritten, ops = split_column_ops(expr)
    for name, arg, spec in ops:
        if spec.agg is not None:
            columns[name] = _eager_agg_over(columns, spec)
            continue
        value = columns[arg.name] if isinstance(arg, _Col) else Columnar(arg).apply(columns)
        second = None
        if spec.column2 is not None:
            second = (
                columns[spec.column2.name]
                if isinstance(spec.column2, _Col)
                else Columnar(spec.column2).apply(columns)
            )
        columns[name] = (
            run_column_op(value, spec, second)
            if spec.over is None
            else _eager_over(columns, value, spec, second)
        )
    if isinstance(rewritten, _Col):
        return columns[rewritten.name]
    return Columnar(rewritten).apply(columns)


def _eager_over(
    columns: Dict[str, Series], value: Series, spec: ColumnOp, second: Optional[Series]
) -> Series:
    from .columnar import GroupBy
    from .dataframe import DataFrame

    keys = spec.over or []
    if spec.kind is None:
        raise TypeError(f"{spec.op}: over() has no group-wise form for this op")
    frame = DataFrame({**{k: columns[k] for k in keys}, "__dftu_v__": value})
    out = GroupBy(frame, keys)._transform(spec.kind, spec.n, _rank_name(spec), _ascending(spec))
    return Series(out._native["__dftu_v__"])


def _eager_agg_over(columns: Dict[str, Series], spec: ColumnOp) -> Series:
    from .columnar import GroupBy
    from .dataframe import DataFrame

    assert spec.agg is not None
    keys = spec.over or []
    frame = DataFrame({k: v for k, v in columns.items() if not k.startswith("__dftu_cop_")})
    value = "__dftu_over_v__"
    grouped = GroupBy(frame, keys).agg(spec.agg.alias(value))
    if not keys:
        v = Series(grouped._native[value])[0]
        assert isinstance(v, (int, float))
        first = next(iter(columns.values()))
        return first.full_like(float(v))
    row = "__dftu_over_row__"
    joined = frame.with_row_index(row).join(grouped, on=keys).sort_values(row)
    return Series(joined._native[value])


def _rank_name(spec: ColumnOp) -> str:
    if spec.op != "dftu.series.rank":
        return "average"
    return {v: k for k, v in _RANK_METHODS.items() if k != "first"}[int(spec.a)]


def _ascending(spec: ColumnOp) -> bool:
    return not (spec.op == "dftu.series.rank" and int(spec.b))


# ---- lazy ------------------------------------------------------------------


def lower_column_ops(plan: "LazyFrame", expr: "Expr") -> "Tuple[LazyFrame, Expr, List[str]]":
    """The plan with every column op of ``expr`` appended as steps (each
    argument computed into a temporary column, the op run over it in
    place), the expression rewritten over those temporaries, and the
    temporaries to drop once the caller has used the expression."""
    from .columnar import _Col
    from .lazyframe import LazyFrame

    rewritten, ops = split_column_ops(expr)
    temps: List[str] = []
    for name, arg, spec in ops:
        if spec.agg is not None:
            plan = _lazy_agg_over(plan, name, spec)
            temps.append(name)
            continue
        plan = plan.with_column(name, arg)
        temps.append(name)
        column2 = None
        if spec.column2 is not None:
            if isinstance(spec.column2, _Col):
                column2 = spec.column2.name
            else:
                column2 = f"{name}b"
                plan = plan.with_column(column2, spec.column2)
                temps.append(column2)
        if spec.over is None:
            plan = LazyFrame(
                plan._native.column_op(
                    name, spec.op, column2=column2, a=spec.a, b=spec.b, text=spec.text
                )
            )
        else:
            plan = _lazy_over(plan, name, spec)
    return plan, rewritten, temps


def _lazy_agg_over(plan: "LazyFrame", name: str, spec: ColumnOp) -> "LazyFrame":
    from .lazyframe import LazyGroupBy

    assert spec.agg is not None
    keys = spec.over or []
    row = "__dftu_over_row__"
    indexed = plan.with_row_index(row)
    grouped = LazyGroupBy(indexed, keys).agg(spec.agg.alias(name))
    if not keys:
        raise TypeError("over: an aggregate's over() needs keys on a plan")
    return indexed.join(grouped, on=keys).sort_by(row).drop(row)


def _lazy_over(plan: "LazyFrame", name: str, spec: ColumnOp) -> "LazyFrame":
    from .lazyframe import LazyGroupBy

    if spec.kind is None:
        raise TypeError(f"{spec.op}: over() has no group-wise form for this op")
    keys = spec.over or []
    row = "__dftu_over_row__"
    indexed = plan.with_row_index(row)
    part = indexed.select(*(keys + [row, name]))
    valued = LazyGroupBy(part.select(*(keys + [name])), keys)._transform(
        spec.kind, spec.n, _rank_name(spec), _ascending(spec)
    )
    # The transform keeps input order, so the row index lines up positionally.
    valued = valued.with_row_index(row)
    joined = indexed.drop(name).join(valued.select(row, name), on=row, how="left")
    return joined.sort_by(row).drop(row)
