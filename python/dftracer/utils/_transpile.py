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

Anything outside that subset raises :class:`TranspileError` with the exact
construct, so the caller can fall back and say why.
"""

from __future__ import annotations

import ast
import inspect
import math
import textwrap
from typing import Any, Callable, Dict, List, Optional, Sequence

from .columnar import Expr, _Select, col, lit

# Python 3.8 wraps a subscript's slice in ast.Index; 3.9+ stores the value
# directly.
_AST_INDEX = getattr(ast, "Index", None)


def _unwrap_index(node: ast.expr) -> ast.expr:
    if _AST_INDEX is not None and isinstance(node, _AST_INDEX):
        return getattr(node, "value", node)
    return node


class TranspileError(Exception):
    """The function uses something with no engine form; the message names it."""


_BIN = {ast.Add: "+", ast.Sub: "-", ast.Mult: "*", ast.Div: "/"}
_CMP = {ast.Lt: "lt", ast.LtE: "le", ast.Gt: "gt", ast.GtE: "ge", ast.Eq: "eq", ast.NotEq: "ne"}
_FLIP = {"lt": "gt", "le": "ge", "gt": "lt", "ge": "le", "eq": "eq", "ne": "ne"}
_MATH = {"sqrt": "sqrt", "log": "log", "exp": "exp", "floor": "floor", "ceil": "ceil"}
_STR_MAP = {
    "lower": "lower",
    "upper": "upper",
    "strip": "strip",
    "lstrip": "lstrip",
    "rstrip": "rstrip",
}


def _source_node(func: Callable[..., object]) -> ast.AST:
    """The ``FunctionDef`` or ``Lambda`` node of ``func``'s own source."""
    try:
        src = textwrap.dedent(inspect.getsource(func))
    except (OSError, TypeError) as e:
        raise TranspileError(f"no source available ({e})") from None
    try:
        tree = ast.parse(src)
    except SyntaxError:
        # A lambda inside a larger expression: the line does not parse alone.
        # Wrap so the lambda's own text is what we look at.
        try:
            tree = ast.parse("(" + src.strip() + ")")
        except SyntaxError as e:
            raise TranspileError(f"source does not parse alone ({e.msg})") from None
    code = getattr(func, "__code__", None)
    if code is None:
        raise TranspileError("not a plain Python function")
    argnames = list(code.co_varnames[: code.co_argcount])
    fname = str(getattr(func, "__name__", "<lambda>"))
    if fname != "<lambda>":
        for node in ast.walk(tree):
            if isinstance(node, ast.FunctionDef) and node.name == fname:
                return node
        raise TranspileError(f"function {fname!r} not found in its source")
    lambdas = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Lambda) and [a.arg for a in node.args.args] == argnames
    ]
    if len(lambdas) != 1:
        raise TranspileError(
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
        func: Callable[..., object],
        node: ast.AST,
        columns: Optional[Sequence[str]],
        bound: Optional[Dict[str, Expr]] = None,
        depth: int = 0,
        int_inputs: Optional[Dict[str, bool]] = None,
        bound_int: Optional[Dict[str, bool]] = None,
    ) -> None:
        self.func = func
        self.node = node
        self.columns = list(columns) if columns is not None else None
        self.depth = depth
        self.int_inputs: Dict[str, bool] = dict(int_inputs or {})
        self.bound_int: Dict[str, bool] = dict(bound_int or {})
        self.local_int: Dict[str, bool] = {}
        args = node.args.args if isinstance(node, (ast.FunctionDef, ast.Lambda)) else []
        names = [a.arg for a in args]
        if columns is not None:
            if len(names) != 1:
                raise TranspileError("a row function takes exactly one argument")
            self.row_param: Optional[str] = names[0]
            self.bound: Dict[str, Expr] = {}
        else:
            self.row_param = None
            if bound is None:
                if len(names) != 1:
                    raise TranspileError("the function must take exactly one argument")
                bound = {names[0]: col("__x0__")}
                self.bound_int = {names[0]: self.int_inputs.get("__x0__", False)}
            if list(bound) != names:
                raise TranspileError(
                    f"{getattr(func, '__name__', 'function')} takes {len(names)} argument(s), got {len(bound)}"
                )
            self.bound = dict(bound)
        self.locals: Dict[str, Expr] = {}
        cv = inspect.getclosurevars(func)
        self.consts: Dict[str, Any] = {**cv.globals, **cv.nonlocals}

    # -- entry -----------------------------------------------------------------
    def lower(self) -> Expr:
        if isinstance(self.node, ast.Lambda):
            return self.expr(self.node.body)
        if not isinstance(self.node, ast.FunctionDef):
            raise TranspileError("not a function")
        body = [s for s in self.node.body if not _is_docstring(s)]
        out = self.block(body)
        if out is None:
            raise TranspileError("the function has a path with no return")
        return out

    # -- statements ------------------------------------------------------------
    def block(self, stmts: List[ast.stmt]) -> Optional[Expr]:
        """The value a statement list returns, or None if it falls through."""
        for i, s in enumerate(stmts):
            if isinstance(s, ast.Return):
                if s.value is None:
                    raise TranspileError("a bare return has no value")
                return self.expr(s.value)
            if isinstance(s, ast.Assign):
                if len(s.targets) != 1 or not isinstance(s.targets[0], ast.Name):
                    raise TranspileError("only `name = value` assignments")
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
                    raise TranspileError("every branch of an `if` must return")
                return _Select(cond, then, otherwise)
            if isinstance(s, ast.Pass):
                continue
            raise TranspileError(f"statement {type(s).__name__} has no engine form")
        return None

    # -- expressions -----------------------------------------------------------
    def expr(self, n: ast.expr) -> Expr:
        if isinstance(n, ast.Constant):
            if isinstance(n.value, bool) or not isinstance(n.value, (int, float)):
                raise TranspileError(f"constant {n.value!r} is not a number")
            return lit(n.value)
        if isinstance(n, ast.Name):
            return self.name(n.id)
        if isinstance(n, ast.Subscript):
            return self.field(n.value, _unwrap_index(n.slice))
        if isinstance(n, ast.Attribute):
            if isinstance(n.value, ast.Name) and n.value.id == self.row_param:
                return self.column(n.attr)
            raise TranspileError(f"attribute .{n.attr} is not a row field")
        if isinstance(n, ast.BinOp):
            return self.binop(n)
        if isinstance(n, ast.UnaryOp):
            if isinstance(n.op, ast.USub):
                inner = self.scalar_or_expr(n.operand)
                if isinstance(inner, Expr):
                    return inner * -1
                return lit(-inner)
            if isinstance(n.op, ast.Not):
                return ~self.expr(n.operand)
            raise TranspileError(f"unary {type(n.op).__name__} has no engine form")
        if isinstance(n, ast.BoolOp):
            parts = [self.expr(v) for v in n.values]
            out = parts[0]
            for p in parts[1:]:
                out = (out & p) if isinstance(n.op, ast.And) else (out | p)
            return out
        if isinstance(n, ast.Compare):
            return self.compare(n)
        if isinstance(n, ast.IfExp):
            return _Select(self.expr(n.test), self.expr(n.body), self.expr(n.orelse))
        if isinstance(n, ast.Call):
            return self.call(n)
        raise TranspileError(f"expression {type(n).__name__} has no engine form")

    def binop(self, n: ast.BinOp) -> Expr:
        left, right = self.scalar_or_expr(n.left), self.scalar_or_expr(n.right)
        if not isinstance(left, Expr) and not isinstance(right, Expr):
            raise TranspileError("arithmetic on two constants")
        op = _BIN.get(type(n.op))
        if op is not None:
            return {
                "+": lambda a, b: a + b,
                "-": lambda a, b: a - b,
                "*": lambda a, b: a * b,
                "/": lambda a, b: a / b,
            }[op](left, right)
        both_int = self.is_int(n.left) and self.is_int(n.right)
        if isinstance(n.op, ast.FloorDiv):
            # Python floors toward -inf: floor(a / b), back to int for ints.
            q = (left / right).floor()
            return q.cast("int64") if both_int else q
        if isinstance(n.op, ast.Mod):
            # Python's sign follows the divisor: a - floor(a / b) * b.
            r = left - (left / right).floor() * right
            return r.cast("int64") if both_int else r
        if isinstance(n.op, ast.Pow):
            if not isinstance(left, Expr):
                raise TranspileError("a constant base has no engine form")
            if right == 0.5:
                return left.sqrt()
            if isinstance(right, int) and not isinstance(right, bool) and 0 <= right <= 8:
                if right == 0:
                    return left * 0 + 1
                out = left
                for _ in range(right - 1):
                    out = out * left
                return out
            raise TranspileError("** needs a constant exponent in 0..8 or 0.5")
        raise TranspileError(f"operator {type(n.op).__name__} has no engine form")

    def is_int(self, n: ast.expr) -> bool:
        """Whether a subtree is integer-valued: an int constant, an integer
        input column, an integer local, `int(...)`, or `+ - * // %` on those.
        Decides whether `//` and `%` cast back to Int64, as Python does."""
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

    def field_name(self, n: ast.expr) -> Optional[str]:
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

    def scalar_or_expr(self, n: ast.expr) -> Any:
        """A Python number for a constant subtree, else an Expr."""
        v = self.constant(n)
        if v is not None:
            return v
        return self.expr(n)

    def constant(self, n: ast.expr) -> Any:
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
            raise TranspileError("a row is not a value; index it with row['column'] or row.column")
        if ident in self.locals:
            return self.locals[ident]
        if ident in self.bound:
            return self.bound[ident]
        if ident in self.consts:
            v = self.consts[ident]
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                return lit(v)
            raise TranspileError(f"{ident!r} is a {type(v).__name__}, not a number")
        raise TranspileError(f"unknown name {ident!r}")

    def field(self, value: ast.expr, key: ast.expr) -> Expr:
        if not (isinstance(value, ast.Name) and value.id == self.row_param):
            raise TranspileError("only the row argument can be indexed")
        k = key.value if isinstance(key, ast.Constant) else None
        if not isinstance(k, str):
            raise TranspileError("row[...] needs a literal column name")
        return self.column(k)

    def column(self, name: str) -> Expr:
        if self.columns is None:
            raise TranspileError("a value has no fields to index")
        if name not in self.columns:
            raise KeyError(f"no column named {name!r}")
        return col(name)

    def compare(self, n: ast.Compare) -> Expr:
        pairs = list(zip([n.left, *n.comparators[:-1]], n.ops, n.comparators))
        out: Optional[Expr] = None
        for left, op, right in pairs:
            if isinstance(op, (ast.In, ast.NotIn)):
                part = self.membership(left, right, negate=isinstance(op, ast.NotIn))
            elif isinstance(op, (ast.Is, ast.IsNot)):
                part = self.null_test(left, right, negate=isinstance(op, ast.IsNot))
            else:
                code = _CMP.get(type(op))
                if code is None:
                    raise TranspileError(f"comparison {type(op).__name__} has no engine form")
                lv, rv = self.scalar_or_expr(left), self.scalar_or_expr(right)
                if isinstance(lv, Expr) and not isinstance(rv, Expr):
                    part = getattr(lv, code)(rv) if code in ("eq", "ne") else _cmp(lv, code, rv)
                elif isinstance(rv, Expr) and not isinstance(lv, Expr):
                    code = _FLIP[code]
                    part = getattr(rv, code)(lv) if code in ("eq", "ne") else _cmp(rv, code, lv)
                elif isinstance(lv, Expr) and isinstance(rv, Expr):
                    if code not in ("eq", "ne"):
                        # a < b  <=>  (a - b) < 0
                        part = _cmp(lv - rv, code, 0)
                    else:
                        part = getattr(lv - rv, code)(0)
                else:
                    raise TranspileError("comparison of two constants")
            out = part if out is None else (out & part)
        assert out is not None
        return out

    def inline(self, callee: Callable[..., object], args: List[ast.expr]) -> Expr:
        """A call to another plain Python function: lower its body with each
        parameter bound to the argument expression (or constant)."""
        if self.depth >= _MAX_INLINE_DEPTH:
            raise TranspileError("calls nest too deep (recursion?)")
        node = _source_node(callee)
        params = (
            [a.arg for a in node.args.args]
            if isinstance(node, (ast.FunctionDef, ast.Lambda))
            else []
        )
        if len(params) != len(args):
            raise TranspileError(
                f"{getattr(callee, '__name__', 'function')} takes {len(params)} argument(s), called with {len(args)}"
            )
        bound: Dict[str, Expr] = {}
        ints: Dict[str, bool] = {}
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
            raise TranspileError("`is` compares a value with None only")
        value = self.scalar_or_expr(right if none_left else left)
        if not isinstance(value, Expr):
            raise TranspileError("`is None` needs a value on the other side")
        return value.is_not_null() if negate else value.is_null()

    def membership(self, left: ast.expr, right: ast.expr, negate: bool) -> Expr:
        lv = self.scalar_or_expr(left)
        if isinstance(lv, str):
            # "sub" in value: a substring test
            return ~self.expr(right).contains(lv) if negate else self.expr(right).contains(lv)
        if not isinstance(lv, Expr):
            raise TranspileError("`in` needs a value on the left")
        if isinstance(right, (ast.List, ast.Tuple, ast.Set)):
            values = [self.constant(e) for e in right.elts]
        elif isinstance(right, ast.Name) and right.id in self.consts:
            values = list(self.consts[right.id])
        else:
            raise TranspileError("`in` needs a literal list or a constant on the right")
        if any(v is None for v in values):
            raise TranspileError("`in` needs a list of constants")
        return lv.not_in(values) if negate else lv.is_in(values)

    def call(self, n: ast.Call) -> Expr:
        if n.keywords:
            raise TranspileError("keyword arguments have no engine form")
        f = n.func
        if isinstance(f, ast.Name):
            name = f.id
            args = n.args
            if name == "abs" and len(args) == 1:
                return self.expr(args[0]).abs()
            if name in ("min", "max") and len(args) >= 2:
                vals = [self.scalar_or_expr(a) for a in args]
                if not any(isinstance(v, Expr) for v in vals):
                    raise TranspileError(f"{name} of constants only")
                out = vals[0] if isinstance(vals[0], Expr) else lit(vals[0])
                for v in vals[1:]:
                    ve = v if isinstance(v, Expr) else lit(v)
                    cond = _cmp(out - ve, "lt" if name == "min" else "gt", 0)
                    out = _Select(cond, out, ve)
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
            if callable(callee) and hasattr(callee, "__code__"):
                return self.inline(callee, args)
            raise TranspileError(f"call to {name!r} has no engine form")
        if isinstance(f, ast.Attribute):
            if isinstance(f.value, ast.Name) and self.consts.get(f.value.id) is math:
                op = _MATH.get(f.attr)
                if op is None or len(n.args) != 1:
                    raise TranspileError(f"math.{f.attr} has no engine form")
                return getattr(self.expr(n.args[0]), op)()
            target = self.expr(f.value)
            method = f.attr
            if method in _STR_MAP and not n.args:
                return getattr(target, _STR_MAP[method])()
            if method in ("startswith", "endswith") and len(n.args) == 1:
                pat = self.constant(n.args[0])
                if not isinstance(pat, str):
                    raise TranspileError(f"{method} needs a literal string")
                return target.starts_with(pat) if method == "startswith" else target.ends_with(pat)
            if method == "replace" and len(n.args) == 2:
                old, new = self.constant(n.args[0]), self.constant(n.args[1])
                if not isinstance(old, str) or not isinstance(new, str):
                    raise TranspileError("replace needs two literal strings")
                return target.replace_all(old, new)
            raise TranspileError(f"method .{method} has no engine form")
        raise TranspileError("call target has no engine form")


def _cmp(e: Expr, code: str, value: Any) -> Expr:
    return {"lt": e.__lt__, "le": e.__le__, "gt": e.__gt__, "ge": e.__ge__, "eq": e.eq, "ne": e.ne}[
        code
    ](value)


def _is_docstring(s: ast.stmt) -> bool:
    return (
        isinstance(s, ast.Expr)
        and isinstance(s.value, ast.Constant)
        and isinstance(s.value.value, str)
    )


def transpile(
    func: Callable[..., object],
    columns: Optional[Sequence[str]] = None,
    int_inputs: Optional[Dict[str, bool]] = None,
) -> Expr:
    """``func`` as an ``Expr``. With ``columns`` the one argument is a row and
    its fields are those columns; without, it is a value, ``col("__x0__")``.
    ``int_inputs`` says which inputs (column names, or ``"__x0__"``) hold
    integers, so ``//`` and ``%`` come back as Int64 the way Python does.
    Raises :class:`TranspileError` naming the first unsupported construct, and
    ``KeyError`` for a field that is not a column."""
    return _Lowering(func, _source_node(func), columns, None, 0, int_inputs).lower()
