"""Author a compose op (``dftu_op``) in Python and JIT-compile it to a native
plugin.

``@op`` turns a typed value transform into a compose leaf; ``|`` pipes them,
mirroring the C++ typed ``Op<In,Out>`` over the ``dftu_svc_compose`` ABI. Calling
an op JIT-compiles it and drives the ``dftu_op`` graph on a standalone compose
host (no plugin, no scan); the same op referenced from a ``@jit.each_event``
body is inlined into the plugin as a static C function instead.

Example::

    @op
    def double(x: jit.i64) -> jit.i64:
        return x * 2

    @op
    def plus10(x: jit.i64) -> jit.i64:
        return x + 10

    (double | plus10)(5)  # 5*2+10 = 20
"""

from __future__ import annotations

import ast
import inspect
import textwrap
from typing import TYPE_CHECKING, Callable, List, Optional, Sequence, Tuple, TypedDict, Union

from . import _plugin_build
from .enums import DType

if TYPE_CHECKING:
    from typing_extensions import TypeGuard

__all__ = [
    "op",
    "compile_op",
    "run_op",
    "run_op_array",
    "lower_to_expr",
    "can_fuse",
    "Op",
    "JitOpError",
]

# POD value types that cross the compose ABI as bytes, with their C type + size.
# DFTU_T_STR is an interned-string id (dftu_str, a uint32_t): supports only
# equality against a string literal, never arithmetic or ordering - id order
# is intern order, not string order. DFTU_T_BYTES is not included: it crosses
# as an opaque, variable-length dftu_bytes record (pointer + len + owning
# free_fn), not a fixed-size comparable value, so no op in this DSL applies to
# it.
_CTYPE = {
    "DFTU_T_I64": "int64_t",
    "DFTU_T_I32": "int32_t",
    "DFTU_T_I16": "int16_t",
    "DFTU_T_I8": "int8_t",
    "DFTU_T_U64": "uint64_t",
    "DFTU_T_U32": "uint32_t",
    "DFTU_T_U16": "uint16_t",
    "DFTU_T_U8": "uint8_t",
    "DFTU_T_F64": "double",
    "DFTU_T_F32": "float",
    "DFTU_T_STR": "dftu_str",
}
_SIZE = {
    "DFTU_T_I64": 8,
    "DFTU_T_I32": 4,
    "DFTU_T_I16": 2,
    "DFTU_T_I8": 1,
    "DFTU_T_U64": 8,
    "DFTU_T_U32": 4,
    "DFTU_T_U16": 2,
    "DFTU_T_U8": 1,
    "DFTU_T_F64": 8,
    "DFTU_T_F32": 4,
    "DFTU_T_STR": 4,
}
_FLOAT = {"DFTU_T_F64", "DFTU_T_F32"}
_STR = "DFTU_T_STR"
_ARITH = {
    ast.Add: "+",
    ast.Sub: "-",
    ast.Mult: "*",
    ast.Div: "/",
    ast.Mod: "%",
}
_CMP = {
    ast.Eq: "==",
    ast.NotEq: "!=",
    ast.Lt: "<",
    ast.Gt: ">",
    ast.LtE: "<=",
    ast.GtE: ">=",
}
# The only comparators an interned id supports; equality is id equality
# (equal iff the underlying strings are), never an ordering.
_STR_CMP = {ast.Eq: "==", ast.NotEq: "!="}

# columnar.py's post-order AST opcodes for _ext.vec_eval (mirror columnar_eval.cpp).
_AST_COL, _AST_LIT_I, _AST_LIT_F, _AST_BIN = 0, 1, 2, 3
_AST_CAST, _AST_UNARY = 8, 9
_EXPR_BIN = {ast.Add: 0, ast.Sub: 1, ast.Mult: 2, ast.Div: 3}  # no Mod
_UNARY_ABS, _UNARY_NEGATE = 0, 8
_NONCOMMUTATIVE = {ast.Sub, ast.Div}
# Op value types the Expr engine can target today (dataframe::TypeId ordinals).
_FUSABLE_TYPE_ID = {"DFTU_T_I64": int(DType.INT64), "DFTU_T_F64": int(DType.FLOAT64)}


class JitOpError(Exception):
    """A compose-op body outside the supported subset (arithmetic on the input)."""


# One leaf stage of an Op: the string fields feed the C emitter, `node` is the
# parsed body expression `_lower_expr` re-lowers to a columnar Expr for fusion,
# and `literals` are the DFTU_T_STR placeholders that stage's `expr` references.
Stage = TypedDict(
    "Stage",
    {
        "in": str,
        "out": str,
        "expr": str,
        "node": ast.expr,
        "var": str,
        "literals": List[str],
    },
)


