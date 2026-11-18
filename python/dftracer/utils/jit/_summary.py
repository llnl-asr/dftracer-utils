# SPDX-License-Identifier: MIT
"""A summary authored in Python: ``add``, ``merge`` and ``may_contain`` are
AST-compiled to a native index extension.

The class declares its state as annotated ``int`` or ``float`` fields with a
default, the state of an empty chunk; the ``paths`` it summarizes and the
``name`` it registers under::

    @jit.summary(name="acme.size_range", paths=["args.size"])
    class SizeRange:
        n: int = 0
        lo: float = 0.0
        hi: float = 0.0

        def add(self, value):
            if self.n == 0 or value < self.lo:
                self.lo = value
            if self.n == 0 or value > self.hi:
                self.hi = value
            self.n += 1

        def merge(self, other):
            if other.n > 0:
                if self.n == 0 or other.lo < self.lo:
                    self.lo = other.lo
                if self.n == 0 or other.hi > self.hi:
                    self.hi = other.hi
                self.n += other.n

        def may_contain(self, cond):
            if self.n == 0:
                return False
            if cond.op == jit.EQ:
                return self.lo <= cond.value and cond.value <= self.hi
            ...
            return True

The built plugin runs ``add`` on every numeric value of each path in each chunk
and ``merge`` of the chunk states into the file state, stores the state as the
payload, and runs ``may_contain`` on a stored state for each comparison leaf
(``==``, ``<``, ``<=``, ``>``, ``>=``) on a path. It is plain C: it holds no
reference to Python, so a query that uses it runs no Python code.
"""

from __future__ import annotations

import ast
import builtins
import hashlib
import re
from typing import Callable, Sequence, TypeVar, cast

from .._jit_errors import JitError, Source, class_error
from .maps import own_annotations
from .package import DEFAULT_PACKAGE
from .plugin import JitPlugin

C = TypeVar("C", bound=type)


class Cond:
    """The filter leaf ``path <op> value`` that ``may_contain`` judges: ``op`` is
    one of ``jit.EQ``, ``jit.LT``, ``jit.LE``, ``jit.GT`` and ``jit.GE``, and
    ``value`` the number it compares with."""

    op: int
    value: float


# The ``cond.op`` values, as ``jit.EQ`` and the others.
EQ = 0
LT = 2
LE = 3
GT = 4
GE = 5
_OP_C = {
    "EQ": "DFTU_INDEX_EQ",
    "LT": "DFTU_INDEX_LT",
    "LE": "DFTU_INDEX_LE",
    "GT": "DFTU_INDEX_GT",
    "GE": "DFTU_INDEX_GE",
}
_NAME = re.compile(r"^[a-z0-9_-]+\.[a-z0-9_.-]+$")
_PATH = re.compile(r"^[A-Za-z0-9_.-]+$")
_BINOPS: dict[type, str] = {ast.Add: "+", ast.Sub: "-", ast.Mult: "*", ast.Div: "/"}
_CMPOPS: dict[type, str] = {
    ast.Lt: "<",
    ast.LtE: "<=",
    ast.Gt: ">",
    ast.GtE: ">=",
    ast.Eq: "==",
    ast.NotEq: "!=",
}
_ROLES = {"add": ("self", "value"), "merge": ("self", "other"), "may_contain": ("self", "cond")}


