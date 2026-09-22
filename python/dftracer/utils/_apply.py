"""``apply`` / ``map`` over a user function, engine-first.

A Python callable is tried in three tiers, fastest first:

1. Trace. The function is called once with a symbolic column (or a symbolic
   row whose fields are columns); if it returns a columnar ``Expr``, the whole
   thing runs fused in the engine with no per-row Python. This is what
   ``@jit.series`` does at decoration time, done at call time instead.
2. Transpile. When tracing cannot see through the function (a Python ``if``
   on a value), its source is lowered to an ``Expr`` by ``_transpile``,
   control flow and calls to other plain functions included. A ``@jit.op``
   value transform lowers to the same engine when its body allows, else runs
   its compiled graph per element.
3. Fall back to plain Python per element (or per row), with a warning naming
   the construct that kept the function out of the engine, so the slow path
   is never silent.

On every tier a null element maps to a null result for ``Series.apply`` /
``map``; a row function sees the engine's rule (a null test takes the else
arm) on the engine tiers and ``None`` values on the Python tier.
"""

from __future__ import annotations

import logging
from typing import TYPE_CHECKING, Any, Callable, Dict, List, Mapping, Optional, Sequence, cast

from . import dftracer_utils_ext as _ext

if TYPE_CHECKING:
    from .columnar import Expr
    from .dataframe import DataFrame
    from .series import Series

_log = logging.getLogger("dftracer.utils")

_TRACE_INPUT = "__x0__"


def _trace_errors() -> tuple:
    from .jit import JitError

    return (TypeError, AttributeError, ValueError, KeyError, NotImplementedError, JitError)


def _is_jit_series(func: object) -> bool:
    return callable(func) and str(getattr(func, "__qualname__", "")).startswith("ops.")


def _is_jit_op(func: object) -> bool:
    from .jit_op import Op

    return isinstance(func, Op)


def _warn_fallback(what: str, func: object, reason: str) -> None:
    name = getattr(func, "__name__", repr(func))
    _log.warning(
        "%s: '%s' could not be traced into the engine (%s); running it in Python "
        "per %s, which is orders of magnitude slower. Write it over column "
        "expressions, or decorate it with @jit.series / @jit.op.",
        what,
        name,
        reason,
        "group" if what.startswith("GroupBy") else "row" if what.endswith("axis=1)") else "element",
    )


def _eval_expr(expr: "Expr", source: Mapping[str, "Series"]) -> "Series":
    from .columnar import Columnar

    return Columnar(expr).apply(dict(source))


def _from_python(values: Sequence[object]) -> "Series":
    from .series import Series

    return Series.from_list(list(values))


def _column(frame: "DataFrame", name: str) -> "Series":
    from .series import Series

    return Series(frame._native[name])


def apply_series(series: "Series", func: Callable[..., object], what: str) -> "Series":
    """Elementwise ``func`` over ``series`` (pandas ``Series.apply`` / ``map``)."""
    from .columnar import col

    if _is_jit_series(func):
        from .series import Series

        out = func(series)
        if not isinstance(out, Series):
            raise TypeError(
                f"{what}: the jit.series op returned {type(out).__name__}, not a Series"
            )
        return out
    if _is_jit_op(func):
        return _apply_jit_op(series, func, what)

    source = {_TRACE_INPUT: series}
    expr, reason = _trace(func, col(_TRACE_INPUT), source)
    if expr is not None:
        return _null_where_input_is(_eval_expr(expr, source), series)
    expr, reason2 = _transpile(func, None, source)
    if expr is not None:
        return _null_where_input_is(_eval_expr(expr, source), series)

    _warn_fallback(what, func, reason2 or reason)
    values = [None if v is None else func(v) for v in series.to_list()]
    return _from_python(values)


def _null_where_input_is(out: "Series", series: "Series") -> "Series":
    """A null element maps to a null result on every tier. The engine's own
    rule (a null test takes the else arm) would otherwise let a branch turn a
    missing value into a present one."""
    if series.null_count == 0:
        return out
    nulls = series.isna().to_list()
    return out.take([-1 if nulls[i] else i for i in range(len(nulls))])


def _trace(
    func: Callable[..., object], arg: object, source: Mapping[str, "Series"]
) -> "tuple[Optional[Expr], str]":
    """Tier 1: run ``func`` once on a symbolic argument. The Expr it built, or
    None and the reason."""
    from .columnar import Expr

    if _uses_identity_test(func):
        return None, "`is` cannot be traced; lowered from source instead"
    try:
        traced = func(arg)
    except _trace_errors() as e:
        return None, f"{type(e).__name__}: {e}"
    if not isinstance(traced, Expr):
        return None, f"returned {type(traced).__name__}, not a column expression"
    try:
        _check_columns(traced, source)
    except _trace_errors() as e:
        return None, f"{type(e).__name__}: {e}"
    return traced, ""


