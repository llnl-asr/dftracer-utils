# SPDX-License-Identifier: MIT
"""Compile a small Python function into a columnar ``Expr``, control flow
included.

The trace tier of ``apply`` runs a function once with a symbolic column, so
it cannot see a Python ``if``. This tier reads the function's source instead
and lowers the AST: ``x if c else y`` and ``if`` / ``elif`` / ``else`` with
``return`` in every arm become a per-row select, ``and`` / ``or`` / ``not``
become the mask operators, ``min`` / ``max`` / ``abs`` / ``int`` / ``float``
and ``math.sqrt`` / ``log`` / ``exp`` / ``floor`` / ``ceil`` map to the engine
kernels, and a string method on a value maps to the string kernels. Locals
assigned once are inlined; closure and global scalars are constants.

Anything outside that subset raises :class:`DFTUtilsTranspileError` with the exact
construct, so the caller can fall back and say why.
"""

from __future__ import annotations

import ast
import io
import math
import operator
import sys
import textwrap
import tokenize
import unicodedata
from typing import Any, Callable, Iterable, Mapping, Sequence, cast

from ._eval import COMPARE_NAME, ENGINE_ARITH_SYMBOL
from ._expr import Expr, Select, col, lit
from .errors import DFTUtilsTranspileError

# Python 3.8 wraps a subscript's slice in ast.Index; 3.9+ stores the value
# directly.
_AST_INDEX = getattr(ast, "Index", None)


def _unwrap_index(node: ast.AST) -> ast.expr:
    if _AST_INDEX is not None and isinstance(node, _AST_INDEX):
        node = getattr(node, "value", node)
    return cast("ast.expr", node)


def _missing(method: str) -> ast.expr:
    raise DFTUtilsTranspileError(f".{method} needs an argument")


# The all-null float64 column `DataFrame.eval` adds to the frame for `where` and `mask`
# without a replacement (the expression language has no null literal).
NULL_COLUMN = "__eval_null__"

_FLIP = {"lt": "gt", "le": "ge", "gt": "lt", "ge": "le", "eq": "eq", "ne": "ne"}
_MATH = {"sqrt": "sqrt", "log": "log", "exp": "exp", "floor": "floor", "ceil": "ceil"}
_NULL_TESTS = {"isna": True, "isnull": True, "notna": False, "notnull": False}  # True: is missing
_STR_MAP = {
    "lower": "lower",
    "upper": "upper",
    "strip": "strip",
    "lstrip": "lstrip",
    "rstrip": "rstrip",
}


def source_node(func: Callable[..., object]) -> ast.AST:
    """The ``FunctionDef`` or ``Lambda`` node of ``func``'s own source."""
    try:
        import inspect

        src = textwrap.dedent(inspect.getsource(func))
    except (OSError, TypeError) as e:
        raise DFTUtilsTranspileError(f"no source available ({e})") from None
    try:
        tree = ast.parse(src)
    except SyntaxError:
        # A lambda inside a larger expression: the line does not parse alone.
        # Wrap so the lambda's own text is what we look at.
        try:
            tree = ast.parse("(" + src.strip() + ")")
        except SyntaxError as e:
            raise DFTUtilsTranspileError(f"source does not parse alone ({e.msg})") from None
    code = getattr(func, "__code__", None)
    if code is None:
        raise DFTUtilsTranspileError("not a plain Python function")
    argnames = list(code.co_varnames[: code.co_argcount])
    fname = str(getattr(func, "__name__", "<lambda>"))
    if fname != "<lambda>":
        for node in ast.walk(tree):
            if isinstance(node, ast.FunctionDef) and node.name == fname:
                return node
        raise DFTUtilsTranspileError(f"function {fname!r} not found in its source")
    lambdas = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Lambda) and [a.arg for a in node.args.args] == argnames
    ]
    if len(lambdas) != 1:
        raise DFTUtilsTranspileError(
            "several lambdas with the same arguments share this line; put the function on its own line"
        )
    return lambdas[0]


_MAX_INLINE_DEPTH = 8