class _Lower:
    """One method as a C function body over ``double`` state."""

    def __init__(
        self, src: Source, fields: dict[str, int], role: str, roles: tuple[str, str]
    ) -> None:
        self.src = src
        self.fields = fields
        self.role = role
        self.self_name, self.arg_name = roles
        self.locals: list[str] = []
        self.is_cond = role == "may_contain"

    def fail(self, reason: str, node: ast.AST) -> JitError:
        return self.src.error(reason, node)

    def expr(self, node: ast.expr) -> str:
        if isinstance(node, ast.Constant):
            v = node.value
            if isinstance(v, bool):
                return "1.0" if v else "0.0"
            if (
                isinstance(v, (int, float))
                and float(v) == float(v)
                and abs(float(v)) != float("inf")
            ):
                return repr(float(v))
            raise self.fail("a summary body takes only finite numbers and booleans", node)
        if isinstance(node, ast.Name):
            if node.id == self.arg_name:
                if self.role == "add":
                    return "value"
                raise self.fail(f"'{node.id}' is read by attribute only", node)
            if node.id in self.locals:
                return f"l_{node.id}"
            raise self.fail(f"'{node.id}' is not a local, a parameter or a state field", node)
        if isinstance(node, ast.Attribute):
            return self.attribute(node)
        if isinstance(node, ast.BinOp) and type(node.op) in _BINOPS:
            return f"({self.expr(node.left)} {_BINOPS[type(node.op)]} {self.expr(node.right)})"
        if isinstance(node, ast.UnaryOp):
            if isinstance(node.op, ast.USub):
                return f"(-{self.expr(node.operand)})"
            if isinstance(node.op, ast.Not):
                return f"(!{self.expr(node.operand)})"
        if isinstance(node, ast.BoolOp):
            joiner = " && " if isinstance(node.op, ast.And) else " || "
            return "(" + joiner.join(self.expr(v) for v in node.values) + ")"
        if isinstance(node, ast.Compare):
            parts = []
            left = node.left
            for op, right in zip(node.ops, node.comparators):
                if type(op) not in _CMPOPS:
                    raise self.fail("a summary body compares with ==, !=, <, <=, > and >=", node)
                parts.append(f"({self.expr(left)} {_CMPOPS[type(op)]} {self.expr(right)})")
                left = right
            return "(" + " && ".join(parts) + ")"
        if isinstance(node, ast.IfExp):
            return f"({self.expr(node.test)} ? {self.expr(node.body)} : {self.expr(node.orelse)})"
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id in ("min", "max", "abs")
            and not node.keywords
        ):
            args = [self.expr(a) for a in node.args]
            if node.func.id == "abs" and len(args) == 1:
                return f"fabs({args[0]})"
            if node.func.id in ("min", "max") and len(args) == 2:
                return f"dftu_s_{node.func.id}({args[0]}, {args[1]})"
        raise self.fail("this expression is outside the summary subset", node)

    def attribute(self, node: ast.Attribute) -> str:
        base = node.value
        if isinstance(base, ast.Name):
            if base.id == self.self_name:
                if node.attr not in self.fields:
                    raise self.fail(f"'{node.attr}' is not a state field", node)
                return f"self->f[{self.fields[node.attr]}]"
            if base.id == self.arg_name:
                if self.is_cond:
                    if node.attr == "op":
                        return "((double)cond_op)"
                    if node.attr == "value":
                        return "cond_value"
                    raise self.fail("cond has `op` and `value`", node)
                if self.arg_name == "other" and node.attr in self.fields:
                    return f"other->f[{self.fields[node.attr]}]"
                if self.arg_name == "other":
                    raise self.fail(f"'{node.attr}' is not a state field", node)
            if node.attr in _OP_C:
                return f"((double){_OP_C[node.attr]})"
        raise self.fail("this expression is outside the summary subset", node)

    def target(self, node: ast.expr) -> str:
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
            if node.value.id == self.self_name and node.attr in self.fields:
                return f"self->f[{self.fields[node.attr]}]"
            raise self.fail("a summary body assigns only the state fields of self", node)
        if isinstance(node, ast.Name):
            if node.id in (self.self_name, self.arg_name):
                raise self.fail(f"'{node.id}' is a parameter and cannot be assigned", node)
            if node.id not in self.locals:
                self.locals.append(node.id)
            return f"l_{node.id}"
        raise self.fail("a summary body assigns a state field of self or a local", node)

    def body(self, stmts: Sequence[ast.stmt], indent: int) -> list[str]:
        pad = "    " * indent
        out: list[str] = []
        for s in stmts:
            if isinstance(s, ast.Pass) or (
                isinstance(s, ast.Expr) and isinstance(s.value, ast.Constant)
            ):
                continue
            if isinstance(s, ast.Assign) and len(s.targets) == 1:
                out.append(f"{pad}{self.target(s.targets[0])} = {self.expr(s.value)};")
            elif isinstance(s, ast.AugAssign) and type(s.op) in _BINOPS:
                t = self.target(s.target)
                out.append(f"{pad}{t} = {t} {_BINOPS[type(s.op)]} {self.expr(s.value)};")
            elif isinstance(s, ast.If):
                out.append(f"{pad}if ({self.expr(s.test)}) {{")
                out += self.body(s.body, indent + 1)
                if s.orelse:
                    out.append(f"{pad}}} else {{")
                    out += self.body(s.orelse, indent + 1)
                out.append(f"{pad}}}")
            elif isinstance(s, ast.Return) and self.is_cond and s.value is not None:
                out.append(f"{pad}return {self.expr(s.value)} != 0.0;")
            else:
                raise self.fail("this statement is outside the summary subset", s)
        return out