def _uses_identity_test(func: Callable[..., object]) -> bool:
    """Whether the function's source holds an ``is`` / ``is not`` test. Python
    resolves ``is`` on the tracer object itself (never a hook), so tracing
    would fold ``x is None`` to False; the source lowering reads it as the
    null mask."""
    import ast

    from ._transpile import TranspileError, _source_node

    try:
        node = _source_node(func)
    except TranspileError:
        return False
    return any(
        isinstance(op, (ast.Is, ast.IsNot))
        for n in ast.walk(node)
        if isinstance(n, ast.Compare)
        for op in n.ops
    )


def _transpile(
    func: Callable[..., object],
    columns: Optional[Sequence[str]],
    source: Mapping[str, "Series"],
) -> "tuple[Optional[Expr], str]":
    """Tier 2: lower the function's own source, control flow included."""
    from ._transpile import TranspileError, transpile
    from .enums import DType

    ints = {
        DType.INT8,
        DType.INT16,
        DType.INT32,
        DType.INT64,
        DType.UINT8,
        DType.UINT16,
        DType.UINT32,
        DType.UINT64,
    }
    int_inputs = {name: s.dtype in ints for name, s in source.items()}
    try:
        expr = transpile(func, columns, int_inputs)
        _check_columns(expr, source)
    except TranspileError as e:
        return None, f"not compilable: {e}"
    except _trace_errors() as e:
        return None, f"{type(e).__name__}: {e}"
    return expr, ""


def _check_columns(expr: "Expr", source: Mapping[str, "Series"]) -> None:
    from .columnar import _collect_columns

    for name in _collect_columns(expr):
        if name not in source:
            raise KeyError(f"no column named {name!r}")


def _apply_jit_op(series: "Series", op_obj: Any, what: str) -> "Series":
    from .jit_op import lower_to_expr, run_op

    tokens = lower_to_expr(op_obj)
    if tokens is not None:
        from .series import Series

        return Series(_ext.vec_eval(tokens, [series._native]))
    _warn_fallback(what, op_obj, "the @jit.op body has no columnar Expr form")
    values: List[object] = [
        None if v is None else run_op(op_obj, cast(float, v)) for v in series.to_list()
    ]
    return _from_python(values)


class _RowTrace:
    """The symbolic row handed to ``func`` on the trace tier: each field read
    is the column expression for that name."""

    __slots__ = ("_columns",)

    def __init__(self, columns: Sequence[str]) -> None:
        self._columns = list(columns)

    def __getitem__(self, name: str) -> object:
        from .columnar import col

        if name not in self._columns:
            raise KeyError(f"no column named {name!r}")
        return col(name)

    def __getattr__(self, name: str) -> object:
        if name.startswith("_"):
            raise AttributeError(name)
        return self[name]

    def keys(self) -> List[str]:
        return list(self._columns)

    def __contains__(self, name: object) -> bool:
        return name in self._columns


class Row(Mapping[str, object]):
    """One row on the Python fallback tier: a read-only mapping of column name
    to value, with attribute access (``row.dur``) like a pandas row."""

    __slots__ = ("_values",)

    def __init__(self, values: Dict[str, object]) -> None:
        self._values = values

    def __getitem__(self, name: str) -> object:
        return self._values[name]

    def __getattr__(self, name: str) -> object:
        if name.startswith("_"):
            raise AttributeError(name)
        try:
            return self._values[name]
        except KeyError:
            raise AttributeError(name) from None

    def __iter__(self):
        return iter(self._values)

    def __len__(self) -> int:
        return len(self._values)

    def __repr__(self) -> str:
        return f"Row({self._values!r})"


def apply_rows(frame: "DataFrame", func: Callable[..., object], what: str) -> "Series":
    """``func`` once per row (pandas ``DataFrame.apply(axis=1)``)."""

    columns = frame.columns
    source = {name: _column(frame, name) for name in columns}
    expr, reason = _trace(func, _RowTrace(columns), source)
    if expr is not None:
        return _eval_expr(expr, source)
    expr, reason2 = _transpile(func, columns, source)
    if expr is not None:
        return _eval_expr(expr, source)

    _warn_fallback(what, func, reason2 or reason)
    lists = {name: _column(frame, name).to_list() for name in columns}
    n = frame.height
    values = [func(Row({name: lists[name][i] for name in columns})) for i in range(n)]
    return _from_python(values)