class Op:
    """A composable value transform, one or more leaf stages piped in order."""

    def __init__(self, stages: List[Stage]):
        self.stages = stages

    @property
    def in_type(self) -> str:
        return self.stages[0]["in"]

    @property
    def out_type(self) -> str:
        return self.stages[-1]["out"]

    def __or__(self, other: "Op") -> "Op":
        if not isinstance(other, Op):
            return NotImplemented
        if self.out_type != other.in_type:
            raise JitOpError(
                f"cannot pipe {self.out_type} -> {other.in_type}: the middle type must chain"
            )
        return Op(self.stages + other.stages)

    def __call__(self, value: Union[int, float]) -> Union[int, float]:
        """Run this op over `value` in plain Python: JIT-compile it and drive the
        graph on a standalone compose host, returning the output value.

        Inside a ``@jit.each_event`` body an op call is inlined into the plugin
        at compile time instead - the body is AST-compiled, never executed, so
        this Python path never runs there."""
        return run_op(self, value)


# Python's own numeric annotations, for an op that does not need a specific
# width. float is an IEEE double, so f64 is exact rather than a guess; int is
# arbitrary precision, and i64 is the width numpy and pandas also choose for it.
_BUILTIN_TYPE = {float: "DFTU_T_F64", int: "DFTU_T_I64"}


def _type_of(t: object) -> str:
    dft = getattr(t, "dft", None) or _BUILTIN_TYPE.get(t)  # type: ignore[arg-type]
    if dft not in _CTYPE:
        raise JitOpError(
            f"unsupported op value type {t!r}; use float, int, or a width from "
            "jit.i64/i32/u64/f64/..."
        )
    return dft


def _lower(node: ast.expr, var: str, in_type: str, literals: List[str]) -> str:
    """Lower an op body expression on the single input `var` to C, with the
    input bound to the local `x`. `literals` collects the string constants a
    DFTU_T_STR comparison references, as placeholders `__LIT<n>__` the
    caller resolves to an interned id once the host is known (see `_emit`)."""
    if isinstance(node, ast.Compare):
        if len(node.ops) != 1 or len(node.comparators) != 1:
            raise JitOpError("only a single comparison is allowed in an op body")
        cmp_op = node.ops[0]
        sym = _CMP.get(type(cmp_op), type(cmp_op).__name__)
        if in_type != _STR or type(cmp_op) not in _STR_CMP:
            raise JitOpError(
                f"operator {sym!r} is not supported on {in_type}; DFTU_T_STR "
                "supports only == and != against a string literal (an interned "
                "id's order is intern order, not string order), and no other "
                "op value type supports a comparison here"
            )
        lit, name_node = _str_literal_operand(node.left, node.comparators[0], var)
        idx = len(literals)
        literals.append(lit)
        return f"({_lower(name_node, var, in_type, literals)} {sym} __LIT{idx}__)"
    if in_type == _STR:
        if isinstance(node, ast.Name) and node.id == var:
            return "x"
        raise JitOpError(_str_op_error(_op_name(node)))
    if isinstance(node, ast.BinOp):
        sym = _ARITH.get(type(node.op))
        if sym is None:
            raise JitOpError("only + - * / % are allowed in an op body")
        return (
            f"({_lower(node.left, var, in_type, literals)} {sym} "
            f"{_lower(node.right, var, in_type, literals)})"
        )
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        return f"(-{_lower(node.operand, var, in_type, literals)})"
    if isinstance(node, ast.Name):
        if node.id != var:
            raise JitOpError(f"unknown name {node.id!r}; an op body uses only its input {var!r}")
        return "x"
    if (
        isinstance(node, ast.Constant)
        and isinstance(node.value, (int, float))
        and not isinstance(node.value, bool)
    ):
        return repr(node.value)
    if _is_abs_call(node):
        inner = _lower(node.args[0], var, in_type, literals)
        return f"(({inner}) < 0 ? -({inner}) : ({inner}))"
    raise JitOpError("op body must be `return <arithmetic on the input>`")


def _op_name(node: ast.expr) -> str:
    if isinstance(node, ast.BinOp):
        return _ARITH.get(type(node.op), type(node.op).__name__)
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        return "-"
    if _is_abs_call(node):
        return "abs"
    return "this operation"


def _str_op_error(sym: str) -> str:
    return (
        f"operator {sym!r} is not supported on DFTU_T_STR: an interned id has "
        "no arithmetic and no ordering (id order is intern order, not string "
        "order); only == and != against a string literal are allowed"
    )


