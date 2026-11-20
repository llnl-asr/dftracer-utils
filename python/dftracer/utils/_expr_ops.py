# SPDX-License-Identifier: MIT
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

import copy
import re
from typing import TYPE_CHECKING, cast

from . import dftracer_utils_ext as _ext
from ._eval import Columnar, Source, unwrap_source_columns
from .dataframe import DataFrame
from .enums import EPOCH_NS, DType, dt_unit, rule_to_units
from .expr import (
    CAST_CODES,
    RANK_METHODS,
    Col,
    ColumnOp,
    Expr,
    has_column_op,
    split_column_ops,
)
from .groupby import GroupBy
from .series import Series

if TYPE_CHECKING:
    from .lazyframe import LazyFrame, LazyGroupBy


def _tokens(op: str) -> tuple[list[str], str]:
    info = _ext.op_info(op)
    m = re.match(r"\((.*)\) -> (\w+)", str(info["signature"]))
    assert m is not None
    return [t.strip() for t in m.group(1).split(",") if t.strip()], m.group(2)


def _round_every(spec: ColumnOp, unit: str) -> int:
    assert spec.text is not None
    return rule_to_units(spec.text, unit)


def resolve_round(spec: ColumnOp, unit: str) -> ColumnOp:
    """``spec`` with its dt_round rule as a count of ``unit``, unchanged when
    the rule is already a count."""
    if spec.op != "dftu.series.dt_round" or spec.text is None:
        return spec
    return ColumnOp(spec.op, spec.column2, _round_every(spec, unit), spec.b)


def is_epoch(spec: ColumnOp) -> bool:
    return spec.kind == "epoch"


