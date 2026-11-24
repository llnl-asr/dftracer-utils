# SPDX-License-Identifier: MIT
"""Compile a typed scalar Python function to a native scalar UDF.

``@dft.udf`` on ``def f(x: float, n: int) -> float`` is lowered here: the body is
checked against a small per-row subset, emitted as C (in a C++ file that the
plugin build compiles), built to a cached shared library that exports one
``dftu_udf_desc``, and registered by ``dftracer.utils._expr``. The compiled
UDF runs on the engine's workers and never takes the GIL.

Nulls: the host nulls the result of every row with a null argument, and the
compiled loop skips those rows. Types: ``float`` is ``Float64``, ``int`` is
``Int64`` and ``bool`` is ``Bool``. A ``float`` argument takes any numeric
column and an ``int`` argument any integer column, widened per row.

The result of an arithmetic step follows Python except where C is fixed width:
``int`` wraps at 64 bits, ``/`` and ``**`` give ``float``, ``/``, ``//`` and
``%`` by zero fail the call as Python's ZeroDivisionError does, a float ``//``
and ``%`` follow CPython's floored algorithm, and the ``math`` functions are
the C ones (``math.log(0)`` is ``-inf``, not an error). A ``Uint64`` column binds to an ``int`` argument,
and a value above 2**63-1 fails the call.
"""

from __future__ import annotations

import ast
import builtins
import math
from typing import Callable, NoReturn

from .. import _build
from .._c_text import c_str_literal
from .._eval import COMPARE_SYMBOL
from .._jit_errors import JitError, Source

BOOL, INT, FLOAT = 0, 1, 2
_TYPE_NAMES = {"bool": BOOL, "int": INT, "float": FLOAT}
_C_TYPES = {BOOL: "int", INT: "int64_t", FLOAT: "double"}
_ARG_TYPES = {BOOL: "DFTU_TYPE_BOOL", INT: "DFTU_TYPE_INT64", FLOAT: "DFTU_TYPE_FLOAT64"}

VOCABULARY = (
    "typed float, int and bool arguments, numbers, + - * / // % **, unary -, "
    "comparisons (a < x <= b), and, or, not, a if c else b, abs, min, max, float, int, bool, "
    "math.sqrt/exp/log/log2/log10/sin/cos/tan/floor/ceil/..., local variables, "
    "if/elif/else and return"
)
_BATCH_HINT = "; use @dft.udf(batch=True) for a body that runs Python on whole columns"