class _Lowering:
    """Lowers one function. ``columns`` makes its single argument a row; else
    ``bound`` maps each parameter name to the expression standing for it (the
    top-level value, or a caller's argument when inlining a callee)."""

    def __init__(
        self,
        func: Callable[..., object] | None,
        node: ast.AST,
        columns: Sequence[str] | None,
        bound: dict[str, Expr] | None = None,
        depth: int = 0,
        int_inputs: dict[str, bool] | None = None,
        bound_int: dict[str, bool] | None = None,
        consts: dict[str, Any] | None = None,
        kinds: dict[str, str] | None = None,
        shown: dict[str, str] | None = None,
    ) -> None:
        self.func = func
        self.kinds: dict[str, str] = dict(kinds or {})
        self.shown: dict[str, str] = dict(shown or {})
        self.node = node
        self.uses_null = False
        self.columns = list(columns) if columns is not None else None
        self.depth = depth
        self.int_inputs: dict[str, bool] = dict(int_inputs or {})
        self.bound_int: dict[str, bool] = dict(bound_int or {})
        self.local_int: dict[str, bool] = {}
        args = node.args.args if isinstance(node, (ast.FunctionDef, ast.Lambda)) else []
        names = [a.arg for a in args]
        if columns is not None:
            if len(names) != 1:
                raise DFTUtilsTranspileError("a row function takes exactly one argument")
            self.row_param: str | None = names[0]
            self.bound: dict[str, Expr] = {}
        else:
            self.row_param = None
            if bound is None:
                if len(names) != 1:
                    raise DFTUtilsTranspileError("the function must take exactly one argument")
                bound = {names[0]: col("__x0__")}
                self.bound_int = {names[0]: self.int_inputs.get("__x0__", False)}
            if list(bound) != names:
                raise DFTUtilsTranspileError(
                    f"{getattr(func, '__name__', 'function')} takes {len(names)} argument(s), got {len(bound)}"
                )
            self.bound = dict(bound)
        self.locals: dict[str, Expr] = {}
        if func is None:  # a source string (transpile_expr): only the names it is given
            self.consts: dict[str, Any] = dict(consts or {})
        else:
            import inspect

            cv = inspect.getclosurevars(func)
            self.consts = {**cv.globals, **cv.nonlocals}

    # -- entry -----------------------------------------------------------------
    def lower(self) -> Expr:
        if isinstance(self.node, ast.Lambda):
            return self.expr(self.node.body)
        if not isinstance(self.node, ast.FunctionDef):
            raise DFTUtilsTranspileError("not a function")
        body = [s for s in self.node.body if not _is_docstring(s)]
        out = self.block(body)
        if out is None:
            raise DFTUtilsTranspileError("the function has a path with no return")
        return out

    # -- statements ------------------------------------------------------------
    def block(self, stmts: list[ast.stmt]) -> Expr | None:
        """The value a statement list returns, or None if it falls through."""
        for i, s in enumerate(stmts):
            if isinstance(s, ast.Return):
                if s.value is None:
                    raise DFTUtilsTranspileError("a bare return has no value")
                return self.expr(s.value)
            if isinstance(s, ast.Assign):
                if len(s.targets) != 1 or not isinstance(s.targets[0], ast.Name):
                    raise DFTUtilsTranspileError("only `name = value` assignments")
                self.local_int[s.targets[0].id] = self.is_int(s.value)
                self.locals[s.targets[0].id] = self.expr(s.value)
                continue
            if isinstance(s, ast.If):
                cond = self.expr(s.test)
                saved = dict(self.locals)
                then = self.block(s.body)
                self.locals = dict(saved)
                rest = stmts[i + 1 :]
                otherwise = self.block(s.orelse + rest) if s.orelse else self.block(rest)
                self.locals = saved
                if then is None or otherwise is None:
                    raise DFTUtilsTranspileError("every branch of an `if` must return")
                return Select(cond, then, otherwise)
            if isinstance(s, ast.Pass):
                continue
            raise DFTUtilsTranspileError(f"statement {type(s).__name__} has no engine form")
        return None

    # -- expressions -----------------------------------------------------------
    def expr(self, n: ast.expr) -> Expr:
        if isinstance(n, ast.Constant):
            if isinstance(n.value, bool) or not isinstance(n.value, (int, float)):
                raise DFTUtilsTranspileError(f"constant {n.value!r} is not a number")
            return lit(n.value)
        if isinstance(n, ast.Name):
            return self.name(n.id)
        if isinstance(n, ast.Subscript):
            return self.field(n.value, _unwrap_index(n.slice))
        if isinstance(n, ast.Attribute):
            if isinstance(n.value, ast.Name) and n.value.id == self.row_param:
                return self.column(n.attr)
            raise DFTUtilsTranspileError(f"attribute .{n.attr} is not a row field")
        if isinstance(n, ast.BinOp):
            return self.binop(n)
        if isinstance(n, ast.UnaryOp):
            if isinstance(n.op, ast.USub):
                inner = self.scalar_or_expr(n.operand)
                if isinstance(inner, Expr):
                    return inner * -1
                return lit(-cast("int | float", inner))
            if isinstance(n.op, ast.UAdd):
                inner = self.scalar_or_expr(n.operand)
                return inner if isinstance(inner, Expr) else lit(inner)
            if isinstance(n.op, (ast.Not, ast.Invert)):  # the mask negation
                return ~self.expr(n.operand)
            raise DFTUtilsTranspileError(f"unary {type(n.op).__name__} has no engine form")
        if isinstance(n, ast.BoolOp):
            parts = [self.expr(v) for v in n.values]
            out = parts[0]
            for p in parts[1:]:
                out = (out & p) if isinstance(n.op, ast.And) else (out | p)
            return out
        if isinstance(n, ast.Compare):
            return self.compare(n)
        if isinstance(n, ast.IfExp):
            return Select(self.expr(n.test), self.expr(n.body), self.expr(n.orelse))
        if isinstance(n, ast.Call):
            return self.call(n)
        raise DFTUtilsTranspileError(f"expression {type(n).__name__} has no engine form")

    def binop(self, n: ast.BinOp) -> Expr:
        left = cast("Expr | int | float", self.scalar_or_expr(n.left))
        right = cast("Expr | int | float", self.scalar_or_expr(n.right))
        if not isinstance(left, Expr) and not isinstance(right, Expr):
            raise DFTUtilsTranspileError("arithmetic on two constants")
        op = ENGINE_ARITH_SYMBOL.get(type(n.op))
        if op is not None:
            return cast(
                "Expr",
                {
                    "+": operator.add,
                    "-": operator.sub,
                    "*": operator.mul,
                    "/": operator.truediv,
                }[op](left, right),
            )
        if isinstance(n.op, ast.FloorDiv):
            # The engine floors as Python does: toward -inf, 1.0 // 0.1 is 9.0.
            return cast("Expr", left // right)
        if isinstance(n.op, ast.Mod):
            return cast("Expr", left % right)
        if isinstance(n.op, ast.Pow):
            if not isinstance(left, Expr):
                raise DFTUtilsTranspileError("a constant base has no engine form")
            if isinstance(right, float) and right == 0.5:
                return left.sqrt()
            if isinstance(right, int) and not isinstance(right, bool) and 0 <= right <= 8:
                if right == 0:
                    return left * 0 + 1
                out = left
                for _ in range(right - 1):
                    out = out * left
                return out
            raise DFTUtilsTranspileError("** needs a constant exponent in 0..8 or 0.5")
        raise DFTUtilsTranspileError(f"operator {type(n.op).__name__} has no engine form")

    def is_int(self, n: ast.expr) -> bool:
        """Whether a subtree is integer-valued: an int constant, an integer
        input column, an integer local, `int(...)`, or `+ - * // %` on those."""
        v = self.constant(n)
        if v is not None:
            return isinstance(v, int)
        if isinstance(n, ast.Name):
            if n.id in self.locals:
                return self.local_int.get(n.id, False)
            return self.bound_int.get(n.id, False)
        if isinstance(n, (ast.Subscript, ast.Attribute)):
            name = self.field_name(n)
            return name is not None and self.int_inputs.get(name, False)
        if isinstance(n, ast.BinOp) and not isinstance(n.op, (ast.Div, ast.Pow)):
            return self.is_int(n.left) and self.is_int(n.right)
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, ast.USub):
            return self.is_int(n.operand)
        if isinstance(n, ast.IfExp):
            return self.is_int(n.body) and self.is_int(n.orelse)
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Name):
            if n.func.id in ("int", "len"):
                return True
            if n.func.id in ("abs", "min", "max"):
                return all(self.is_int(a) for a in n.args)
        return False

    def field_name(self, n: ast.expr) -> str | None:
        if isinstance(n, ast.Attribute):
            if isinstance(n.value, ast.Name) and n.value.id == self.row_param:
                return n.attr
            return None
        if isinstance(n, ast.Subscript):
            if isinstance(n.value, ast.Name) and n.value.id == self.row_param:
                sl = _unwrap_index(n.slice)
                k = sl.value if isinstance(sl, ast.Constant) else None
                return k if isinstance(k, str) else None
        return None

    def scalar_or_expr(self, n: ast.expr) -> Expr | int | float | str:
        """A Python number for a constant subtree, else an Expr."""
        v = self.constant(n)
        if v is not None:
            return v
        return self.expr(n)

    def constant(self, n: ast.expr) -> int | float | str | None:
        if (
            isinstance(n, ast.Constant)
            and isinstance(n.value, (int, float, str))
            and not isinstance(n.value, bool)
        ):
            return n.value
        if isinstance(n, ast.Name) and n.id in self.consts and n.id not in self.locals:
            v = self.consts[n.id]
            if isinstance(v, (int, float, str)) and not isinstance(v, bool):
                return v
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, ast.USub):
            v = self.constant(n.operand)
            if isinstance(v, (int, float)):
                return -v
        return None

    def name(self, ident: str) -> Expr:
        if ident == self.row_param:
            raise DFTUtilsTranspileError(
                "a row is not a value; index it with row['column'] or row.column"
            )
        if ident in self.locals:
            return self.locals[ident]
        if ident in self.bound:
            return self.bound[ident]
        if ident in self.consts:
            v = self.consts[ident]
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                return lit(v)
            shown = self.shown.get(ident, ident)
            raise DFTUtilsTranspileError(f"{shown!r} is a {type(v).__name__}, not a number")
        raise DFTUtilsTranspileError(f"unknown name {ident!r}")

    def field(self, value: ast.expr, key: ast.expr) -> Expr:
        if not (isinstance(value, ast.Name) and value.id == self.row_param):
            if isinstance(value, ast.Call):
                self.expr(value)  # an unsupported call is the thing to name
            raise DFTUtilsTranspileError("only the row argument can be indexed")
        k = key.value if isinstance(key, ast.Constant) else None
        if not isinstance(k, str):
            raise DFTUtilsTranspileError("row[...] needs a literal column name")
        return self.column(k)

    def column(self, name: str) -> Expr:
        if self.columns is None:
            raise DFTUtilsTranspileError("a value has no fields to index")
        if name not in self.columns:
            raise KeyError(f"no column named {name!r}")
        return col(name)

    def compare(self, n: ast.Compare) -> Expr:
        pairs = list(zip([n.left, *n.comparators[:-1]], n.ops, n.comparators))
        out: Expr | None = None
        for left, op, right in pairs:
            if isinstance(op, (ast.In, ast.NotIn)):
                part = self.membership(left, right, negate=isinstance(op, ast.NotIn))
            elif isinstance(op, (ast.Is, ast.IsNot)):
                part = self.null_test(left, right, negate=isinstance(op, ast.IsNot))
            else:
                code = COMPARE_NAME.get(type(op))
                if code is None:
                    raise DFTUtilsTranspileError(
                        f"comparison {type(op).__name__} has no engine form"
                    )
                part = self.comparison(left, code, right)
            out = part if out is None else (out & part)
        assert out is not None
        return out

    def bool_value(self, n: ast.expr) -> bool | None:
        """The value of a ``True`` / ``False`` literal or of a bool variable."""
        flag = _bool_constant(n)
        if flag is None and isinstance(n, ast.Name) and n.id not in self.locals:
            v = self.consts.get(n.id)
            flag = v if isinstance(v, bool) else None
        return flag

    def kind(self, n: ast.expr) -> str | None:
        """``"num"``, ``"text"`` or ``"bool"`` when the value type of a subtree is
        known (from the column types the caller gave), else None."""
        if self.bool_value(n) is not None:
            return "bool"
        v = self.constant(n)
        if v is not None:
            return "text" if isinstance(v, str) else "num"
        name = self.field_name(n)
        if name is not None:
            return self.kinds.get(name)
        if isinstance(n, ast.Compare):
            return "bool"
        if isinstance(n, ast.BoolOp):
            return "bool" if _same([self.kind(v) for v in n.values]) == "bool" else None
        if isinstance(n, ast.UnaryOp):
            inner = self.kind(n.operand)
            if isinstance(n.op, (ast.Not, ast.Invert)):
                return inner if inner == "bool" else None
            return inner if inner == "num" else None
        if isinstance(n, ast.BinOp):
            both = self.kind(n.left) == "num" and self.kind(n.right) == "num"
            return "num" if both else None
        if isinstance(n, ast.IfExp):
            return _same([self.kind(n.body), self.kind(n.orelse)])
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Name):
            args = [self.kind(a) for a in n.args]
            if n.func.id in ("int", "float", "len"):
                return "num"
            if n.func.id in ("abs", "round", "min", "max"):
                return _same(args)
            return None
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute):
            attr, target = n.func.attr, n.func.value
            if isinstance(target, ast.Name) and self.consts.get(target.id) is math:
                return "num"
            if attr in _BOOL_METHODS:
                return "bool"
            if attr == _STR_PART or attr in _STR_MAP or attr == "replace":
                return "text"
            if attr in ("abs", "round", "clip", "fillna", "where", "mask"):
                inner = self.kind(target)
                return (
                    inner
                    if inner == "num" or (inner == "bool" and attr in ("where", "mask"))
                    else None
                )
        return None

    def comparison(self, left: ast.expr, code: str, right: ast.expr) -> Expr:
        """One comparison. Two numbers, two strings or two Bools of known type
        compare as they are; other pairs (a type the caller did not give, or a
        timestamp) compare as ``(a - b) op 0``, which the engine orders for every
        type it subtracts. A null on either side gives null. ``True`` and
        ``False`` test a Bool value and are 1 and 0 against a number."""
        left_flag, right_flag = self.bool_value(left), self.bool_value(right)
        if left_flag is not None or right_flag is not None:
            flag = left_flag if left_flag is not None else right_flag
            other = right if left_flag is not None else left
            other_kind = self.kind(other)
            if self.constant(other) is not None or self.bool_value(other) is not None:
                raise DFTUtilsTranspileError("comparison of two constants")
            if other_kind == "bool" and code in ("eq", "ne"):
                value = self.expr(other)
                return value if flag == (code == "eq") else ~value
            if other_kind == "num":
                code = code if right_flag is not None else _FLIP[code]
                return _cmp(self.expr(other), code, int(cast("bool", flag)))
            raise DFTUtilsTranspileError(
                "True and False compare with a Bool or a number of known type"
            )
        lv, rv = self.scalar_or_expr(left), self.scalar_or_expr(right)
        if not isinstance(lv, Expr) and not isinstance(rv, Expr):
            raise DFTUtilsTranspileError("comparison of two constants")
        if isinstance(lv, Expr) and isinstance(rv, Expr):
            return self.compare_values(lv, code, rv, self.kind(left), self.kind(right))
        if not isinstance(lv, Expr):
            lv, rv, code = rv, lv, _FLIP[code]
        assert isinstance(lv, Expr)
        return _cmp(lv, code, rv)

    @staticmethod
    def compare_values(lv: Expr, code: str, rv: Expr, left: str | None, right: str | None) -> Expr:
        if left is not None and left == right:
            return _cmp(lv, code, rv)
        # a < b  <=>  (a - b) < 0
        return _cmp(lv - rv, code, 0)

    def inline(self, callee: Callable[..., object], args: list[ast.expr]) -> Expr:
        """A call to another plain Python function: lower its body with each
        parameter bound to the argument expression (or constant)."""
        if self.depth >= _MAX_INLINE_DEPTH:
            raise DFTUtilsTranspileError("calls nest too deep (recursion?)")
        node = source_node(callee)
        params = (
            [a.arg for a in node.args.args]
            if isinstance(node, (ast.FunctionDef, ast.Lambda))
            else []
        )
        if len(params) != len(args):
            raise DFTUtilsTranspileError(
                f"{getattr(callee, '__name__', 'function')} takes {len(params)} argument(s), called with {len(args)}"
            )
        bound: dict[str, Expr] = {}
        ints: dict[str, bool] = {}
        for p, a in zip(params, args):
            v = self.scalar_or_expr(a)
            bound[p] = v if isinstance(v, Expr) else lit(v)
            ints[p] = self.is_int(a)
        return _Lowering(callee, node, None, bound, self.depth + 1, None, ints).lower()

    def null_test(self, left: ast.expr, right: ast.expr, negate: bool) -> Expr:
        """``x is None`` / ``x is not None``: the null mask of ``x``."""
        none_left = isinstance(left, ast.Constant) and left.value is None
        none_right = isinstance(right, ast.Constant) and right.value is None
        if none_left == none_right:
            raise DFTUtilsTranspileError("`is` compares a value with None only")
        value = self.scalar_or_expr(right if none_left else left)
        if not isinstance(value, Expr):
            raise DFTUtilsTranspileError("`is None` needs a value on the other side")
        return value.is_not_null() if negate else value.is_null()

    def membership(self, left: ast.expr, right: ast.expr, negate: bool) -> Expr:
        lv = self.scalar_or_expr(left)
        if isinstance(lv, str):
            # "sub" in value: a substring test
            return ~self.expr(right).contains(lv) if negate else self.expr(right).contains(lv)
        if not isinstance(lv, Expr):
            raise DFTUtilsTranspileError("`in` needs a value on the left")
        members = self.members(right, "`in`")
        return lv.not_in(members) if negate else lv.is_in(members)

    def members(self, node: ast.expr, what: str) -> list[str | int | float | bool]:
        """The constants of a literal list, tuple or set, or of a list variable."""
        if isinstance(node, (ast.List, ast.Tuple, ast.Set)):
            values: list[object] = [
                e.value
                if isinstance(e, ast.Constant) and isinstance(e.value, bool)
                else self.constant(e)
                for e in node.elts
            ]
        elif isinstance(node, ast.Name) and node.id in self.consts:
            given = self.consts[node.id]
            if isinstance(given, (str, bytes, bytearray)) or not isinstance(given, Iterable):
                shown = self.shown.get(node.id, node.id)
                raise DFTUtilsTranspileError(
                    f"{what} needs a list, but {shown} is a {type(given).__name__}"
                )
            values = list(given)
        else:
            raise DFTUtilsTranspileError(f"{what} needs a literal list or a list variable")
        if any(v is None for v in values):
            raise DFTUtilsTranspileError(f"{what} needs a list of constants")
        return cast("list[str | int | float | bool]", values)

    def _bind(self, method: str, n: ast.Call, params: Sequence[str]) -> dict[str, ast.expr]:
        """The positional and keyword arguments of a method call by parameter
        name; an unknown or repeated one raises, naming it."""
        if len(n.args) > len(params):
            raise DFTUtilsTranspileError(f".{method} takes at most {len(params)} argument(s)")
        got: dict[str, ast.expr] = dict(zip(params, n.args))
        for k in n.keywords:
            if k.arg is None or k.arg not in params:
                raise DFTUtilsTranspileError(f"keyword {k.arg!r} has no engine form for .{method}")
            if k.arg in got:
                raise DFTUtilsTranspileError(f".{method} got {k.arg!r} twice")
            got[k.arg] = k.value
        return got

    def _flag(self, node: ast.expr | None, default: bool, what: str) -> bool:
        if node is None:
            return default
        flag = self.bool_value(node)
        if flag is None:
            raise DFTUtilsTranspileError(f"{what} needs True or False")
        return flag

    def _bound(self, node: ast.expr | None, what: str) -> int | float | None:
        """A number, or None for an absent bound (omitted or the literal None)."""
        if node is None or (isinstance(node, ast.Constant) and node.value is None):
            return None
        v = self.constant(node)
        if not isinstance(v, (int, float)):
            raise DFTUtilsTranspileError(f"{what} needs a number")
        return v

    def _null(self) -> Expr:
        self.uses_null = True
        return col(NULL_COLUMN)

    def split_part(self, split: ast.expr, index: ast.expr) -> Expr:
        """``x.str.split(sep).str[i]``: part ``i`` of the split, null past the end.
        Only this form lowers a split, so a list never reaches other code."""
        if not (
            isinstance(split, ast.Call)
            and isinstance(split.func, ast.Attribute)
            and split.func.attr in _SPLITS
        ):
            raise DFTUtilsTranspileError(".str[i] takes a part of a split")
        method = split.func.attr
        pat = self.constant(self._bind(method, split, ("pat",)).get("pat") or _missing(method))
        if not isinstance(pat, str) or not pat:
            raise DFTUtilsTranspileError(f"{method} needs a literal, non-empty separator")
        # pandas reads a longer split separator as a regex, and an rsplit separator
        # that overlaps itself finds other parts than a split from the left.
        if method == "split" and len(pat) > 1 and any(c in _REGEX_CHARS for c in pat):
            raise DFTUtilsTranspileError("split needs a separator with no regex character")
        if method == "rsplit" and any(pat[:k] == pat[-k:] for k in range(1, len(pat))):
            raise DFTUtilsTranspileError("rsplit needs a separator that cannot overlap itself")
        at = self.constant(index)
        if not isinstance(at, int):
            raise DFTUtilsTranspileError(".str[i] needs a literal whole number")
        return self.expr(split.func.value).split(pat).get(at)

    def method(self, target: Expr, method: str, n: ast.Call) -> Expr | None:
        """The pandas methods with an engine form beyond the string and null
        tests; None when ``method`` is not one of them."""
        if method == "abs":
            self._bind(method, n, ())
            return target.abs()
        if method == "isin":
            values = self._bind(method, n, ("values",)).get("values") or _missing("isin")
            return target.is_in(self.members(values, ".isin"))
        if method == "fillna":
            v = self.scalar_or_expr(
                self._bind(method, n, ("value",)).get("value") or _missing("fillna")
            )
            if isinstance(v, Expr):
                return Select(target.isna(), v, target)
            return target.fillna(cast("int | float", v))
        if method == "clip":
            a = self._bind(method, n, ("lower", "upper"))
            lo, hi = (
                self._bound(a.get("lower"), "clip lower"),
                self._bound(a.get("upper"), "clip upper"),
            )
            if lo is not None and hi is not None:
                return target.clip(lo, hi)
            # one bound: a null condition takes the else branch, so a null stays null
            if lo is not None:
                return Select(_cmp(target, "lt", lo), lit(lo), target)
            if hi is not None:
                return Select(_cmp(target, "gt", hi), lit(hi), target)
            raise DFTUtilsTranspileError("clip needs a lower or an upper bound")
        if method == "round":
            digits = (
                self._bound(self._bind(method, n, ("decimals",)).get("decimals"), "round decimals")
                or 0
            )
            if not isinstance(digits, int) or digits < 0:
                raise DFTUtilsTranspileError("round takes a whole number of decimals, zero or more")
            if digits == 0:
                return target.round()
            scale = 10**digits
            return cast("Expr", (target * scale).round() / scale)
        if method in ("where", "mask"):
            a = self._bind(method, n, ("cond", "other"))
            if "cond" not in a:
                raise DFTUtilsTranspileError(f".{method} needs a condition")
            cond = self.expr(a["cond"])
            other = a.get("other")
            if other is None or (isinstance(other, ast.Constant) and other.value is None):
                other_e: Expr | int | float = self._null()
            else:
                other_e = cast("Expr | int | float", self.scalar_or_expr(other))
            return target.where(cond, other_e) if method == "where" else target.mask(cond, other_e)
        return None

    def call(self, n: ast.Call) -> Expr:
        f = n.func
        if n.keywords and (
            isinstance(f, ast.Name)
            or (
                isinstance(f, ast.Attribute)
                and isinstance(f.value, ast.Name)
                and self.consts.get(f.value.id) is math
            )
        ):
            raise DFTUtilsTranspileError("keyword arguments have no engine form")
        if isinstance(f, ast.Name):
            name = f.id
            args = n.args
            if name == "abs" and len(args) == 1:
                return self.expr(args[0]).abs()
            if name in ("min", "max") and len(args) >= 2:
                vals = [self.scalar_or_expr(a) for a in args]
                if not any(isinstance(v, Expr) for v in vals):
                    raise DFTUtilsTranspileError(f"{name} of constants only")
                kinds = [self.kind(a) for a in args]
                same = kinds[0] if all(k == kinds[0] for k in kinds) else None
                out = vals[0] if isinstance(vals[0], Expr) else lit(vals[0])
                for v in vals[1:]:
                    ve = v if isinstance(v, Expr) else lit(v)
                    cond = self.compare_values(out, "lt" if name == "min" else "gt", ve, same, same)
                    out = Select(cond, out, ve)
                return out
            if name == "int" and len(args) == 1:
                return self.expr(args[0]).cast("int64")
            if name == "float" and len(args) == 1:
                return self.expr(args[0]).cast("float64")
            if name == "len" and len(args) == 1:
                return self.expr(args[0]).len_chars()
            if name == "round" and len(args) == 1:
                return self.expr(args[0]).round()
            callee = self.consts.get(name)
            if self.func is not None and callable(callee) and hasattr(callee, "__code__"):
                return self.inline(callee, args)
            raise DFTUtilsTranspileError(
                f"call to {self.shown.get(name, name)!r} has no engine form"
            )
        if isinstance(f, ast.Attribute):
            if isinstance(f.value, ast.Name) and self.consts.get(f.value.id) is math:
                op = _MATH.get(f.attr)
                if op is None or len(n.args) != 1:
                    raise DFTUtilsTranspileError(f"math.{f.attr} has no engine form")
                return cast("Expr", getattr(self.expr(n.args[0]), op)())
            if f.attr == _STR_PART and self.func is None and len(n.args) == 1:
                return self.split_part(f.value, n.args[0])
            if f.attr == _STR_PART:
                raise DFTUtilsTranspileError(f"method .{_STR_PART} has no engine form")
            target = self.expr(f.value)
            method = f.attr
            if method in _STR_MAP and not n.args:
                return cast("Expr", getattr(target, _STR_MAP[method])())
            if method in ("startswith", "endswith") and len(n.args) == 1:
                pat = self.constant(n.args[0])
                if not isinstance(pat, str):
                    raise DFTUtilsTranspileError(f"{method} needs a literal string")
                return target.starts_with(pat) if method == "startswith" else target.ends_with(pat)
            if method == "replace" and len(n.args) == 2:
                old, new = self.constant(n.args[0]), self.constant(n.args[1])
                if not isinstance(old, str) or not isinstance(new, str):
                    raise DFTUtilsTranspileError("replace needs two literal strings")
                return target.replace_all(old, new)
            if method == "contains":  # pandas str.contains: a regex search unless regex=False
                a = self._bind(method, n, ("pat", "case", "flags", "na", "regex"))
                for unsupported in ("flags", "na"):
                    if unsupported in a:
                        raise DFTUtilsTranspileError(f"contains {unsupported}= has no engine form")
                pat = self.constant(a["pat"]) if "pat" in a else None
                if not isinstance(pat, str):
                    raise DFTUtilsTranspileError("contains needs a literal string")
                case = self._flag(a.get("case"), True, "contains case")
                if self._flag(a.get("regex"), True, "contains regex"):
                    return target.regex(pat if case else "(?i)" + pat)
                return target.contains(pat, case=case)
            if method in _NULL_TESTS and not n.args and not n.keywords:
                return target.isna() if _NULL_TESTS[method] else target.notna()
            lowered = self.method(target, method, n)
            if lowered is not None:
                return lowered
            raise DFTUtilsTranspileError(f"method .{method} has no engine form")
        raise DFTUtilsTranspileError("call target has no engine form")