def _spellings(path: str) -> list[str]:
    """A filter names an args key bare (`size`) or with its prefix (`args.size`)."""
    bare = path[5:] if path.startswith("args.") else path
    return [bare, "args." + bare]


def _method(cls: type, name: str, fields: dict[str, int]) -> tuple[list[str], list[str]]:
    fn = cls.__dict__.get(name)
    if fn is None or not callable(fn):
        raise class_error(cls, f"@jit.summary needs a method `{name}(self, {_ROLES[name][1]})`")
    src = Source(fn)
    node = ast.parse(src.text).body[0]
    if not isinstance(node, ast.FunctionDef):
        raise src.error("expected a method", node)
    params = [a.arg for a in node.args.args]
    if len(params) != 2 or node.args.vararg or node.args.kwonlyargs or node.args.kwarg:
        raise src.error(f"`{name}` takes (self, {_ROLES[name][1]})", node)
    low = _Lower(src, fields, name, (params[0], params[1]))
    body = low.body(node.body, 1)
    return low.locals, body


def _fields(cls: type) -> tuple[dict[str, int], list[float]]:
    notes = own_annotations(cls)
    fields: dict[str, int] = {}
    init: list[float] = []
    for attr, note in notes.items():
        text = note if isinstance(note, str) else getattr(note, "__name__", "")
        if text not in ("int", "float"):
            raise class_error(
                cls, f"the state field '{attr}' is annotated `int` or `float`, not {text!r}", attr
            )
        default = cls.__dict__.get(attr)
        if isinstance(default, bool) or not isinstance(default, (int, float)):
            raise class_error(
                cls,
                f"the state field '{attr}' needs an int or float default, its empty state",
                attr,
            )
        fields[attr] = len(fields)
        init.append(float(default))
    if not fields:
        raise class_error(cls, "@jit.summary needs at least one state field")
    return fields, init


def _c_function(
    name: str, params: str, ret: str, decls: list[str], body: list[str], tail: str
) -> list[str]:
    return [
        f"static {ret} {name}({params}) {{",
        *[f"    double l_{d} = 0.0;" for d in decls],
        *[f"    (void)l_{d};" for d in decls],
        *body,
        f"    {tail}" if tail else "",
        "}",
        "",
    ]


