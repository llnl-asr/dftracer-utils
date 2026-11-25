# SPDX-License-Identifier: MIT
"""Evaluate an :class:`~dftracer.utils.Expr` in memory: the native engine
for an expression over columns, and the Python evaluator for a constant one.

``Columnar(expr).apply(source)`` serializes the expression tree to a post-order
AST and hands it to the native engine (``vec_eval``), which does the type
inference, common-subexpression elimination, lowering and the fused chunked,
parallel evaluation. An expression with no column is folded here.
"""

from __future__ import annotations

import ast
from typing import TYPE_CHECKING, Dict, NamedTuple, Optional, Union, cast

from . import dftracer_utils_ext as _ext
from ._convert import _arrow_native
from ._enums import DType
from ._expr import (
    CMP_CODES,
    LOGICAL_CODES,
    PRIM_CODES,
    STRING_NODES,
    UNARY_BOOL,
    UNARY_FLOAT,
    Bin,
    Cast,
    Clip,
    Cmp,
    Col,
    Expr,
    Fillna,
    IsNull,
    Lit,
    Logical,
    Prim,
    Scalar,
    Select,
    Unary,
    collect_columns,
    emit_ast,
)
from ._series import Series

if TYPE_CHECKING:
    import pyarrow as pa
    from typing_extensions import TypeAlias

    from ._dataframe import DataFrame

__all__ = ["Columnar"]


class AstOp(NamedTuple):
    """One Python operator in an authored body, in every form a consumer needs."""

    node: type
    symbol: str
    """The operator as C and Python spell it."""
    name: str
    """The engine's name for the operation."""
    code: Optional[int]
    """The opcode in the engine's AST bytecode (``vec_eval`` and ``@each_batch``);
    None for an operator the engine has no opcode for."""


# The one table of the arithmetic and comparison operators that `apply`, the jit
# compiler and `jit.op` lower. `Mod` has no engine opcode.
ARITH_OPS: tuple[AstOp, ...] = (
    AstOp(ast.Add, "+", "add", 0),
    AstOp(ast.Sub, "-", "sub", 1),
    AstOp(ast.Mult, "*", "mul", 2),
    AstOp(ast.Div, "/", "div", 3),
    AstOp(ast.Mod, "%", "mod", None),
)
COMPARE_OPS: tuple[AstOp, ...] = (
    AstOp(ast.Gt, ">", "gt", 0),
    AstOp(ast.GtE, ">=", "ge", 1),
    AstOp(ast.Lt, "<", "lt", 2),
    AstOp(ast.LtE, "<=", "le", 3),
    AstOp(ast.Eq, "==", "eq", 4),
    AstOp(ast.NotEq, "!=", "ne", 5),
)

ARITH_SYMBOL: dict[type, str] = {o.node: o.symbol for o in ARITH_OPS}
ARITH_CODE: dict[type, int] = {o.node: o.code for o in ARITH_OPS if o.code is not None}
ENGINE_ARITH_SYMBOL: dict[type, str] = {n: ARITH_SYMBOL[n] for n in ARITH_CODE}
COMPARE_SYMBOL: dict[type, str] = {o.node: o.symbol for o in COMPARE_OPS}
COMPARE_NAME: dict[type, str] = {o.node: o.name for o in COMPARE_OPS}
COMPARE_CODE: dict[type, int] = {o.node: o.code for o in COMPARE_OPS if o.code is not None}

# A source of columns for in-memory evaluation: a DataFrame, a
# {name: Series} mapping, or a pyarrow Table imported at the Arrow boundary.
Source: TypeAlias = Union["DataFrame", Dict[str, "Series"], "pa.Table"]
# where(): a frame in, the same kind of frame out.
Frame: TypeAlias = Union["DataFrame", "pa.Table"]

_INT64 = int(DType.INT64)
_UINT64 = int(DType.UINT64)
_FLOAT64 = int(DType.FLOAT64)