def _cmp(e: Expr, code: str, value: Expr | int | float | str) -> Expr:
    return {"lt": e.__lt__, "le": e.__le__, "gt": e.__gt__, "ge": e.__ge__, "eq": e.eq, "ne": e.ne}[
        code
    ](value)


def _same(kinds: list[str | None]) -> str | None:
    """The one kind of all ``kinds``, else None."""
    return kinds[0] if kinds and all(k == kinds[0] for k in kinds) else None


def _bool_constant(n: ast.expr) -> bool | None:
    if isinstance(n, ast.Constant) and isinstance(n.value, bool):
        return n.value
    return None


_SPLITS = ("split", "rsplit")
_REGEX_CHARS = frozenset(".^$*+?{}[]\\|()")
# Methods with a Bool result, so `x.isna() == False` is a Bool test.
_BOOL_METHODS = frozenset(
    {"isna", "isnull", "notna", "notnull", "isin", "startswith", "endswith", "contains"}
)
# The method `x.str[i]` becomes, so the lowering sees which call it indexes.
_STR_PART = "__str_part__"


def _is_docstring(s: ast.stmt) -> bool:
    return (
        isinstance(s, ast.Expr)
        and isinstance(s.value, ast.Constant)
        and isinstance(s.value.value, str)
    )