_PRELUDE = r"""
#include <dftracer/utils/dataframe/c_api_udf.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int64_t i_add(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }
static inline int64_t i_sub(int64_t a, int64_t b) { return (int64_t)((uint64_t)a - (uint64_t)b); }
static inline int64_t i_mul(int64_t a, int64_t b) { return (int64_t)((uint64_t)a * (uint64_t)b); }
static inline int64_t i_neg(int64_t a) { return (int64_t)(0ULL - (uint64_t)a); }
static inline int64_t i_abs(int64_t a) { return a < 0 ? i_neg(a) : a; }
static inline int64_t i_floordiv(int64_t a, int64_t b, int* e) {
    if (b == 0) { *e = 1; return 0; }
    if (b == -1) return i_neg(a);
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
static inline int64_t i_mod(int64_t a, int64_t b, int* e) {
    if (b == 0) { *e = 1; return 0; }
    if (b == -1) return 0;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}
static inline int64_t i_pow(int64_t a, int64_t n) {
    int64_t r = 1;
    while (n > 0) { if (n & 1) r = i_mul(r, a); a = i_mul(a, a); n >>= 1; }
    return r;
}
static inline double f_div(double a, double b, int* e) {
    if (b == 0.0) { *e = 1; return 0.0; }
    return a / b;
}
/* CPython float_divmod: the quotient and remainder of a floored division. */
static inline double f_mod(double a, double b, int* e) {
    if (b == 0.0) { *e = 1; return 0.0; }
    double r = fmod(a, b);
    if (r != 0.0) { if ((b < 0.0) != (r < 0.0)) r += b; }
    else r = copysign(0.0, b);
    return r;
}
static inline double f_floordiv(double a, double b, int* e) {
    if (b == 0.0) { *e = 1; return 0.0; }
    double r = fmod(a, b);
    double d = (a - r) / b;
    if (r != 0.0) { if ((b < 0.0) != (r < 0.0)) d -= 1.0; }
    if (d != 0.0) {
        double f = floor(d);
        if (d - f > 0.5) f += 1.0;
        return f;
    }
    return copysign(0.0, a / b);
}
static inline int64_t f_to_i(double x, int* e) {
    if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0)) { *e = 1; return 0; }
    return (int64_t)x;
}
static inline int64_t i_min(int64_t a, int64_t b) { return b < a ? b : a; }
static inline int64_t i_max(int64_t a, int64_t b) { return b > a ? b : a; }
static inline double f_min(double a, double b) { return b < a ? b : a; }
static inline double f_max(double a, double b) { return b > a ? b : a; }

static inline int64_t rd_i(int t, const void* d, int64_t r) {
    switch (t) {
        case DFTU_TYPE_INT8: return ((const int8_t*)d)[r];
        case DFTU_TYPE_INT16: return ((const int16_t*)d)[r];
        case DFTU_TYPE_INT32: return ((const int32_t*)d)[r];
        case DFTU_TYPE_INT64: return ((const int64_t*)d)[r];
        case DFTU_TYPE_UINT8: return ((const uint8_t*)d)[r];
        case DFTU_TYPE_UINT16: return ((const uint16_t*)d)[r];
        case DFTU_TYPE_UINT32: return ((const uint32_t*)d)[r];
        default: return (int64_t)((const uint64_t*)d)[r];
    }
}
static inline double rd_f(int t, const void* d, int64_t r) {
    switch (t) {
        case DFTU_TYPE_FLOAT32: return ((const float*)d)[r];
        case DFTU_TYPE_FLOAT64: return ((const double*)d)[r];
        case DFTU_TYPE_UINT64: return (double)((const uint64_t*)d)[r];
        default: return (double)rd_i(t, d, r);
    }
}
static inline int rd_b(const void* d, int64_t r) {
    return (((const uint8_t*)d)[r >> 3] >> (r & 7)) & 1;
}
static inline int is_null_at(const dftu_series* s, int64_t nulls, const uint8_t* bits,
                             int64_t offset, int64_t r) {
    if (nulls == 0) return 0;
    if (bits == NULL) return dftu_series_is_null(s, r);
    int64_t k = offset + r;
    return !((bits[k >> 3] >> (k & 7)) & 1);
}
static const char* type_name_of(int t) {
    switch (t) {
        case DFTU_TYPE_BOOL: return "Bool";
        case DFTU_TYPE_INT8: return "Int8";
        case DFTU_TYPE_INT16: return "Int16";
        case DFTU_TYPE_INT32: return "Int32";
        case DFTU_TYPE_INT64: return "Int64";
        case DFTU_TYPE_UINT8: return "Uint8";
        case DFTU_TYPE_UINT16: return "Uint16";
        case DFTU_TYPE_UINT32: return "Uint32";
        case DFTU_TYPE_UINT64: return "Uint64";
        case DFTU_TYPE_FLOAT32: return "Float32";
        case DFTU_TYPE_FLOAT64: return "Float64";
        default: return "a non-numeric type";
    }
}
static inline int is_integer_type(int t) {
    return t >= DFTU_TYPE_INT8 && t <= DFTU_TYPE_UINT64;
}
"""

# math.<name> -> (C text with {0}, {1} for the arguments, arity, result type or None for float)
_MATH_FLOAT = {
    "sqrt": "sqrt({0})",
    "exp": "exp({0})",
    "expm1": "expm1({0})",
    "log": "log({0})",
    "log2": "log2({0})",
    "log10": "log10({0})",
    "log1p": "log1p({0})",
    "sin": "sin({0})",
    "cos": "cos({0})",
    "tan": "tan({0})",
    "asin": "asin({0})",
    "acos": "acos({0})",
    "atan": "atan({0})",
    "sinh": "sinh({0})",
    "cosh": "cosh({0})",
    "tanh": "tanh({0})",
    "fabs": "fabs({0})",
    "degrees": "(({0}) * (180.0 / 3.14159265358979323846))",
    "radians": "(({0}) * (3.14159265358979323846 / 180.0))",
}
_MATH_FLOAT2 = {
    "atan2": "atan2({0}, {1})",
    "pow": "pow({0}, {1})",
    "fmod": "fmod({0}, {1})",
    "hypot": "hypot({0}, {1})",
    "copysign": "copysign({0}, {1})",
}
_MATH_INT = {"floor": "floor", "ceil": "ceil", "trunc": "trunc"}
_MATH_BOOL = {"isnan": "isnan", "isinf": "isinf", "isfinite": "isfinite"}
_MATH_CONST = {
    "pi": math.pi,
    "e": math.e,
    "tau": math.tau,
    "inf": math.inf,
    "nan": math.nan,
}