def _str_literal_operand(left: ast.expr, right: ast.expr, var: str) -> Tuple[str, ast.expr]:
    if isinstance(left, ast.Name) and left.id == var:
        lit = _str_const_value(right)
        if lit is not None:
            return lit, left
    if isinstance(right, ast.Name) and right.id == var:
        lit = _str_const_value(left)
        if lit is not None:
            return lit, right
    raise JitOpError(
        'a DFTU_T_STR comparison must be `<input> == "literal"` or `<input> != "literal"`'
    )


def _str_const_value(node: ast.expr) -> Optional[str]:
    return node.value if isinstance(node, ast.Constant) and isinstance(node.value, str) else None


def _is_abs_call(node: ast.expr) -> TypeGuard[ast.Call]:
    return (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "abs"
        and len(node.args) == 1
        and not node.keywords
    )


def _lower_expr(
    node: ast.expr, var: str, leaf: List[tuple], var_is_float: bool
) -> Optional[Tuple[List[tuple], bool, bool]]:
    """Lower `node` to a columnar Expr AST, splicing `leaf` in for each
    reference to `var`. Returns `(tokens, is_float, is_scalar)` or None if
    there is no Expr equivalent - refusing an integer `/` (C truncates; Expr's
    Div always promotes to Float64 first) and `scalar - column` / `scalar /
    column` (no such kernel; Add/Mul commute instead of needing one)."""
    if isinstance(node, ast.BinOp):
        code = _EXPR_BIN.get(type(node.op))
        if code is None:
            return None
        left = _lower_expr(node.left, var, leaf, var_is_float)
        if left is None:
            return None
        left_tok, left_f, left_s = left
        right = _lower_expr(node.right, var, leaf, var_is_float)
        if right is None:
            return None
        right_tok, right_f, right_s = right
        if type(node.op) in _NONCOMMUTATIVE and left_s and not right_s:
            return None
        if isinstance(node.op, ast.Div) and not left_f and not right_f:
            return None
        out_f = left_f or right_f or isinstance(node.op, ast.Div)
        return left_tok + right_tok + [(_AST_BIN, code)], out_f, left_s and right_s
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        inner = _lower_expr(node.operand, var, leaf, var_is_float)
        if inner is None:
            return None
        inner_tok, inner_f, inner_s = inner
        return inner_tok + [(_AST_UNARY, _UNARY_NEGATE)], inner_f, inner_s
    if _is_abs_call(node):
        inner = _lower_expr(node.args[0], var, leaf, var_is_float)
        if inner is None:
            return None
        inner_tok, inner_f, inner_s = inner
        return inner_tok + [(_AST_UNARY, _UNARY_ABS)], inner_f, inner_s
    if isinstance(node, ast.Name):
        return (list(leaf), var_is_float, False) if node.id == var else None
    if (
        isinstance(node, ast.Constant)
        and isinstance(node.value, (int, float))
        and not isinstance(node.value, bool)
    ):
        if isinstance(node.value, int):
            return [(_AST_LIT_I, node.value)], False, True
        return [(_AST_LIT_F, node.value)], True, True
    return None


# fn is an arbitrary user-authored one-argument function (jit-typed annotations).
def op(fn: Callable[..., object]) -> Op:
    """Turn a typed one-argument function into a compose leaf.

    The body must be a single ``return <arithmetic on the argument>``; anything
    else raises :class:`JitOpError` at decoration time.
    """
    ann = getattr(fn, "__annotations__", {})
    tree = ast.parse(textwrap.dedent(inspect.getsource(fn)))
    func = tree.body[0]
    if not isinstance(func, ast.FunctionDef) or len(func.args.args) != 1:
        raise JitOpError("@op needs a function of exactly one typed argument")
    var = func.args.args[0].arg
    if var not in ann or "return" not in ann:
        raise JitOpError("@op needs annotations on the argument and the return")
    in_type = _type_of(ann[var])
    out_type = _type_of(ann["return"])
    # Skip a leading docstring; the rest must be a single return.
    body = [s for s in func.body if not isinstance(s, ast.Expr)]
    if len(body) != 1 or not isinstance(body[0], ast.Return) or body[0].value is None:
        raise JitOpError("@op body must be a single `return <expr>`")
    literals: List[str] = []
    expr = _lower(body[0].value, var, in_type, literals)
    if literals and (out_type in _FLOAT or out_type in (_STR, "DFTU_T_BYTES")):
        raise JitOpError(
            f"a DFTU_T_STR comparison must return an integer type (0/1), not {out_type}"
        )
    return Op(
        [
            {
                "in": in_type,
                "out": out_type,
                "expr": expr,
                "node": body[0].value,
                "var": var,
                "literals": literals,
            }
        ]
    )