def _emit(
    plugin: str,
    ext: str,
    version: int,
    paths: Sequence[str],
    init: list[float],
    add: tuple[list[str], list[str]],
    merge: tuple[list[str], list[str]],
    may: tuple[list[str], list[str]],
) -> str:
    nf = len(init)
    lines = [
        "#include <dftracer/utils/dataframe/c_api.h>",
        "#include <dftracer/utils/plugins/abi.h>",
        "",
        "#include <math.h>",
        "#include <stdint.h>",
        "#include <stdlib.h>",
        "#include <string.h>",
        "",
        f"#define NF {nf}",
        "#define TWO53 INT64_C(9007199254740992)",
        "typedef struct { double f[NF]; } state;",
        "static double dftu_s_min(double a, double b) { return a < b ? a : b; }",
        "static double dftu_s_max(double a, double b) { return a > b ? a : b; }",
        f"static const double INIT[NF] = {{{', '.join(repr(v) for v in init)}}};",
        "static void state_init(state* s) { memcpy(s->f, INIT, sizeof(INIT)); }",
        "",
        *_c_function(
            "add_value",
            "state* self, double value",
            "void",
            add[0],
            add[1],
            "(void)self; (void)value;",
        ),
        *_c_function(
            "merge_state",
            "state* self, const state* other",
            "void",
            merge[0],
            merge[1],
            "(void)self; (void)other;",
        ),
        *_c_function(
            "may_contain",
            "const state* self, int32_t cond_op, double cond_value",
            "int",
            may[0],
            may[1],
            "(void)self; (void)cond_op; (void)cond_value; return 1;",
        ),
        "typedef struct {",
        "    state chunk;",
        "    state file;",
        "    int skip_chunk;",
        "    int skip_file;",
        "} builder;",
        "",
        "typedef struct {",
        "    int32_t op;",
        "    double value;",
        "} compiled;",
        "",
        "static const char* const COLUMNS[] = {" + ", ".join(f'"{p}"' for p in paths) + ", NULL};",
        "",
        "static dftu_status make_builder(dftu_ctx* ctx, void* self, void** out) {",
        "    builder* b;",
        "    (void)self;",
        "    b = (builder*)calloc(1, sizeof(builder));",
        '    if (!b) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
        "    state_init(&b->chunk);",
        "    state_init(&b->file);",
        "    *out = b;",
        "    return DFTU_OK;",
        "}",
        "",
        "static dftu_status step_column(dftu_ctx* ctx, void* h, const char* name,",
        "                               uint32_t name_len, const dftu_series* col) {",
        "    builder* b = (builder*)h;",
        "    int64_t n = dftu_series_length(col);",
        "    int32_t type = dftu_series_type(col);",
        "    const void* data = dftu_series_data(col);",
        "    (void)ctx;",
        "    (void)name;",
        "    (void)name_len;",
        "    if (type != DFTU_TYPE_INT64 && type != DFTU_TYPE_UINT64 &&",
        "        type != DFTU_TYPE_FLOAT64) {",
        "        b->skip_chunk = 1;",
        "        return DFTU_OK;",
        "    }",
        "    for (int64_t i = 0; i < n; ++i) {",
        "        double v;",
        "        if (dftu_series_is_null(col, i)) continue;",
        "        if (type == DFTU_TYPE_INT64) {",
        "            int64_t x = ((const int64_t*)data)[i];",
        "            if (x > TWO53 || x < -TWO53) { b->skip_chunk = 1; continue; }",
        "            v = (double)x;",
        "        } else if (type == DFTU_TYPE_UINT64) {",
        "            uint64_t x = ((const uint64_t*)data)[i];",
        "            if (x > (uint64_t)TWO53) { b->skip_chunk = 1; continue; }",
        "            v = (double)x;",
        "        } else {",
        "            v = ((const double*)data)[i];",
        "        }",
        "        if (v != v) continue;",
        "        add_value(&b->chunk, v);",
        "    }",
        "    return DFTU_OK;",
        "}",
        "",
        "static void put_state(const state* s, const dftu_index_out* out) {",
        "    uint32_t head = (uint32_t)NF;",
        "    out->append(out->self, &head, sizeof(head));",
        "    out->append(out->self, s->f, sizeof(s->f));",
        "}",
        "",
        "static dftu_status finish_granule(dftu_ctx* ctx, void* h, uint64_t granule,",
        "                                  const dftu_index_out* out) {",
        "    builder* b = (builder*)h;",
        "    (void)ctx;",
        "    (void)granule;",
        "    if (b->skip_chunk) {",
        "        b->skip_file = 1;",
        "    } else {",
        "        put_state(&b->chunk, out);",
        "        merge_state(&b->file, &b->chunk);",
        "    }",
        "    b->skip_chunk = 0;",
        "    state_init(&b->chunk);",
        "    return DFTU_OK;",
        "}",
        "",
        "static dftu_status finish_file(dftu_ctx* ctx, void* h, const dftu_index_out* out) {",
        "    builder* b = (builder*)h;",
        "    (void)ctx;",
        "    if (!b->skip_file) put_state(&b->file, out);",
        "    return DFTU_OK;",
        "}",
        "",
        "static void destroy_builder(void* h) { free(h); }",
        "",
        "static dftu_status compile_leaf(dftu_ctx* ctx, void* self, const dftu_index_leaf* leaf,",
        "                                void** out) {",
        "    compiled* c;",
        "    int known = 0;",
        "    (void)self;",
        "    *out = NULL;",
        *[
            f'    if (strcmp(leaf->path, "{q}") == 0) known = 1;'
            for p in paths
            for q in _spellings(p)
        ],
        "    if (leaf->any || !known) return DFTU_OK;",
        "    if (leaf->op != DFTU_INDEX_EQ && leaf->op != DFTU_INDEX_LT &&",
        "        leaf->op != DFTU_INDEX_LE && leaf->op != DFTU_INDEX_GT &&",
        "        leaf->op != DFTU_INDEX_GE)",
        "        return DFTU_OK;",
        "    if (leaf->type != 0 && leaf->type != DFTU_TYPE_INT64 &&",
        "        leaf->type != DFTU_TYPE_UINT64 && leaf->type != DFTU_TYPE_FLOAT64)",
        "        return DFTU_OK;",
        "    if (leaf->value->kind != DFTU_VAL_I64 && leaf->value->kind != DFTU_VAL_F64)",
        "        return DFTU_OK;",
        "    if (leaf->value->kind == DFTU_VAL_I64 &&",
        "        (leaf->value->as.i64 > TWO53 || leaf->value->as.i64 < -TWO53))",
        "        return DFTU_OK;",
        "    c = (compiled*)malloc(sizeof(compiled));",
        '    if (!c) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
        "    c->op = leaf->op;",
        "    c->value = dftu_as_f64(leaf->value, 0);",
        "    *out = c;",
        "    return DFTU_OK;",
        "}",
        "",
        "static int judge(void* h, const uint8_t* p, uint64_t n) {",
        "    const compiled* c = (const compiled*)h;",
        "    state s;",
        "    uint32_t head;",
        "    if (n != sizeof(head) + sizeof(s.f)) return 1;",
        "    memcpy(&head, p, sizeof(head));",
        "    if (head != (uint32_t)NF) return 1;",
        "    memcpy(s.f, p + sizeof(head), sizeof(s.f));",
        "    return may_contain(&s, c->op, c->value);",
        "}",
        "",
        "static void release(void* h) { free(h); }",
        "",
        "static dftu_index_extension g_ext;",
        "",
        "static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {",
        "    (void)self;",
        "    *out = calloc(1, 1);",
        '    if (!*out) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
        "    return DFTU_OK;",
        "}",
        "static dftu_status on_batch(dftu_ctx* ctx, void* slice, const dftu_dataframe* df,",
        "                            const dftu_plugin_host* host) {",
        "    (void)ctx; (void)slice; (void)df; (void)host;",
        "    return DFTU_OK;",
        "}",
        "static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {",
        "    (void)ctx; (void)into; (void)other;",
        "    return DFTU_OK;",
        "}",
        "static dftu_status on_finalize(dftu_ctx* ctx, void* slice, const dftu_plugin_host* host,",
        "                               dftu_task** pending) {",
        "    (void)ctx; (void)slice; (void)host; (void)pending;",
        "    return DFTU_OK;",
        "}",
        "static void destroy_slice(void* slice) { free(slice); }",
        "static void destroy(void* self) { (void)self; }",
        "",
        "static dftu_plugin g_plugin;",
        "",
        f'DFTU_PLUGIN_INFO("{plugin}", "1.0")',
        "",
        "DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h,",
        "                                               const dftu_value* config,",
        "                                               dftu_plugin** out) {",
        "    const dftu_svc_index* index =",
        "        (const dftu_svc_index*)h->get_service(h->h, DFTU_SVC_INDEX);",
        "    dftu_status st;",
        "    (void)config;",
        '    if (!index) return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no index service at load");',
        "    memset(&g_ext, 0, sizeof(g_ext));",
        "    g_ext.struct_size = sizeof(g_ext);",
        f"    g_ext.version = {version}u;",
        "    g_ext.params_hash = PARAMS_HASH;",
        "    g_ext.make_builder = make_builder;",
        "    g_ext.finish_granule = finish_granule;",
        "    g_ext.finish_file = finish_file;",
        "    g_ext.destroy_builder = destroy_builder;",
        "    g_ext.compile = compile_leaf;",
        "    g_ext.may_match = judge;",
        "    g_ext.file_may_match = judge;",
        "    g_ext.release = release;",
        "    g_ext.columns = COLUMNS;",
        "    g_ext.step_column = step_column;",
        f'    st = index->register_extension(ctx, h->h, "{ext}", &g_ext, NULL);',
        "    if (st < 0) return st;",
        "    memset(&g_plugin, 0, sizeof(g_plugin));",
        "    g_plugin.struct_size = sizeof(g_plugin);",
        "    g_plugin.make_slice = make_slice;",
        "    g_plugin.on_batch = on_batch;",
        "    g_plugin.merge = merge;",
        "    g_plugin.on_finalize = on_finalize;",
        "    g_plugin.destroy_slice = destroy_slice;",
        "    g_plugin.destroy = destroy;",
        "    *out = &g_plugin;",
        "    return DFTU_OK;",
        "}",
        "",
    ]
    text = "\n".join(lines)
    digest = int.from_bytes(hashlib.sha256(text.encode()).digest()[:8], "little")
    return text.replace("PARAMS_HASH", f"UINT64_C({digest})")