class _Compiler:
    def __init__(self, fn: Callable[..., object], src: Source) -> None:
        self.fn = fn
        self.src = src
        self.vars: dict[str, int] = {}
        self.params: list[tuple[str, int]] = []
        self.ret = FLOAT

    def reject(self, what: str, node: ast.AST | None) -> NoReturn:
        raise self.src.error(f"unsupported in @dft.udf: {what}{_BATCH_HINT}", node)

    def fail(self, reason: str, node: ast.AST | None) -> NoReturn:
        raise self.src.error(reason, node)

    def annotation(self, node: ast.expr | None, where: str, at: ast.AST) -> int:
        if isinstance(node, ast.Name) and node.id in _TYPE_NAMES:
            return _TYPE_NAMES[node.id]
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            if node.value in _TYPE_NAMES:
                return _TYPE_NAMES[node.value]
        self.fail(
            f"@dft.udf needs a float, int or bool annotation on {where}"
            f"{_BATCH_HINT if node is not None else ''}",
            node if node is not None else at,
        )

    def coerce(self, code: str, have: int, want: int, node: ast.AST) -> str:
        if have == want:
            return code
        if want == FLOAT:
            return f"((double)({code}))"
        if want == INT and have == BOOL:
            return f"((int64_t)({code}))"
        names = {BOOL: "bool", INT: "int", FLOAT: "float"}
        self.fail(f"a {names[have]} value cannot be used where {names[want]} is needed", node)

    def expr(self, node: ast.expr, ctx: set[str]) -> tuple[str, int]:
        if isinstance(node, ast.Constant):
            v = node.value
            if isinstance(v, bool):
                return ("1" if v else "0"), BOOL
            if isinstance(v, int):
                if not -(1 << 63) <= v < (1 << 63):
                    self.fail(f"integer literal {v} does not fit in 64 bits", node)
                return (f"INT64_C({v})" if v != -(1 << 63) else "INT64_MIN"), INT
            if isinstance(v, float):
                return self.float_literal(v), FLOAT
            self.reject(f"the literal {v!r}", node)
        if isinstance(node, ast.Name):
            return self.name(node, ctx)
        if isinstance(node, ast.Attribute):
            return self.attribute(node)
        if isinstance(node, ast.UnaryOp):
            return self.unary(node, ctx)
        if isinstance(node, ast.BinOp):
            return self.binop(node, ctx)
        if isinstance(node, ast.BoolOp):
            op = "&&" if isinstance(node.op, ast.And) else "||"
            parts = []
            for value in node.values:
                code, ty = self.expr(value, ctx)
                if ty != BOOL:
                    self.fail(
                        "'and' and 'or' take bool operands in @dft.udf; compare the value "
                        "explicitly (x != 0)",
                        value,
                    )
                parts.append(code)
            return "(" + f" {op} ".join(parts) + ")", BOOL
        if isinstance(node, ast.Compare):
            return self.compare(node, ctx)
        if isinstance(node, ast.IfExp):
            cond = self.condition(node.test, ctx)
            a, ta = self.expr(node.body, ctx)
            b, tb = self.expr(node.orelse, ctx)
            ty = BOOL if ta == tb == BOOL else max(ta, tb, INT)
            a = self.coerce(a, ta, ty, node.body)
            b = self.coerce(b, tb, ty, node.orelse)
            return f"({cond} ? {a} : {b})", ty
        if isinstance(node, ast.Call):
            return self.call(node, ctx)
        self.reject(f"the expression {type(node).__name__}", node)

    def float_literal(self, v: float) -> str:
        if math.isnan(v):
            return "NAN"
        if math.isinf(v):
            return "INFINITY" if v > 0 else "(-INFINITY)"
        return repr(v)

    def name(self, node: ast.Name, ctx: set[str]) -> tuple[str, int]:
        ident = node.id
        if ident in self.vars:
            if ident not in ctx:
                self.fail(f"'{ident}' may be used before it is assigned", node)
            return f"v_{ident}", self.vars[ident]
        found = self.free_name(ident)
        if isinstance(found, bool):
            return ("1" if found else "0"), BOOL
        if isinstance(found, int):
            return f"INT64_C({found})", INT
        if isinstance(found, float):
            return self.float_literal(found), FLOAT
        self.fail(
            f"'{ident}' is not an argument, a local, or an int, float or bool constant"
            f"{_BATCH_HINT}",
            node,
        )

    def free_name(self, ident: str) -> object:
        code = getattr(self.fn, "__code__", None)
        closure = getattr(self.fn, "__closure__", None)
        if code is not None and closure is not None and ident in code.co_freevars:
            try:
                return closure[code.co_freevars.index(ident)].cell_contents
            except ValueError:
                return None
        return getattr(self.fn, "__globals__", {}).get(ident)

    def is_math(self, node: ast.expr) -> bool:
        return (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and self.free_name(node.value.id) is math
        )

    def attribute(self, node: ast.Attribute) -> tuple[str, int]:
        if self.is_math(node) and node.attr in _MATH_CONST:
            return self.float_literal(_MATH_CONST[node.attr]), FLOAT
        self.reject("an attribute access", node)

    def condition(self, node: ast.expr, ctx: set[str]) -> str:
        code, ty = self.expr(node, ctx)
        if ty != BOOL:
            self.fail(
                "a condition must be a bool in @dft.udf; compare the value explicitly (x != 0)",
                node,
            )
        return code

    def unary(self, node: ast.UnaryOp, ctx: set[str]) -> tuple[str, int]:
        code, ty = self.expr(node.operand, ctx)
        if isinstance(node.op, ast.Not):
            if ty != BOOL:
                self.fail("'not' takes a bool in @dft.udf; compare the value explicitly", node)
            return f"(!{code})", BOOL
        if isinstance(node.op, ast.UAdd):
            return (code, INT if ty == BOOL else ty)
        if isinstance(node.op, ast.USub):
            if ty == FLOAT:
                return f"(-{code})", FLOAT
            return f"i_neg({self.coerce(code, ty, INT, node)})", INT
        self.reject("this unary operator", node)

    def binop(self, node: ast.BinOp, ctx: set[str]) -> tuple[str, int]:
        a, ta = self.expr(node.left, ctx)
        b, tb = self.expr(node.right, ctx)
        op = node.op
        ty = max(ta, tb, INT)
        if isinstance(op, ast.Div):
            return (
                f"f_div({self.coerce(a, ta, FLOAT, node)}, {self.coerce(b, tb, FLOAT, node)}, e_)",
                FLOAT,
            )
        if isinstance(op, ast.Pow):
            if ta != FLOAT and tb != FLOAT:
                lit = node.right
                if isinstance(lit, ast.Constant) and isinstance(lit.value, int) and lit.value >= 0:
                    return f"i_pow({self.coerce(a, ta, INT, node)}, INT64_C({lit.value}))", INT
                self.fail(
                    "an int ** int needs a non-negative int literal exponent in @dft.udf; "
                    "use a float exponent for any other power",
                    node,
                )
            return (
                f"pow({self.coerce(a, ta, FLOAT, node)}, {self.coerce(b, tb, FLOAT, node)})",
                FLOAT,
            )
        if isinstance(op, (ast.Add, ast.Sub, ast.Mult)):
            sym = "+" if isinstance(op, ast.Add) else "-" if isinstance(op, ast.Sub) else "*"
            if ty == FLOAT:
                return (
                    f"({self.coerce(a, ta, FLOAT, node)} {sym} {self.coerce(b, tb, FLOAT, node)})",
                    FLOAT,
                )
            fn = {"+": "i_add", "-": "i_sub", "*": "i_mul"}[sym]
            return (
                f"{fn}({self.coerce(a, ta, INT, node)}, {self.coerce(b, tb, INT, node)})",
                INT,
            )
        if isinstance(op, (ast.FloorDiv, ast.Mod)):
            floor = isinstance(op, ast.FloorDiv)
            if ty == FLOAT:
                x = self.coerce(a, ta, FLOAT, node)
                y = self.coerce(b, tb, FLOAT, node)
                return (f"f_floordiv({x}, {y}, e_)" if floor else f"f_mod({x}, {y}, e_)"), FLOAT
            fn = "i_floordiv" if floor else "i_mod"
            return (
                f"{fn}({self.coerce(a, ta, INT, node)}, {self.coerce(b, tb, INT, node)}, e_)",
                INT,
            )
        self.reject("this binary operator", node)

    def compare(self, node: ast.Compare, ctx: set[str]) -> tuple[str, int]:
        operands = [self.expr(node.left, ctx)] + [self.expr(c, ctx) for c in node.comparators]
        nodes = [node.left] + list(node.comparators)
        parts = []
        for i, op in enumerate(node.ops):
            sym = COMPARE_SYMBOL.get(type(op))
            if sym is None:
                self.reject("this comparison operator", node)
            (a, ta), (b, tb) = operands[i], operands[i + 1]
            ty = BOOL if ta == tb == BOOL else max(ta, tb, INT)
            a = self.coerce(a, ta, ty, nodes[i])
            b = self.coerce(b, tb, ty, nodes[i + 1])
            parts.append(f"({a} {sym} {b})")
        return "(" + " && ".join(parts) + ")", BOOL

    def call(self, node: ast.Call, ctx: set[str]) -> tuple[str, int]:
        if node.keywords:
            self.reject("a keyword argument", node)
        func = node.func
        args = [self.expr(a, ctx) for a in node.args]
        n = len(args)
        if self.is_math(func):
            assert isinstance(func, ast.Attribute)
            return self.math_call(func.attr, args, node)
        if not isinstance(func, ast.Name):
            self.reject("this call", node)
        shadow = self.free_name(func.id)
        if shadow is not None and shadow is not getattr(builtins, func.id, None):
            self.reject(f"a call of {func.id}, which is not a builtin here", node)
        what = func.id
        if what == "abs" and n == 1:
            code, ty = args[0]
            if ty == FLOAT:
                return f"fabs({code})", FLOAT
            return f"i_abs({self.coerce(code, ty, INT, node)})", INT
        if what in ("min", "max") and n >= 2:
            ty = FLOAT if any(t == FLOAT for _, t in args) else INT
            fn = ("f_" if ty == FLOAT else "i_") + what
            cur = self.coerce(args[0][0], args[0][1], ty, node)
            for code, t in args[1:]:
                cur = f"{fn}({cur}, {self.coerce(code, t, ty, node)})"
            return cur, ty
        if what == "float" and n == 1:
            return self.coerce(args[0][0], args[0][1], FLOAT, node), FLOAT
        if what == "int" and n == 1:
            code, ty = args[0]
            if ty == FLOAT:
                return f"f_to_i({code}, e_)", INT
            return self.coerce(code, ty, INT, node), INT
        if what == "bool" and n == 1:
            return f"(({args[0][0]}) != 0)", BOOL
        self.reject(f"the call {what}() with {n} argument(s)", node)

    def math_call(self, name: str, args: list[tuple[str, int]], node: ast.Call) -> tuple[str, int]:
        flt = [self.coerce(c, t, FLOAT, node) for c, t in args]
        if name in _MATH_FLOAT and len(args) == 1:
            return _MATH_FLOAT[name].format(*flt), FLOAT
        if name in _MATH_FLOAT2 and len(args) == 2:
            return _MATH_FLOAT2[name].format(*flt), FLOAT
        if name in _MATH_INT and len(args) == 1:
            return f"f_to_i({_MATH_INT[name]}({flt[0]}), e_)", INT
        if name in _MATH_BOOL and len(args) == 1:
            return f"(({_MATH_BOOL[name]}({flt[0]})) != 0)", BOOL
        self.reject(f"math.{name} with {len(args)} argument(s)", node)

    def assign_var(self, name: str, ty: int, node: ast.AST) -> None:
        have = self.vars.get(name)
        if have is None:
            self.vars[name] = ty
        elif have < ty:
            names = {BOOL: "bool", INT: "int", FLOAT: "float"}
            self.fail(
                f"'{name}' is {names[have]} and cannot take a {names[ty]} value in @dft.udf",
                node,
            )

    def block(
        self, body: list[ast.stmt], ctx: set[str], depth: int
    ) -> tuple[list[str], set[str], bool]:
        lines: list[str] = []
        pad = "    " * depth
        returned = False
        for stmt in body:
            if returned:
                self.fail("this statement is never reached", stmt)
            if isinstance(stmt, ast.Pass):
                continue
            if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Constant):
                continue
            if isinstance(stmt, ast.Return):
                if stmt.value is None:
                    self.fail("a @dft.udf function must return a value", stmt)
                code, ty = self.expr(stmt.value, ctx)
                code = self.coerce(code, ty, self.ret, stmt.value)
                lines.append(f"{pad}return {code};")
                returned = True
            elif isinstance(stmt, (ast.Assign, ast.AnnAssign, ast.AugAssign)):
                lines.extend(self.assign(stmt, ctx, pad))
            elif isinstance(stmt, ast.If):
                cond = self.condition(stmt.test, ctx)
                body_lines, body_ctx, body_ret = self.block(stmt.body, set(ctx), depth + 1)
                else_lines, else_ctx, else_ret = self.block(stmt.orelse, set(ctx), depth + 1)
                lines.append(f"{pad}if ({cond}) {{")
                lines.extend(body_lines)
                lines.append(f"{pad}}} else {{")
                lines.extend(else_lines)
                lines.append(f"{pad}}}")
                if body_ret and else_ret:
                    returned = True
                elif body_ret:
                    ctx = else_ctx
                elif else_ret:
                    ctx = body_ctx
                else:
                    ctx = body_ctx & else_ctx
            else:
                self.reject(f"the statement {type(stmt).__name__}", stmt)
        return lines, ctx, returned

    def assign(self, stmt: ast.stmt, ctx: set[str], pad: str) -> list[str]:
        if isinstance(stmt, ast.Assign):
            if len(stmt.targets) != 1 or not isinstance(stmt.targets[0], ast.Name):
                self.reject("an assignment to anything but one name", stmt)
            target, value = stmt.targets[0], stmt.value
        elif isinstance(stmt, ast.AnnAssign):
            if not isinstance(stmt.target, ast.Name) or stmt.value is None:
                self.reject("an annotation without a value", stmt)
            target, value = stmt.target, stmt.value
        else:
            assert isinstance(stmt, ast.AugAssign)
            if not isinstance(stmt.target, ast.Name):
                self.reject("an assignment to anything but one name", stmt)
            target = stmt.target
            value = ast.copy_location(
                ast.BinOp(
                    left=ast.copy_location(ast.Name(id=target.id, ctx=ast.Load()), stmt),
                    op=stmt.op,
                    right=stmt.value,
                ),
                stmt,
            )
        code, ty = self.expr(value, ctx)
        if isinstance(stmt, ast.AnnAssign):
            want = self.annotation(stmt.annotation, f"'{target.id}'", stmt)
            code = self.coerce(code, ty, want, value)
            ty = want
        self.assign_var(target.id, ty, stmt)
        code = self.coerce(code, ty, self.vars[target.id], value)
        ctx.add(target.id)
        return [f"{pad}v_{target.id} = {code};"]

    def compile(self, node: ast.FunctionDef) -> tuple[str, list[tuple[str, int]], int]:
        args = node.args
        if args.vararg or args.kwarg or args.kwonlyargs or args.defaults or args.kw_defaults:
            self.reject("*args, **kwargs, keyword-only arguments or defaults", node)
        ctx: set[str] = set()
        for a in list(args.posonlyargs) + list(args.args):
            ty = self.annotation(a.annotation, f"the argument '{a.arg}'", a)
            self.params.append((a.arg, ty))
            self.vars[a.arg] = ty
            ctx.add(a.arg)
        if not self.params:
            self.fail("@dft.udf needs at least one argument", node)
        self.ret = self.annotation(node.returns, "the return value", node)
        lines, _, returned = self.block(node.body, ctx, 1)
        if not returned:
            self.fail("this function can reach its end without returning a value", node)
        params = ", ".join(f"{_C_TYPES[t]} v_{n}" for n, t in self.params)
        local_decls = [
            f"    {_C_TYPES[t]} v_{n};"
            for n, t in self.vars.items()
            if n not in {p for p, _ in self.params}
        ]
        text = (
            f"static inline {_C_TYPES[self.ret]} row_fn({params}, int* e_) {{\n"
            + "\n".join(local_decls + lines)
            + f"\n    return ({_C_TYPES[self.ret]})0;\n}}\n"
        )
        return text, self.params, self.ret