def _emit(op_obj: Op) -> str:
    """Emit a .so that builds `op_obj` as a dftu_op graph on a caller-provided
    host. The standalone runner (native OpRunner binding) supplies the host and
    drives the run; there is no plugin/scan here."""
    lines: List[str] = [
        "#include <dftracer/utils/plugins/abi.h>",
        "#include <stdint.h>",
        "",
    ]
    for i, st in enumerate(op_obj.stages):
        ict = _CTYPE[st["in"]]
        oct_ = _CTYPE[st["out"]]
        literals: List[str] = st["literals"]
        for j in range(len(literals)):
            lines.append(f"static dftu_str leaf_{i}_lit_{j};")
        expr = st["expr"]
        for j in range(len(literals)):
            expr = expr.replace(f"__LIT{j}__", f"leaf_{i}_lit_{j}")
        lines += [
            f"static dftu_task* leaf_{i}(void* s, const void* in, void* out, int* rc) {{",
            "    (void)s;",
            f"    {ict} x = *(const {ict}*)in;",
            f"    *({oct_}*)out = ({oct_})({expr});",
            "    if (rc) *rc = 0;",
            "    return NULL;",
            "}",
            "",
        ]
    lines += [
        "#ifdef __cplusplus",
        'extern "C"',
        "#endif",
        "dftu_op* dftracer_build_op(const dftu_plugin_host* host) {",
        "    const dftu_svc_compose* c = (const dftu_svc_compose*)"
        "host->get_service(host->h, DFTU_SVC_COMPOSE);",
        "    if (!c) return 0;",
    ]
    # A string literal interns to a stable id for this host's whole lifetime
    # (the graph runs once), so resolving it once here - rather than lazily
    # inside the leaf, which never sees the host - is correct and simplest.
    for i, st in enumerate(op_obj.stages):
        for j, lit in enumerate(st["literals"]):
            b = lit.encode("utf-8")
            lines.append(
                f"    leaf_{i}_lit_{j} = host->intern(host->h, {_c_str_literal(lit)}, {len(b)});"
            )
    for i, st in enumerate(op_obj.stages):
        lines.append(
            f"    dftu_op* op_{i} = c->make_op(host->h, leaf_{i}, 0, 0, "
            f"{st['in']}, {_SIZE[st['in']]}, {st['out']}, {_SIZE[st['out']]});"
        )
    lines.append("    dftu_op* g = op_0;")
    for i in range(1, len(op_obj.stages)):
        lines.append(f"    g = c->then(host->h, g, op_{i});")
    lines += ["    return g;", "}", ""]
    return "\n".join(lines)