def epoch_steps(spec: ColumnOp, own: str) -> list[ColumnOp]:
    """The column ops that turn a temporal column counted in ``own`` ("d" for
    a date32) into integers of the unit ``spec.text``: a cast to Int64, then a
    multiplication or a floor division by the ratio of the two units."""
    assert spec.text is not None
    want, have = EPOCH_NS[spec.text], EPOCH_NS[own]
    steps = [ColumnOp("dftu.series.cast", None, CAST_CODES["int64"])]
    if have > want:
        steps.append(ColumnOp("dftu.series.mul_scalar", None, have // want))
    elif have < want:
        steps.append(ColumnOp("dftu.series.floordiv_scalar", None, want // have, 0))
    return steps


def _own_unit(kind: int, time_unit: int, given: str | None) -> str:
    if given is None and kind == DType.DATE32:
        return "d"
    return dt_unit(kind, time_unit, given)


def _plan_column(plan: LazyFrame, name: str) -> tuple[int, int]:
    """The type code and the time unit code of column ``name`` of a plan."""
    for column, kind, time_unit, _zone in _ext.LazyFrame.output_schema(plan):
        if column == name:
            return kind, time_unit
    raise ValueError(
        f"dt: the column type of {name!r} is not known before the plan runs "
        "(after a data-dependent op); collect first"
    )


def run_column_op(series: Series, spec: ColumnOp, series2: Series | None) -> Series:
    """``spec`` over ``series`` through the registry, the operands placed by
    the op's signature tokens exactly as the lazy ``column_op`` step places
    them."""
    if is_epoch(spec):
        for step in epoch_steps(spec, _own_unit(series.type, series.time_unit, spec.unit)):
            series = run_column_op(series, step, None)
        return series
    tokens, ret = _tokens(spec.op)
    if not tokens or tokens[0] != "series" or ret != "series":
        raise TypeError(f"{spec.op}: not a column -> column op")
    if spec.op == "dftu.series.dt_round" and spec.text is not None:
        spec = resolve_round(spec, dt_unit(series.type, series.time_unit, spec.unit))
    scalars = [spec.a, spec.b]
    args: list[object] = []
    used_text = False
    for tok in tokens[1:]:
        if tok == "series":
            if series2 is None:
                raise TypeError(f"{spec.op}: needs a second column")
            args.append(series2)
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
    out = _ext.op_run(spec.op, series, *args)
    if not isinstance(out, Series):
        raise TypeError(f"{spec.op}: not a column -> column op")
    return out


# ---- eager -----------------------------------------------------------------


def _as_columns(source: object) -> dict[str, Series]:
    return unwrap_source_columns(source)


def eval_with_column_ops(expr: Expr, source: object) -> Series:
    """``Expr.apply`` for an expression holding column ops: each op's value
    joins the source under its temporary name, then the rest evaluates."""
    columns = _as_columns(source)
    rewritten, ops = split_column_ops(expr)
    for name, arg, spec in ops:
        if spec.agg is not None:
            columns[name] = _eager_agg_over(columns, spec)
            continue
        value = columns[arg.name] if isinstance(arg, Col) else Columnar(arg).apply(columns)
        second = None
        if spec.column2 is not None:
            second = (
                columns[spec.column2.name]
                if isinstance(spec.column2, Col)
                else Columnar(spec.column2).apply(columns)
            )
        columns[name] = (
            run_column_op(value, spec, second)
            if spec.over is None
            else _eager_over(columns, value, spec, second)
        )
    if isinstance(rewritten, Col):
        return columns[rewritten.name]
    return Columnar(rewritten).apply(columns)


def _eager_over(
    columns: dict[str, Series], value: Series, spec: ColumnOp, second: Series | None
) -> Series:
    keys = spec.over or []
    if spec.kind is None:
        raise TypeError(f"{spec.op}: over() has no group-wise form for this op")
    frame = DataFrame.from_dict({**{k: columns[k] for k in keys}, "__dftu_v__": value})
    out = GroupBy(frame, keys)._transform(spec.kind, spec.n, _rank_name(spec), _ascending(spec))
    return out["__dftu_v__"]


def _eager_agg_over(columns: dict[str, Series], spec: ColumnOp) -> Series:
    assert spec.agg is not None
    keys = spec.over or []
    frame = DataFrame.from_dict(
        {k: v for k, v in columns.items() if not k.startswith("__dftu_cop_")}
    )
    value = "__dftu_over_v__"
    grouped = GroupBy(frame, keys).agg(spec.agg.alias(value))
    if not keys:
        v = grouped[value][0]
        assert isinstance(v, (int, float))
        first = next(iter(columns.values()))
        return first.full_like(float(v))
    row = "__dftu_over_row__"
    # A null key is a group of its own, as in polars, so its rows match it.
    joined = (
        frame.with_row_index(row)
        .join(grouped, on=keys, how="left", nulls_equal=True)
        ._sort_values(row)
    )
    return joined[value]


def _rank_name(spec: ColumnOp) -> str:
    if spec.op != "dftu.series.rank":
        return "average"
    return {v: k for k, v in RANK_METHODS.items() if k != "first"}[int(spec.a)]


def _ascending(spec: ColumnOp) -> bool:
    return not (spec.op == "dftu.series.rank" and int(spec.b) & 1)


# ---- lazy ------------------------------------------------------------------


def lower_column_ops(plan: LazyFrame, expr: Expr) -> tuple[LazyFrame, Expr, list[str]]:
    """The plan with every column op of ``expr`` appended as steps (each
    argument computed into a temporary column, the op run over it in
    place), the expression rewritten over those temporaries, and the
    temporaries to drop once the caller has used the expression."""
    rewritten, ops = split_column_ops(expr)
    temps: list[str] = []
    for name, arg, spec in ops:
        if spec.agg is not None:
            plan = _lazy_agg_over(plan, name, spec)
            temps.append(name)
            continue
        plan = plan.with_column(name, arg)
        temps.append(name)
        column2 = None
        if spec.column2 is not None:
            if isinstance(spec.column2, Col):
                column2 = spec.column2.name
            else:
                column2 = f"{name}b"
                plan = plan.with_column(column2, spec.column2)
                temps.append(column2)
        if is_epoch(spec):
            kind, time_unit = _plan_column(plan, name)
            for step in epoch_steps(spec, _own_unit(kind, time_unit, spec.unit)):
                plan = _ext.LazyFrame._column_op(
                    plan, name, step.op, column2=None, a=step.a, b=step.b, text=None
                )
            continue
        if spec.op == "dftu.series.dt_round" and spec.text is not None:
            kind, time_unit = _plan_column(plan, name)
            spec = resolve_round(spec, dt_unit(kind, time_unit, spec.unit))
        if spec.over is None:
            plan = _ext.LazyFrame._column_op(
                plan, name, spec.op, column2=column2, a=spec.a, b=spec.b, text=spec.text
            )
        else:
            plan = _lazy_over(plan, name, spec)
    return plan, rewritten, temps


def _lazy_agg_over(plan: LazyFrame, name: str, spec: ColumnOp) -> LazyFrame:
    assert spec.agg is not None
    keys = spec.over or []
    if not keys:
        raise TypeError("over: an aggregate's over() needs keys on a plan")
    row = "__dftu_over_row__"
    indexed = plan.with_row_index(row)
    agg = spec.agg
    # The streaming group-by aggregates columns by name, so a computed value
    # becomes a column first.
    value = f"{name}_value"
    computed = agg.value is not None and not isinstance(agg.value, Col)
    if computed:
        indexed = indexed.with_column(value, cast("Expr", agg.value))
        agg = copy.copy(agg)
        agg.value = Col(value)
    grouped = cast("LazyGroupBy", indexed.group_by(keys, dropna=False)).agg(agg.alias(name))
    joined = indexed.join(grouped, on=keys, how="left", nulls_equal=True).sort_by(row)
    return joined.drop(row, value) if computed else joined.drop(row)


def _lazy_over(plan: LazyFrame, name: str, spec: ColumnOp) -> LazyFrame:
    if spec.kind is None:
        raise TypeError(f"{spec.op}: over() has no group-wise form for this op")
    keys = spec.over or []
    row = "__dftu_over_row__"
    indexed = plan.with_row_index(row)
    part = indexed.select(*(keys + [row, name]))
    # Null keys stay a group: the result lines up with the input by position.
    valued = cast(
        "LazyGroupBy", part.select(*(keys + [name])).group_by(keys, dropna=False)
    )._transform(spec.kind, spec.n, _rank_name(spec), _ascending(spec))
    # The transform keeps input order, so the row index lines up positionally.
    valued = valued.with_row_index(row)
    joined = indexed.drop(name).join(valued.select(row, name), on=row, how="left")
    return joined.sort_by(row).drop(row)


def apply(self: Expr, source: Source) -> Series:
    """Evaluate this expression on ``source`` and return a
    :class:`~dftracer.utils.Series`.

    A value expression yields a value Series; a numeric comparison yields a
    boolean-mask Series. ``source`` is a native ``DataFrame`` (e.g. from
    ``view.collect()``), a ``{name: Series}`` mapping, or a
    ``pyarrow.Table``. Filter-only predicates (string match, membership,
    string/bool comparison) raise; push those down with
    ``TraceViewer.filter()`` instead. See also :meth:`DataFrame.apply` for
    the frame-first spelling ``df.apply(expr)``."""
    if has_column_op(self):
        return eval_with_column_ops(self, source)
    return Columnar(self).apply(source)


setattr(Expr, "apply", apply)