def _emit(name: str, row_fn: str, params: list[tuple[str, int]], ret: int, volatile: bool) -> str:
    n = len(params)
    natural = {BOOL: "DFTU_TYPE_BOOL", INT: "DFTU_TYPE_INT64", FLOAT: "DFTU_TYPE_FLOAT64"}
    lit = c_str_literal(name)
    out = [_PRELUDE, row_fn]

    # bind: check each argument type and keep its tag
    out.append(f"typedef struct {{ int tag[{n}]; }} UdfBound;")
    out.append(
        "static dftu_status udf_bind(dftu_ctx* ctx, void* self, const dftu_type_info* args,\n"
        "                            uint32_t n, const dftu_scalar* const* consts,\n"
        "                            dftu_type_info* out, void** bound) {\n"
        "    (void)self; (void)consts;\n"
        "    static const int kinds[] = {" + ", ".join(str(t) for _, t in params) + "};\n"
        '    static const char* const kind_names[] = {"bool", "int", "float"};\n'
        "    UdfBound* b = (UdfBound*)malloc(sizeof(UdfBound));\n"
        '    if (b == NULL) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");\n'
        f"    for (uint32_t i = 0; i < n && i < {n}; ++i) {{\n"
        "        int t = (int)args[i].id;\n"
        "        int ok = kinds[i] == 0 ? t == DFTU_TYPE_BOOL\n"
        "               : kinds[i] == 1 ? is_integer_type(t)\n"
        "               : (is_integer_type(t) || t == DFTU_TYPE_FLOAT32 || t == DFTU_TYPE_FLOAT64);\n"
        "        if (!ok) {\n"
        "            char msg[240];\n"
        '            snprintf(msg, sizeof msg, "argument %u is %s, but the function declares %s",\n'
        "                     (unsigned)(i + 1), type_name_of(t), kind_names[kinds[i]]);\n"
        "            free(b);\n"
        "            return dftu_fail(ctx, DFTU_E_INVALID, msg);\n"
        "        }\n"
        "        b->tag[i] = t;\n"
        "    }\n"
        "    out->struct_size = sizeof(dftu_type_info);\n"
        f"    out->id = {_ARG_TYPES[ret]};\n"
        "    *bound = b;\n"
        "    return DFTU_OK;\n"
        "}\n"
        "static void udf_free(void* bound) { free(bound); }"
    )

    arg_decl = []
    fast_conds = []
    fast_args = []
    gen_args = []
    null_decl = []
    null_conds = []
    range_scan: list[str] = []
    for i, (_, t) in enumerate(params):
        arg_decl.append(f"    const void* d{i} = dftu_series_data(args[{i}]);")
        null_decl.append(
            f"    int64_t nc{i} = dftu_series_null_count(args[{i}]);\n"
            f"    int64_t vo{i} = 0;\n"
            f"    const uint8_t* vb{i} = dftu_series_validity(args[{i}], &vo{i});"
        )
        if t == INT:
            # A Uint64 column has the bits of Int64 once the range scan passed.
            fast_conds.append(
                f"(b->tag[{i}] == DFTU_TYPE_INT64 || b->tag[{i}] == DFTU_TYPE_UINT64) && nc{i} == 0"
            )
            range_scan.append(
                f"    if (b->tag[{i}] == DFTU_TYPE_UINT64) {{\n"
                f"        const uint64_t* u = (const uint64_t*)d{i};\n"
                "        uint64_t big = 0;\n"
                f"        if (nc{i} == 0) {{\n"
                "            for (int64_t r = 0; r < rows; ++r) big |= u[r];\n"
                "            if ((big >> 63) != 0) {\n"
                "                for (int64_t r = 0; r < rows; ++r)\n"
                "                    if (u[r] >> 63) { bad_row = r; break; }\n"
                "            }\n"
                "        } else {\n"
                "            for (int64_t r = 0; r < rows; ++r)\n"
                f"                if ((u[r] >> 63) && !is_null_at(args[{i}], nc{i}, vb{i}, vo{i}, r)) {{ bad_row = r; break; }}\n"
                "        }\n"
                "        if (bad_row >= 0) {\n"
                "            char msg[200];\n"
                '            snprintf(msg, sizeof msg, "argument %d has the value %llu at row %lld, above the largest int 9223372036854775807",\n'
                f"                     {i + 1}, (unsigned long long)u[bad_row], (long long)bad_row);\n"
                "            free(res);\n"
                "            return dftu_fail(ctx, DFTU_E_INVALID, msg);\n"
                "        }\n"
                "    }"
            )
        else:
            fast_conds.append(f"b->tag[{i}] == {natural[t]} && nc{i} == 0")
        null_conds.append(f"is_null_at(args[{i}], nc{i}, vb{i}, vo{i}, r)")
        if t == BOOL:
            fast_args.append(f"rd_b(d{i}, r)")
            gen_args.append(f"rd_b(d{i}, r)")
        elif t == INT:
            fast_args.append(f"((const int64_t*)d{i})[r]")
            gen_args.append(f"rd_i(b->tag[{i}], d{i}, r)")
        else:
            fast_args.append(f"((const double*)d{i})[r]")
            gen_args.append(f"rd_f(b->tag[{i}], d{i}, r)")

    store = "if (v) res[r >> 3] |= (uint8_t)(1u << (r & 7));" if ret == BOOL else "res[r] = v;"
    res_ctype = "uint8_t" if ret == BOOL else _C_TYPES[ret]
    res_bytes = "(size_t)((rows + 7) / 8)" if ret == BOOL else f"(size_t)rows * sizeof({res_ctype})"
    zero = "calloc(1 + " + res_bytes + ", 1)"
    out.append(
        "static dftu_status udf_exec(dftu_ctx* ctx, void* bound, const dftu_series* const* args,\n"
        "                            uint32_t n, uint64_t rows64, dftu_series** out) {\n"
        "    const UdfBound* b = (const UdfBound*)bound;\n"
        "    int64_t rows = (int64_t)rows64;\n"
        "    (void)n;\n"
        + "\n".join(arg_decl)
        + "\n"
        + "\n".join(null_decl)
        + f"\n    {res_ctype}* res = ({res_ctype}*){zero};\n"
        "    int err = 0;\n"
        "    int64_t bad_row = -1;\n"
        '    if (res == NULL) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");\n'
        + ("\n".join(range_scan) + "\n" if range_scan else "")
        + f"    if ({' && '.join(fast_conds)}) {{\n"
        "        for (int64_t r0 = 0; r0 < rows; r0 += 4096) {\n"
        "            if (dftu_check(ctx) < 0) { free(res); return DFTU_E_CANCELLED; }\n"
        "            int64_t end = r0 + 4096 < rows ? r0 + 4096 : rows;\n"
        "            for (int64_t r = r0; r < end; ++r) {\n"
        f"                {_C_TYPES[ret]} v = row_fn({', '.join(fast_args)}, &err);\n"
        f"                {store}\n"
        "            }\n"
        "        }\n"
        "    } else {\n"
        "        for (int64_t r = 0; r < rows; ++r) {\n"
        "            if ((r & 4095) == 0 && dftu_check(ctx) < 0) { free(res); return DFTU_E_CANCELLED; }\n"
        f"            if ({' || '.join(null_conds)}) continue;\n"
        f"            {_C_TYPES[ret]} v = row_fn({', '.join(gen_args)}, &err);\n"
        f"            {store}\n"
        "        }\n"
        "    }\n"
        "    if (err) {\n"
        "        free(res);\n"
        f'        return dftu_fail(ctx, DFTU_E_INVALID, "division or modulo by zero, or an int() of a non-finite float, in udf \'" {lit} "\'");\n'
        "    }\n"
        f"    *out = dftu_series_new_flat({_ARG_TYPES[ret]}, res, rows, NULL);\n"
        "    free(res);\n"
        '    return *out != NULL ? DFTU_OK : dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");\n'
        "}"
    )
    flags = "DFTU_FN_VOLATILE" if volatile else "0"
    out.append(
        "static const dftu_udf_desc g_desc = {sizeof(dftu_udf_desc), "
        f"{lit}, {flags}, {n}, {n}, udf_bind, udf_exec, udf_free}};\n"
        'extern "C" const dftu_udf_desc* dftu_jit_udf_desc(void) { return &g_desc; }\n'
    )
    return "\n".join(out)


def compile_udf(fn: Callable[..., object], name: str, volatile: bool) -> str:
    """Compile ``fn`` to a native UDF named ``name`` and return the path of the
    shared library. Raises :class:`JitError`, located at the offending node, for
    a body outside the subset."""
    try:
        src = Source(fn)
    except (OSError, TypeError) as exc:
        raise JitError(f"@dft.udf cannot read the source of {name}: {exc}", locate=False) from exc
    tree = ast.parse(src.text)
    node = tree.body[0] if tree.body else None
    if not isinstance(node, ast.FunctionDef):
        raise src.error("@dft.udf decorates a def, not a lambda or another form", node)
    compiler = _Compiler(fn, src)
    row_fn, params, ret = compiler.compile(node)
    source = _emit(name, row_fn, params, ret, volatile)
    return _build_udf(name, source)


def _build_udf(name: str, source: str) -> str:
    try:
        return _build.compile_source(source, "udf", group="udf", extra_flags=("-O2",))
    except _build.BuildError as exc:
        raise JitError(f"udf '{name}' compile failed:\n{exc}", locate=False) from exc