def _c_str_literal(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_inline(op_obj: Op, c_name: str) -> str:
    """Emit `op_obj` as scalar static C functions callable as ``c_name(x)``.

    Used to inline an op referenced from a ``@jit.each_event`` body: each stage
    becomes ``c_name__s<i>`` and ``c_name`` chains them, so the plugin calls the
    op directly with no dftu_op graph or host round-trip."""
    for st in op_obj.stages:
        if st["literals"]:
            raise JitOpError(
                "a DFTU_T_STR comparison op cannot be inlined into a "
                "@jit.each_event body yet (the literal needs a host to "
                "intern, which inlining does not thread through); call it "
                "standalone instead"
            )
    lines: List[str] = []
    for i, st in enumerate(op_obj.stages):
        ict = _CTYPE[st["in"]]
        oct_ = _CTYPE[st["out"]]
        lines.append(f"static {oct_} {c_name}__s{i}({ict} x) {{ return ({oct_})({st['expr']}); }}")
    call = "x"
    for i in range(len(op_obj.stages)):
        call = f"{c_name}__s{i}({call})"
    lines.append(
        f"static {_CTYPE[op_obj.out_type]} {c_name}"
        f"({_CTYPE[op_obj.in_type]} x) {{ return {call}; }}"
    )
    return "\n".join(lines)


def lower_to_expr(op_obj: Op) -> Optional[List[tuple]]:
    """Try to lower every stage of `op_obj` to one columnar Expr AST (the post-
    order form columnar.py serializes for ``_ext.vec_eval``), casting between
    stages where the scalar leaf would cast too. None if a stage's type is
    outside ``_FUSABLE_TYPE_ID`` or has no Expr equivalent (see
    :func:`_lower_expr`); the caller then falls back. See :func:`can_fuse`
    for the yes/no-only form."""
    if op_obj.in_type not in _FUSABLE_TYPE_ID:
        return None
    ast_tokens: List[tuple] = [(_AST_COL, 0)]
    for st in op_obj.stages:
        if st["out"] not in _FUSABLE_TYPE_ID:
            return None
        lowered = _lower_expr(st["node"], st["var"], ast_tokens, is_float_type(st["in"]))
        if lowered is None:
            return None
        tokens, _, _ = lowered
        ast_tokens = tokens + [(_AST_CAST, _FUSABLE_TYPE_ID[st["out"]])]
    return ast_tokens


def can_fuse(op_obj: Op) -> bool:
    """True if `op_obj` lowers to the columnar Expr engine (see
    :func:`lower_to_expr`); the inspectable fuse/fallback decision."""
    return lower_to_expr(op_obj) is not None


def is_float_type(dft: str) -> bool:
    """True if `dft` (a DFTU_T_* name) is a floating-point op value type."""
    return dft in _FLOAT


def c_type(dft: str) -> str:
    """C scalar type for a DFTU_T_* op value type."""
    return _CTYPE[dft]


def compile_op(op_obj: Op, *, name: str = "op") -> str:
    """Emit and build a .so exposing ``dftracer_build_op(host)`` for `op_obj`.
    Returns the cached ``.so`` path (run it via :func:`run_op`)."""
    source = _emit(op_obj)
    include = _plugin_build.include_dir()
    cxx = _plugin_build.compiler()
    digest = _plugin_build.source_digest(source, include, cxx)
    out = _plugin_build.cache_dir() / f"{name}.{digest}.so"
    if not out.is_file():
        src = out.with_suffix(".cpp")
        src.write_text(source, encoding="utf-8")
        _plugin_build.build_shared(str(src), out=str(out), name=name)
    return str(out)


def run_op(op_obj: Op, value: Union[int, float]) -> Union[int, float]:
    """Compile `op_obj`, then build and run its dftu_op graph over `value` on a
    standalone compose host (no plugin, no scan), returning the output value."""
    import struct

    from .dftracer_utils_ext import jit_run_op  # native standalone runner

    so = compile_op(op_obj)
    in_fmt = _STRUCT[op_obj.in_type]
    out_fmt = _STRUCT[op_obj.out_type]
    in_bytes = struct.pack("=" + in_fmt, value)
    out_bytes = jit_run_op(so, in_bytes, _SIZE[op_obj.out_type])
    return struct.unpack("=" + out_fmt, out_bytes)[0]


_STRUCT = {
    "DFTU_T_I64": "q",
    "DFTU_T_I32": "i",
    "DFTU_T_I16": "h",
    "DFTU_T_I8": "b",
    "DFTU_T_U64": "Q",
    "DFTU_T_U32": "I",
    "DFTU_T_U16": "H",
    "DFTU_T_U8": "B",
    "DFTU_T_F64": "d",
    "DFTU_T_F32": "f",
    "DFTU_T_STR": "I",
}

_ArrayValue = Optional[Union[int, float]]


def run_op_array(
    op_obj: Op, values: Sequence[_ArrayValue], *, force_scalar: bool = False
) -> Tuple[List[_ArrayValue], bool]:
    """Run `op_obj` over an array of values (``None`` passes through as null on
    both paths). Runs the Expr engine once over the array when `op_obj` lowers
    to it (:func:`lower_to_expr`); otherwise, or with `force_scalar`, runs the
    existing per-element :func:`run_op` path. Returns ``(results, fused)`` -
    `fused` is the inspectable lowering decision."""
    ast_tokens = None if force_scalar else lower_to_expr(op_obj)
    if ast_tokens is None:
        return [run_op(op_obj, v) if v is not None else None for v in values], False
    return _run_fused(op_obj, ast_tokens, values), True


def _run_fused(
    op_obj: Op, ast_tokens: List[tuple], values: Sequence[_ArrayValue]
) -> List[_ArrayValue]:
    import pyarrow as pa  # ty: ignore[unresolved-import]

    from .dftracer_utils_ext import vec_eval
    from .series import Series, _unwrap

    pa_type = pa.float64() if is_float_type(op_obj.in_type) else pa.int64()
    col = Series.from_list(list(values), dtype=pa_type)
    out = Series(vec_eval(ast_tokens, [_unwrap(col)]))
    return out.to_arrow().to_pylist()