def transpile(
    func: Callable[..., object],
    columns: Sequence[str] | None = None,
    int_inputs: dict[str, bool] | None = None,
) -> Expr:
    """``func`` as an ``Expr``. With ``columns`` the one argument is a row and
    its fields are those columns; without, it is a value, ``col("__x0__")``.
    ``int_inputs`` says which inputs (column names, or ``"__x0__"``) hold
    integers, so ``//`` and ``%`` come back as Int64 the way Python does.
    Raises :class:`DFTUtilsTranspileError` naming the first unsupported construct, and
    ``KeyError`` for a field that is not a column."""
    return _Lowering(func, source_node(func), columns, None, 0, int_inputs).lower()


_ROW = "__row__"


class _Columns(ast.NodeTransformer):
    """Bare column names in an expression become the row fields the lowering reads;
    the names of ``@`` variables stay names."""

    def __init__(self, columns: Sequence[str], variables: Sequence[str] = ()) -> None:
        self.columns = set(columns)
        self.variables = set(variables)

    def visit_Name(self, node: ast.Name) -> ast.AST:
        if node.id in self.variables:
            return node
        if node.id not in self.columns:
            raise KeyError(f"no column named {node.id!r}")
        key = ast.Constant(value=node.id)
        if sys.version_info >= (3, 9):
            field = ast.Subscript(
                value=ast.Name(id=_ROW, ctx=ast.Load()), slice=key, ctx=ast.Load()
            )
        else:
            assert _AST_INDEX is not None
            field = ast.Subscript(
                value=ast.Name(id=_ROW, ctx=ast.Load()), slice=_AST_INDEX(value=key), ctx=ast.Load()
            )
        return ast.copy_location(field, node)

    def visit_Call(self, node: ast.Call) -> ast.AST:
        # A callee that is a bare name (abs, min, int, ...) is no column: the lowering
        # maps it or names it as unsupported.
        if not isinstance(node.func, ast.Name):
            node.func = self.visit(node.func)
        node.args = [self.visit(a) for a in node.args]
        node.keywords = [self.visit(k) for k in node.keywords]
        return node

    def visit_Subscript(self, node: ast.Subscript) -> ast.AST:
        if isinstance(node.value, ast.Attribute) and node.value.attr == "str":
            # Built here with one argument; user text cannot name it (visit_Attribute).
            part = ast.Call(
                func=ast.Attribute(
                    value=self.visit(node.value.value), attr=_STR_PART, ctx=ast.Load()
                ),
                args=[_unwrap_index(node.slice)],
                keywords=[],
            )
            return ast.copy_location(part, node)
        return self.generic_visit(node)

    def visit_Attribute(self, node: ast.Attribute) -> ast.AST:
        if node.attr == _STR_PART:
            raise DFTUtilsTranspileError(f"attribute .{_STR_PART} is not a row field")
        if isinstance(node.value, ast.Name) and node.value.id == "math":  # math.sqrt(...)
            return node
        if node.attr == "str":  # pandas' accessor: `x.str.contains(...)` is `x.contains(...)`
            return cast("ast.AST", self.visit(node.value))
        node.value = self.visit(node.value)
        return node