class Columnar:
    """A value expression ready to evaluate on the DataFrame engine."""

    def __init__(self, expr: Expr) -> None:
        self._expr = expr
        self._columns = collect_columns(expr)

    @property
    def columns(self) -> list[str]:
        """Input column names this expression reads."""
        return list(self._columns)

    def apply(self, source: Source) -> Series:
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
            ast: list[tuple[object, ...]] = []
            self._serialize(self._expr, ast)
            inputs = [cols[name] for name in self._columns]
            return _ext.vec_eval(ast, inputs)
        # Constant expression (no columns): broadcast to the row count.
        out_float = _is_float(self._expr, cols)
        result = _eval(self._expr, cols, out_float)
        if isinstance(result, _ScalarVal):
            sized = cols or _first_column(source)
            return _broadcast_scalar(result.value, out_float, sized)
        return result.value

    # Serialize `expr` to a post-order AST (a list of (op, args...) tuples) that
    # the C++ engine rebuilds and compiles. Structural only - no type logic.
    def _serialize(self, expr: Expr, ast: list[tuple[object, ...]]) -> None:
        emit_ast(expr, self._columns.index, ast)

    def _to_vec_columns(self, source: Source) -> dict[str, Series]:
        return _import_vec_columns(self._columns, source)


def unwrap_source_columns(source: object) -> dict[str, Series]:
    """Every column of ``source`` (a frame, a native batch, a ``{name:
    Series}`` mapping or a pyarrow Table) as wrapped Series, by name."""
    native = source
    if isinstance(native, dict):
        return dict(native)
    if isinstance(native, _ext.DataFrame):
        return {name: _ext.DataFrame.__getitem__(native, name) for name in native.column_names}
    frame = _arrow_frame(native)
    return {name: _ext.DataFrame.__getitem__(frame, name) for name in frame.column_names}


def _arrow_frame(table: object) -> _ext.DataFrame:
    """A pyarrow Table (any ``__arrow_c_stream__`` provider) as a native frame
    read through its stream, so its chunks are shared, not joined."""
    frame: _ext.DataFrame = _arrow_native("_dataframe_from_arrow")(table)
    return frame


def _first_column(source: Source) -> dict[str, Series]:
    """One column of ``source`` to give a constant expression its length (empty
    when the source has no column)."""
    native = source
    names = list(native)[:1] if isinstance(native, dict) else list(native.column_names)[:1]
    return _import_vec_columns(names, source) if names else {}


def _import_vec_columns(names: list[str], source: Source) -> dict[str, Series]:
    """Resolve `names` to native vec columns from `source` (a native batch or a
    pyarrow Table), importing at the Arrow boundary only when needed."""
    # A native batch (a DataFrame or a {name: Series} dict) is used directly.
    if isinstance(source, dict):
        return {name: source[name] for name in names}
    if isinstance(source, _ext.DataFrame):
        return {name: _ext.DataFrame.__getitem__(source, name) for name in names}
    # A pyarrow Table: only the named columns, each read through its stream
    # at the boundary, so their chunks are shared.
    table = cast("pa.Table", source)
    return {name: _arrow_native("_series_from_arrow")(table.column(name)) for name in names}


def _broadcast_scalar(value: Scalar, out_float: bool, cols: dict[str, Series]) -> Series:
    # Only the length of an input column is used, never its values: a null,
    # NaN or infinity in that column must not reach the constant. An all-null
    # column of the target type, filled with the value, has no other source.
    if not cols:
        raise ValueError("a constant columnar expression needs at least one column")
    n = len(next(iter(cols.values())))
    return _ext._series_nulls(int(_FLOAT64 if out_float else _INT64), n).fill_null(value)


def _is_float(expr: Expr, cols: dict[str, Series]) -> bool:
    if isinstance(expr, Lit):
        return isinstance(expr.value, float)
    if isinstance(expr, Col):
        return DType(cols[expr.name].type).is_float
    if isinstance(expr, Bin):
        if expr.op == "/":
            return True
        return _is_float(expr.left, cols) or _is_float(expr.right, cols)
    if isinstance(expr, Unary):
        # log/sqrt/exp widen to float; is_* yield bool; the rest keep the input.
        if expr.name in UNARY_BOOL:
            return False
        return expr.name in UNARY_FLOAT or _is_float(expr.arg, cols)
    if isinstance(expr, (Clip, Fillna)):
        return _is_float(expr.arg, cols)
    if isinstance(expr, Cast):
        return DType(expr.type_id).is_float
    if isinstance(expr, Select):
        return _is_float(expr.a, cols) or _is_float(expr.b, cols)
    if isinstance(expr, STRING_NODES + (Prim, Cmp, Logical, IsNull)):
        return False  # primitives -> int64; comparisons/logical/string -> not float
    raise TypeError(f"unsupported column expression node {type(expr).__name__}")