def build_summary(cls: type, name: str, paths: Sequence[str], version: int) -> type:
    if not _NAME.match(name) or name.startswith("dftu."):
        raise class_error(
            cls,
            f"@jit.summary name '{name}' must be <plugin>.<name> in lowercase ASCII "
            "[a-z0-9._-] and outside the reserved dftu namespace",
        )
    if not paths or builtins.any(not _PATH.match(p) for p in paths):
        raise class_error(
            cls, "@jit.summary paths names the dotted paths it summarizes, such as 'args.size'"
        )
    if not 0 < version < 2**32:
        raise class_error(cls, "@jit.summary version is an int in [1, 2**32)")
    fields, init = _fields(cls)
    add = _method(cls, "add", fields)
    merge = _method(cls, "merge", fields)
    may = _method(cls, "may_contain", fields)
    plugin = name.split(".", 1)[0]
    source = _emit(plugin, name, version, list(paths), init, add, merge, may)
    spec = JitPlugin(
        name.replace(".", "_"), source, {}, {}, DEFAULT_PACKAGE.shippable(cls), "summary"
    )
    setattr(cls, "_jit_plugin", spec)
    return cls


def summary(*, name: str, paths: Sequence[str], version: int = 1) -> Callable[[C], C]:
    """Class decorator: compile a Python summary to a native index extension.

    Args:
        name: The extension's registered name, ``<plugin>.<name>``, which
            ``Indexer(summaries={"size": [name]})`` names.
        paths: The dotted paths whose columns it takes, as filters name them
            under ``args``: ``"args.size"``.
        version: Data built with another version is not used; the build also
            hashes the compiled body, so a changed body rebuilds the data.

    Raises:
        JitError: A construct outside the summary subset, with its line and
            column.
    """

    def wrap(target: C) -> C:
        return cast("C", build_summary(target, name, paths, version))

    return wrap