def _tokens(source: str) -> list[tokenize.TokenInfo]:
    try:
        return list(tokenize.generate_tokens(io.StringIO(source).readline))
    except tokenize.TokenError as e:
        raise SyntaxError(str(e.args[0])) from None


def _scalar(value: object) -> object:
    """A NumPy scalar as the Python value it holds; anything else as it is."""
    if isinstance(value, (bool, int, float, str)):
        return value
    item = getattr(value, "item", None)
    return item() if callable(item) and getattr(value, "shape", None) == () else value


def _plain(value: object) -> object:
    """A variable as the lowering reads it: a Python number, string or bool, or a
    list of them. A NumPy scalar or array, a Series and a pandas Index become one."""
    if isinstance(value, (bool, int, float, str, bytes, bytearray, Mapping)):
        return value
    for to_list in ("to_list", "tolist"):
        fn = getattr(value, to_list, None)
        if callable(fn):
            return fn()
    if isinstance(value, Iterable):
        return [_scalar(v) for v in value]
    return _scalar(value)


def has_at(source: str) -> bool:
    """Whether ``source`` has an ``@`` outside its string literals and comments."""
    if "@" not in source:
        return False
    return any(t.type == tokenize.OP and t.string == "@" for t in _tokens(source))


def resolve_at(
    source: str, local_dict: Mapping[str, Any], global_dict: Mapping[str, Any]
) -> tuple[str, dict[str, Any], dict[str, str]]:
    """``source`` with each ``@name`` made a plain name, the value of each such
    name and its ``@name`` spelling for messages. A value comes from ``local_dict``
    first, then ``global_dict``, as pandas reads them. An unknown name raises
    ``NameError``; an ``@`` with no name right after it raises ``SyntaxError``.
    Text inside string literals and comments is left alone."""
    if not has_at(source):
        return source, {}, {}
    # A prefix that is in no name of the text, so a made name cannot be a column.
    # Python folds a name to NFKC, so check that form of the text.
    folded = unicodedata.normalize("NFKC", source)
    prefix = "__at_"
    while prefix in folded:
        prefix += "_"
    tokens = _tokens(source)
    out: list[tuple[int, str]] = []
    values: dict[str, Any] = {}
    shown: dict[str, str] = {}
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok.type == tokenize.OP and tok.string == "@":
            nxt = tokens[i + 1] if i + 1 < len(tokens) else None
            if nxt is None or nxt.type != tokenize.NAME or nxt.start != tok.end:
                raise SyntaxError("'@' must be followed by a variable name, as in @keys")
            # The parser folds a name to NFKC, as Python stores a variable name.
            ident = unicodedata.normalize("NFKC", nxt.string)
            name = prefix + ident
            if name in values:
                pass
            elif ident in local_dict:
                values[name] = _plain(local_dict[ident])
            elif ident in global_dict:
                values[name] = _plain(global_dict[ident])
            else:
                raise NameError(f"name {ident!r} is not defined")
            shown[name] = "@" + ident
            out.append((tokenize.NAME, name))
            i += 2
            continue
        out.append((tok.type, tok.string))
        i += 1
    return str(tokenize.untokenize(out)), values, shown