# Discriminated result (isinstance-narrowable): a scalar or a native _Series.
class _ScalarVal(NamedTuple):
    value: Scalar


class _ColVal(NamedTuple):
    value: Series


_EvalResult = Union[_ScalarVal, _ColVal]


def _need_col(r: _EvalResult, message: str) -> Series:
    if isinstance(r, _ColVal):
        return r.value
    raise TypeError(message)


def _eval(expr: Expr, cols: dict[str, Series], out_float: bool) -> _EvalResult:
    if isinstance(expr, Lit):
        return _ScalarVal(float(expr.value) if out_float else int(expr.value))
    if isinstance(expr, Col):
        vc = cols[expr.name]
        if out_float and not DType(vc.type).is_float:
            vc = vc.cast(_FLOAT64)
        return _ColVal(vc)
    if isinstance(expr, Bin):
        left = _eval(expr.left, cols, out_float)
        right = _eval(expr.right, cols, out_float)
        if isinstance(left, _ScalarVal) and isinstance(right, _ScalarVal):
            return _ScalarVal(_fold(expr.op, left.value, right.value))
        return _ColVal(_binop(expr.op, left, right))
    if isinstance(expr, Prim):
        # Primitives run on a 64-bit integer column; evaluate the argument as an
        # int expression and cast if it is not already i64/u64.
        v = _need_col(
            _eval(expr.arg, cols, out_float=False),
            f"columnar {expr.name}() needs a column argument",
        )
        if v.type not in (_INT64, _UINT64):
            v = v.cast(_INT64)
        return _ColVal(v.prim(PRIM_CODES[expr.name]))
    if isinstance(expr, Cmp):
        if isinstance(expr.rhs, Expr):
            raise TypeError("a comparison of two expressions needs at least one column")
        if isinstance(expr.rhs, bool) or not isinstance(expr.rhs, (int, float)):
            raise TypeError(
                "in-memory comparison needs a numeric value; a string/bool "
                "comparison is filter-only (push it down with .filter()/.duql())"
            )
        v = _need_col(
            _eval(expr.left, cols, out_float=_is_float(expr.left, cols)),
            "columnar comparison needs a column on the left",
        )
        return _ColVal(_ext.Series.compare(v, CMP_CODES[expr.op], expr.rhs))
    if isinstance(expr, Logical):
        lv = _need_col(_eval(expr.left, cols, out_float=False), "columnar logical needs a column")
        if expr.op == "not" or expr.right is None:
            return _ColVal(lv.logical_not())
        rv = _need_col(_eval(expr.right, cols, out_float=False), "columnar logical needs a column")
        return _ColVal(lv.logical(LOGICAL_CODES[expr.op], rv))
    raise TypeError(f"unsupported column expression node {type(expr).__name__}")


def _fold(op: str, a: Scalar, b: Scalar) -> Scalar:
    if op == "+":
        return a + b
    if op == "-":
        return a - b
    if op == "*":
        return a * b
    return a / b


_COL_OP = {"+": "add", "-": "sub", "*": "mul", "/": "div"}


_SCALAR_OP = {"+": "add_scalar", "-": "sub_scalar", "*": "mul_scalar", "/": "div_scalar"}


def _binop(op: str, left: _EvalResult, right: _EvalResult) -> Series:
    if isinstance(left, _ColVal) and isinstance(right, _ColVal):
        return cast("Series", getattr(_ext.Series, _COL_OP[op])(left.value, right.value))
    if isinstance(left, _ColVal):  # col op scalar
        return cast("Series", getattr(_ext.Series, _SCALAR_OP[op])(left.value, right.value))
    if isinstance(right, _ColVal) and op in ("+", "*"):  # scalar op col (commutes)
        return cast("Series", getattr(_ext.Series, _SCALAR_OP[op])(right.value, left.value))
    raise NotImplementedError(f"scalar {op} column needs a vec kernel (not yet available)")