def eval_statements(source: str) -> list[str]:
    """The text of each statement of a ``DataFrame.eval`` source, in order, so each is
    lowered against the frame the statements before it made. ``@`` names are already
    resolved (see :func:`resolve_at`)."""
    text = source.strip()
    return [ast.get_source_segment(text, s) or "" for s in ast.parse(text, mode="exec").body]


def _boolean_precedence(source: str) -> str:
    """pandas eval's rule: ``&`` and ``|`` mean ``and`` and ``or``, so ``a > 1 & b < 2``
    is ``(a > 1) and (b < 2)``, where Python binds ``&`` tighter than a comparison."""
    tokens = [
        (tokenize.NAME, "and" if tok.string == "&" else "or")
        if tok.type == tokenize.OP and tok.string in ("&", "|")
        else (tok.type, tok.string)
        for tok in _tokens(source)
    ]
    return str(tokenize.untokenize(tokens))


def transpile_expr(
    source: str,
    columns: Sequence[str],
    int_inputs: dict[str, bool] | None = None,
    variables: dict[str, Any] | None = None,
    kinds: dict[str, str] | None = None,
    shown: dict[str, str] | None = None,
) -> tuple[str | None, Expr]:
    """``(target, expr)`` for ``DataFrame.eval``: a Python expression over column names
    (``"a / b"``) or one assignment (``"m = a / b"``, target ``"m"``) lowered by the same
    code as :func:`transpile`. A bare name that is a column is its row field; any
    other name raises ``KeyError``, and an unsupported construct raises
    :class:`DFTUtilsTranspileError` naming it. ``int_inputs`` makes ``//`` and ``%`` integers.
    ``variables`` holds the values of the names :func:`resolve_at` made and
    ``shown`` their ``@name`` spelling; ``kinds`` maps a column to ``"num"``,
    ``"text"`` or ``"bool"``, so two columns of one kind compare directly."""
    body = ast.parse(_boolean_precedence(source.strip()), mode="exec").body
    if len(body) != 1:
        raise DFTUtilsTranspileError("expected one expression or one `name = expression`")
    stmt, target = body[0], None
    if isinstance(stmt, ast.Assign):
        if len(stmt.targets) != 1 or not isinstance(stmt.targets[0], ast.Name):
            raise DFTUtilsTranspileError("only `name = expression` assignments")
        target, value = stmt.targets[0].id, stmt.value
        if variables and target in variables:
            raise DFTUtilsTranspileError(f"cannot assign to {(shown or {}).get(target, target)}")
    elif isinstance(stmt, ast.Expr):
        value = stmt.value
    else:
        raise DFTUtilsTranspileError(f"statement {type(stmt).__name__} has no engine form")
    params = ast.arguments(
        posonlyargs=[],
        args=[ast.arg(arg=_ROW)],
        vararg=None,
        kwonlyargs=[],
        kw_defaults=[],
        kwarg=None,
        defaults=[],
    )
    found = dict(variables or {})
    node = ast.Lambda(args=params, body=_Columns(columns, list(found)).visit(value))
    return target, _Lowering(
        None,
        node,
        columns,
        None,
        0,
        int_inputs,
        consts={"math": math, **found},
        kinds=kinds,
        shown=shown,
    ).lower()
