# SPDX-License-Identifier: MIT
"""The AST compiler of a jit body to C."""

from __future__ import annotations

import ast
import builtins
from typing import (
    TYPE_CHECKING,
    Any,
    Callable,
    Iterator,
    List,
    NamedTuple,
    NoReturn,
    Optional,
    cast,
)

from .. import _jit_op
from .._c_text import c_str_literal as _c_str_literal
from .._eval import COMPARE_SYMBOL, ENGINE_ARITH_SYMBOL
from .._jit_errors import JitError, Source
from .._jit_op import Op
from . import _channels
from ._channels import PortDecl as _PortDecl
from ._flow import FlowFn as _FlowFn
from ._flow import TaskSig as _TaskSig
from ._maps import AnyMap, CfgField, Map, PlainField, PortHandle, StoreField
from ._monoids import Counter, ListV, Max, Min, Monoid, SetV, Sum
from ._types import KIND_DTYPES, int_literal, is_float_dft, key_type, kind_type

if TYPE_CHECKING:
    pass


# Python 3.8 wraps a subscript's slice in ast.Index; 3.9+ stores the value
# directly, and 3.14 may drop the class entirely, so resolve it defensively.
_AST_INDEX = getattr(ast, "Index", None)


def unwrap_index(node: ast.AST) -> ast.expr:
    # _AST_INDEX is resolved at runtime, so the type checker cannot narrow node
    # to ast.Index here; read .value dynamically.
    if _AST_INDEX is not None and isinstance(node, _AST_INDEX):
        node = getattr(node, "value", node)
    return cast(ast.expr, node)


# per-field default (0 / DFTU_STR_NONE), matching dftu_jit_resolve/accessors.
_FIELD_EXPR: dict[str, str] = {
    "pid": "dftu_jit_u64(&_col_pid, i)",
    "tid": "dftu_jit_u64(&_col_tid, i)",
    "ts": "dftu_jit_u64(&_col_ts, i)",
    "dur": "dftu_jit_u64(&_col_dur, i)",
    "cat": "dftu_jit_strid(host, &_col_cat, i)",
    "name": "dftu_jit_strid(host, &_col_name, i)",
    "fhash": "dftu_jit_strid(host, &_col_fhash, i)",
    "hhash": "dftu_jit_strid(host, &_col_hhash, i)",
    "phase": "dftu_jit_i64(&_col_ph, i)",
    # The row-fold frame has no per-row null tracking for dur; approximate
    # has_dur as phase == Complete, matching plugins::Event::has_dur().
    "has_dur": "(dftu_jit_i64(&_col_ph, i) == (int64_t)DFTU_PH_COMPLETE)",
}

# event field -> the batch column name it reads (the "_col_<name>" resolved in
# on_batch).
_FIELD_COLUMN: dict[str, str] = {
    "pid": "pid",
    "tid": "tid",
    "ts": "ts",
    "dur": "dur",
    "cat": "cat",
    "name": "name",
    "fhash": "fhash",
    "hhash": "hhash",
    "phase": "ph",
    "has_dur": "ph",
}

# Every fixed scan column a raw each_event body might reference; raw text is
# not AST-analyzable, so a raw plugin resolves all of them unconditionally.
ALL_FIXED_FIELDS = frozenset(_FIELD_COLUMN.values())

_ARG_METHODS: dict[str, tuple[str, str]] = {
    "arg_i64": ("dftu_jit_arg_i64", "0"),
    "arg_f64": ("dftu_jit_arg_f64", "0.0"),
    "arg_str": ("dftu_jit_arg_str", "DFTU_STR_NONE"),
}

ARG_HELPER_DEFS: dict[str, list[str]] = {
    "arg_i64": [
        "static int64_t dftu_jit_arg_i64(const dftu_jit_col* c, int64_t i) {",
        "    if (c->type != DFTU_TYPE_INT64 || !c->data || dftu_series_is_null(c->handle, i))",
        "        return 0;",
        "    return ((const int64_t*)c->data)[i];",
        "}",
        "",
    ],
    "arg_f64": [
        "static double dftu_jit_arg_f64(const dftu_jit_col* c, int64_t i) {",
        "    if (c->type != DFTU_TYPE_FLOAT64 || !c->data || dftu_series_is_null(c->handle, i))",
        "        return 0.0;",
        "    return ((const double*)c->data)[i];",
        "}",
        "",
    ],
    "arg_str": [
        "static dftu_str dftu_jit_arg_str(const dftu_plugin_host* host, const dftu_jit_col* c,",
        "                                 int64_t i) {",
        "    if (c->type != DFTU_TYPE_STRING) return DFTU_STR_NONE;",
        "    return dftu_jit_strid(host, c, i);",
        "}",
        "",
    ],
}

# A resolved batch column (see plugins::detail::ColBuf, the C++ SDK's mirror
# of this): NULL/-1 fields when the named column is absent from the batch, so
# every accessor below reads that as the field's old default rather than
# re-resolving it.
COLBUF_HELPERS: list[str] = [
    "typedef struct {",
    "    dftu_series* handle;",
    "    int32_t type;",
    "    const void* data;",
    "    const int32_t* offsets;",
    "} dftu_jit_col;",
    "",
    "static dftu_jit_col dftu_jit_resolve(const dftu_dataframe* df, const char* name) {",
    "    dftu_jit_col c;",
    "    c.handle = dftu_dataframe_column(df, name);",
    "    c.type = c.handle ? dftu_series_type(c.handle) : -1;",
    "    c.data = c.handle ? dftu_series_data(c.handle) : NULL;",
    "    c.offsets = c.handle ? dftu_series_offsets(c.handle) : NULL;",
    "    return c;",
    "}",
    "",
    "static uint64_t dftu_jit_u64(const dftu_jit_col* c, int64_t i) {",
    "    if (!c->data || dftu_series_is_null(c->handle, i)) return 0;",
    "    return ((const uint64_t*)c->data)[i];",
    "}",
    "",
    "static int64_t dftu_jit_i64(const dftu_jit_col* c, int64_t i) {",
    "    if (!c->data || dftu_series_is_null(c->handle, i)) return 0;",
    "    return ((const int64_t*)c->data)[i];",
    "}",
    "",
    "static dftu_str dftu_jit_strid(const dftu_plugin_host* host, const dftu_jit_col* c, int64_t i) {",
    "    if (!c->data || !c->offsets) {",
    "        int64_t n = 0;",
    "        const char* p = c->handle ? dftu_series_string_at(c->handle, i, &n) : NULL;",
    "        return p ? host->intern(host->h, p, (uint32_t)n) : DFTU_STR_NONE;",
    "    }",
    "    if (dftu_series_is_null(c->handle, i)) return DFTU_STR_NONE;",
    "    const char* base = (const char*)c->data;",
    "    int32_t o0 = c->offsets[i], o1 = c->offsets[i + 1];",
    "    return host->intern(host->h, base + o0, (uint32_t)(o1 - o0));",
    "}",
    "",
]

# A raw each_event body is spliced verbatim and cannot be re-lowered against
# _col_*, so it gets a per-row shim struct populated from every fixed column
# up front, preserving the old e->field spelling.
RAW_EVENT_SHIM_TYPE: list[str] = [
    "typedef struct {",
    "    uint64_t pid, tid, ts, dur;",
    "    int64_t phase;",
    "    int has_dur;",
    "    dftu_str cat, name, fhash, hhash;",
    "} dftu_jit_event;",
    "",
]

RAW_EVENT_SHIM_POPULATE: list[str] = [
    "dftu_jit_event _ev;",
    "_ev.pid = dftu_jit_u64(&_col_pid, i);",
    "_ev.tid = dftu_jit_u64(&_col_tid, i);",
    "_ev.ts = dftu_jit_u64(&_col_ts, i);",
    "_ev.dur = dftu_jit_u64(&_col_dur, i);",
    "_ev.phase = dftu_jit_i64(&_col_ph, i);",
    "_ev.has_dur = (_ev.phase == (int64_t)DFTU_PH_COMPLETE);",
    "_ev.cat = dftu_jit_strid(host, &_col_cat, i);",
    "_ev.name = dftu_jit_strid(host, &_col_name, i);",
    "_ev.fhash = dftu_jit_strid(host, &_col_fhash, i);",
    "_ev.hhash = dftu_jit_strid(host, &_col_hhash, i);",
    "const dftu_jit_event* e = &_ev;",
]

_STR_FIELDS = frozenset({"cat", "name", "fhash", "hhash"})

# Reductions contributed with `+=` (everything else takes .observe / .append).
_ADDITIVE_OPS = frozenset({"DFTU_AGG_SUM"})

_ARGBY_OPS = frozenset({"DFTU_AGG_ARGMIN", "DFTU_AGG_ARGMAX"})

_TOPK_OPS = frozenset({"DFTU_AGG_TOPK", "DFTU_AGG_BOTTOMK"})

_SET_OPS = frozenset({"DFTU_AGG_SET_UNION"})

_LIST_OPS = frozenset({"DFTU_AGG_LIST_SORTED"})

# Numeric primitives usable in a key or value expression; each lowers to the
# matching static-inline helper in prims.h so a body reads jit.ilog2(e.dur)
# instead of a raw compiler builtin. Value is (c_function, arity, arg_cast); all
# return an integer.
_PRIM_C: dict[str, tuple[str, int, str]] = {
    # bit / log-scale bucketing (unsigned)
    "ilog2": ("dftu_ilog2_u64", 1, "uint64_t"),
    "clz": ("dftu_clz_u64", 1, "uint64_t"),
    "ctz": ("dftu_ctz_u64", 1, "uint64_t"),
    "popcount": ("dftu_popcount_u64", 1, "uint64_t"),
    "bit_width": ("dftu_bit_width_u64", 1, "uint64_t"),
    "ceil_pow2": ("dftu_ceil_pow2_u64", 1, "uint64_t"),
    "floor_pow2": ("dftu_floor_pow2_u64", 1, "uint64_t"),
    "rotl": ("dftu_rotl_u64", 2, "uint64_t"),
    "rotr": ("dftu_rotr_u64", 2, "uint64_t"),
    # integer math
    "abs": ("dftu_abs_i64", 1, "int64_t"),
    "clamp": ("dftu_clamp_i64", 3, "int64_t"),
    "div_ceil": ("dftu_div_ceil_i64", 2, "int64_t"),
    "div_round": ("dftu_div_round_i64", 2, "int64_t"),
    "align_up": ("dftu_align_up_u64", 2, "uint64_t"),
    "align_down": ("dftu_align_down_u64", 2, "uint64_t"),
    "isqrt": ("dftu_isqrt_u64", 1, "uint64_t"),
    "gcd": ("dftu_gcd_u64", 2, "uint64_t"),
    # fast integer hashing
    "mul_hi": ("dftu_mul_hi_u64", 2, "uint64_t"),
    "mix64": ("dftu_mix64", 1, "uint64_t"),
    "fastrange": ("dftu_fastrange_u64", 2, "uint64_t"),
    # floating point (double)
    "fmin": ("dftu_min_f64", 2, "double"),
    "fmax": ("dftu_max_f64", 2, "double"),
    "clampf": ("dftu_clamp_f64", 3, "double"),
    "sqrt": ("dftu_sqrt_f64", 1, "double"),
    "log2": ("dftu_log2_f64", 1, "double"),
    "log": ("dftu_log_f64", 1, "double"),
    "exp": ("dftu_exp_f64", 1, "double"),
    "fma": ("dftu_fma_f64", 3, "double"),
    "lerp": ("dftu_lerp_f64", 3, "double"),
    "copysign": ("dftu_copysign_f64", 2, "double"),
}

_CONSTRUCT = {
    ast.For: "for loop",
    ast.While: "while loop",
    ast.With: "with statement",
    ast.Call: "function call",
    ast.BinOp: "arithmetic expression",
    ast.BoolOp: "boolean expression",
    ast.Assign: "local assignment",
    ast.Return: "return statement",
    ast.IfExp: "conditional expression",
    ast.NamedExpr: "assignment expression (:=)",
}


def _describe(node: ast.AST) -> str:
    return _CONSTRUCT.get(type(node), type(node).__name__)


def _reject_at(reason: str, node: ast.AST | None = None) -> NoReturn:
    """Fail the compile at ``node``; the compiler turns it into file:line:col."""
    err = JitError(reason, locate=False)
    err.node = node
    raise err


def _reject(what: str, node: ast.AST | None = None) -> NoReturn:
    _reject_at(f"unsupported in @jit.each_event: {what}; use a raw C++ plugin", node)


def out_name(decl: AnyMap, comp: int) -> str:
    """Result column name for value component ``comp``."""
    if not decl.is_product:
        return "value"
    if decl.value_names is not None:
        return decl.value_names[comp]
    return f"v{comp}"


class _Update(NamedTuple):
    """One merge-inferable update to a plain-container field: its ``form``
    (``sum``, ``max``, ``min``, ``add`` or ``append``), the key and value
    expressions, and whether a missing key contributes the type's zero first."""

    attr: str
    form: str
    keys: List[ast.expr]
    value: ast.expr
    identity: bool


_STATE_FORMS = {
    "dict": (
        "d[k] += x on a defaultdict, d[k] = d.get(k, 0) + x, d[k] = max(d[k], x) on a "
        "defaultdict, d[k] = max(d.get(k, x), x), or the same with min"
    ),
    "set": "s.add(x)",
    "list": "lst.append(x)",
    "scalar": "n += x",
}
_STATE_PY = {"i64": "int", "f64": "float"}


def _child_blocks(stmt: ast.stmt) -> list[list[ast.stmt]] | None:
    """The statement lists a compound statement runs, None for a simple one."""
    if isinstance(stmt, ast.If):
        return [stmt.body, stmt.orelse]
    if isinstance(stmt, ast.Try):
        return [stmt.body] + [h.body for h in stmt.handlers]
    if isinstance(stmt, (ast.With, ast.For)):
        return [stmt.body]
    return None


class _Local:
    """A local of a body: ``kind`` is ``i64``, ``f64``, ``str`` (an interned id),
    ``view`` (bytes that the host lends: a file line, a chunk or a JSON string) or
    ``val`` (a JSON / YAML value); ``cname`` the C lvalue. ``origins`` names what a
    ``view`` or ``val`` points into, and ``depth`` is the loop depth that made it."""

    __slots__ = ("kind", "cname", "origins", "depth")

    def __init__(self, kind: str, cname: str, depth: int = 0) -> None:
        self.kind = kind
        self.cname = cname
        self.origins: list[tuple[Any, ...]] = []
        self.depth = depth


def _unconditional_nodes(root: ast.AST) -> Iterator[ast.AST]:
    """The nodes of ``root`` that always run. An ``and`` / ``or`` operand after
    the first, and a link of a chained comparison after the first, only run when
    the earlier ones allow it, so hoisting one above its guard would run it for
    a row the guard excludes."""
    stack: list[ast.AST] = [root]
    while stack:
        node = stack.pop()
        yield node
        if isinstance(node, ast.BoolOp):
            stack.append(node.values[0])
        elif isinstance(node, ast.Compare) and len(node.ops) > 1:
            stack.extend((node.left, node.comparators[0]))
        else:
            stack.extend(ast.iter_child_nodes(node))


_KIND_WORD = {
    "i64": "an int",
    "f64": "a float",
    "str": "a string",
    "view": "a string view",
    "val": "a JSON value",
}
_CTYPE = {
    **{kind: kind_type(kind).ctype for kind in KIND_DTYPES},
    "view": "DftuView",
    "val": "const dftu_value*",
}
_CINIT = {
    "i64": "0",
    "f64": "0.0",
    "str": "DFTU_STR_NONE",
    "view": "{NULL, 0, DFTU_STR_NONE}",
    "val": "NULL",
}
_VAL_METHODS = frozenset({"find", "as_int", "as_float", "as_str", "as_bool", "is_null"})
_RAISABLE = {"Fatal": "DFTU_E_UNKNOWN", "SkipRest": "DFTU_OK", "Cancelled": "DFTU_E_CANCELLED"}
_UNCATCHABLE = {
    "Fatal": "it fails the run; raise it from the handler of a jit.Error instead",
    "SkipRest": "it is not an error",
    "Cancelled": "a cancelled body must stop",
}
_CATCH_COND = {
    "Error": "dftu_jit_is_error(_st)",
    "NotFound": "_st == DFTU_E_NOT_FOUND",
    "Closed": "_st == DFTU_E_NOT_FOUND && _pclosed",
    "IoError": "_st == DFTU_E_IO",
    "ParseError": "_st == DFTU_E_PARSE",
}
# What `except jit.Error` catches: a failed call. A jit.Fatal, a cancel, an
# internal error and an allocation failure always end the run.
ERROR_HELPER = [
    "static int dftu_jit_is_error(dftu_status s) {",
    "    return s < 0 && s != DFTU_E_UNKNOWN && s != DFTU_E_INTERNAL && s != DFTU_E_CANCELLED &&",
    "           s != DFTU_E_NO_MEMORY;",
    "}",
    "",
]


class Compiler:
    def __init__(
        self,
        maps: dict[str, AnyMap],
        ops: dict[str, Op] | None = None,
        ports: dict[str, PortHandle] | None = None,
        config_fields: dict[str, CfgField] | None = None,
        plain: dict[str, PlainField] | None = None,
        stores: dict[str, StoreField] | None = None,
        chans: dict[str, _PortDecl] | None = None,
    ) -> None:
        self.maps = maps
        self.plain = plain if plain is not None else {}
        self.stores = stores if stores is not None else {}
        # Channels, latest values and phase results by attribute, and the ones
        # this body uses (their handles are opened before the body runs).
        self.chans = chans if chans is not None else {}
        self.chan_uses: builtins.set[str] = builtins.set()
        self._port_frame = False
        # Per plain field: the (form, identity) its updates agree on.
        self.plain_forms: dict[str, tuple[str, bool]] = {}
        self.store_gets: builtins.set[str] = builtins.set()
        self.store_puts: builtins.set[str] = builtins.set()
        self.ports = ports if ports is not None else {}
        # The plugin's config dataclass fields; each_event reads them as
        # self.config.<name>, lowering to the file-scope _cfg_<name> static set
        # at load.
        self.config_fields = config_fields if config_fields is not None else {}
        # Referenced @jit.op transforms, inlined into the plugin as static C
        # functions the moment the body calls one.
        self.ops = ops if ops is not None else {}
        self.op_defs: list[str] = []
        self.op_emitted: builtins.set[str] = builtins.set()
        # Physical batch columns ("pid", "cat", "ph", ...) the body actually
        # reads; on_batch resolves exactly these once per batch.
        self.fields_used: builtins.set[str] = builtins.set()
        self.str_literals: list[str] = []
        self.arg_keys: list[str] = []
        self.arg_helpers: builtins.set[str] = builtins.set()
        self.self_name = "self"
        self.event_name = "e"
        # Contribution statements per accumulator; the emitted row buffers hold
        # this many rows per scanned event.
        self.rows_per_event: dict[str, int] = {}
        # CSE: ast.dump(node) -> (local name, is_float) for a hoisted expression
        # shared within the current scope.
        self._cse: dict[str, tuple[str, bool]] = {}
        self._cse_n = 0
        # The statement being compiled, where an error without a node points.
        self._cur: ast.AST | None = None
        # The C condition of a body that is one guard around its updates: the
        # emitted code filters the batch with it once, then runs the updates
        # on the surviving rows only. None when the body branches per event.
        self.mask_guard: str | None = None
        # raise / try: whether the body unwinds, the stack of labels a failing
        # statement jumps to (innermost last), and the label past the loop.
        self.can_raise = False
        self._targets: list[str] = []
        self._label_n = 0
        self._source: Source | None = None
        self._store_read = False
        # Locals, files and JSON values (see _flow): a local is a C local of the
        # per-event loop, or a field of the task frame in a flow body.
        self.flow = False
        self.is_async = False
        self.in_task = False
        self.locals: dict[str, _Local] = {}
        self.local_decls: list[str] = []
        self.res_decls: list[str] = []
        self.res_frees: list[str] = []
        self.files: dict[str, tuple[str, str]] = {}
        self.uses: builtins.set[str] = builtins.set()
        self.tasks: dict[str, _TaskSig] = {}
        self.emit_names: list[str] = []
        self.ob_size = 8
        self._res_n = 0
        self._await_n = 0
        self._job_n = 0
        self._cse_ok = True
        self._loops = 0
        self._has_loops = False
        self.doc_gen: dict[str, int] = {}
        self.file_gen: dict[str, int] = {}
        self._active_loops: list[int] = []
        self._loop_seq = 0
        self._iter_files: list[str] = []
        self.docs: dict[str, str] = {}
        self.file_path: dict[str, str] = {}
        self._rhs_node: ast.expr | None = None

    def lower(self, fn: Callable[..., object]) -> list[str]:
        source = Source(fn)
        self._source = source
        try:
            return self._lower(fn, source)
        except JitError as exc:
            raise source.relocate(exc, self._cur) from None

    def _lower(self, fn: Callable[..., object], source: Source) -> list[str]:
        mod = ast.parse(source.text)
        funcs = [n for n in mod.body if isinstance(n, ast.FunctionDef)]
        if len(funcs) != 1:
            _reject("each_event must decorate a single method", mod.body[0])
        fn_ast = funcs[0]
        self._cur = fn_ast
        params = fn_ast.args.args
        if len(params) != 2:
            _reject("each_event must take exactly (self, e)", fn_ast)
        self.self_name = params[0].arg
        self.event_name = params[1].arg
        if self.plain:
            self._infer_plain(fn_ast.body)
            self._declare_plain()
        self._has_loops = builtins.any(isinstance(n, ast.For) for n in ast.walk(fn_ast))
        self.can_raise = builtins.any(
            isinstance(n, (ast.Raise, ast.Try, ast.With))
            or (isinstance(n, ast.Call) and self._jit_name(n.func) in ("json", "yaml"))
            or (isinstance(n, ast.Call) and self._chan_call(n) is not None)
            for n in ast.walk(fn_ast)
        )
        body = fn_ast.body
        if (
            len(body) == 1
            and isinstance(body[0], ast.If)
            and not body[0].orelse
            and not self.stores
        ):
            self._cur = body[0]
            self.mask_guard = self._cond(body[0].test)
            body = body[0].body
        defs, lines = self._compile_scope(body)
        return self.local_decls + defs + lines

    # ---- flow bodies: @jit.finalize and @jit.task ----------------------------

    def lower_flow(self, fn: Callable[..., object], sig: _TaskSig | None) -> _FlowFn:
        source = Source(fn)
        self._source = source
        try:
            return self._lower_flow(fn, source, sig)
        except JitError as exc:
            raise source.relocate(exc, self._cur) from None

    def _lower_flow(
        self, fn: Callable[..., object], source: Source, sig: _TaskSig | None
    ) -> _FlowFn:
        mod = ast.parse(source.text)
        funcs = [n for n in mod.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
        if len(funcs) != 1:
            _reject_at("a flow body decorates a single method", mod.body[0])
        fn_ast = funcs[0]
        self._cur = fn_ast
        self.flow = True
        self.is_async = isinstance(fn_ast, ast.AsyncFunctionDef)
        self.in_task = sig is not None
        self.task_ret = sig.ret if sig is not None else None
        self._cse_ok = False
        self._exit_label = "_fx_exit"
        self.event_name = ""
        self.can_raise = True
        params = fn_ast.args.args
        want = 1 + (len(sig.params) if sig is not None else 0)
        if len(params) != want or fn_ast.args.vararg or fn_ast.args.kwonlyargs or fn_ast.args.kwarg:
            _reject_at(
                "a @jit.task takes (self, <typed parameters>)"
                if sig is not None
                else "a @jit.finalize takes (self)",
                fn_ast,
            )
        self.self_name = params[0].arg
        if sig is not None:
            for arg, (_pname, pkind) in zip(params[1:], sig.params):
                self.local_decls.append(f"{_CTYPE[pkind]} v_{arg.arg};")
                self.locals[arg.arg] = _Local(pkind, f"S->v_{arg.arg}")
        defs, lines = self._compile_scope(fn_ast.body)
        name = sig.name if sig is not None else fn_ast.name
        return _FlowFn(
            name=name,
            is_async=self.is_async,
            sig=sig,
            fields=self.local_decls + self.res_decls,
            body=defs + lines,
            ob_size=self.ob_size,
            uses=builtins.set(self.uses),
            res_frees=self.res_frees,
            emit_names=self.emit_names,
            store_gets=self.store_gets,
            op_defs=self.op_defs,
        )

    # ---- locals, files, JSON values, await (the flow constructs) -------------

    def _local(self, node: ast.Name) -> _Local:
        loc = self.locals.get(node.id)
        if loc is not None:
            self._check_origins(node.id, loc, node)
            return loc
        if node.id in self.files:
            _reject_at(
                f"{node.id} is a file: use `for line in {node.id}` or {node.id}.read(n)", node
            )
        _reject_at(
            f"unknown name {node.id}; a local exists once it is assigned, and the body reads "
            "only its locals, the event, self and jit",
            node,
        )

    def _check_origins(self, name: str, loc: _Local, node: ast.AST) -> None:
        for origin in loc.origins:
            if origin[0] == "doc":
                if self.doc_gen.get(origin[1]) != origin[2]:
                    _reject_at(
                        f"{name} points into {origin[1]}, which is parsed again before this use; "
                        f"read what you need from {name} before the new parse",
                        node,
                    )
            elif origin[0] == "loop":
                if origin[1] not in self._active_loops:
                    _reject_at(f"{name} is valid only inside its for loop", node)
            elif origin[0] == "file":
                if origin[1] not in self.files or self.file_gen.get(origin[1]) != origin[2]:
                    _reject_at(
                        f"{name} is a chunk of {origin[1]} that is read again or closed "
                        "before this use",
                        node,
                    )

    def _origins_of(self, node: ast.expr) -> list[tuple[Any, ...]]:
        node = unwrap_index(node)
        if isinstance(node, ast.Name):
            loc = self.locals.get(node.id)
            return builtins.list(loc.origins) if loc is not None else []
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
            return self._origins_of(node.func.value)
        return []

    def _bind_origins(
        self, name: str, loc: _Local, fresh: bool, origins: list[tuple[Any, ...]], node: ast.AST
    ) -> None:
        if loc.kind == "view" and not fresh:
            old = {o[:2] for o in loc.origins}
            new = {o[:2] for o in origins}
            if old != new and builtins.any(o[0] != "doc" for o in builtins.list(old | new)):
                _reject_at(
                    f"a view cannot be copied into {name}: {name} lives longer than the "
                    "line, chunk or string that the view points into",
                    node,
                )
        loc.origins = origins

    def _new_local(self, name: str, kind: str, node: ast.AST) -> _Local:
        loc = self.locals.get(name)
        if loc is not None:
            if loc.kind != kind and {loc.kind, kind} == {"str", "view"}:
                _reject_at(
                    f"a view cannot be copied into {name}: {name} lives longer than the "
                    "line, chunk or string that the view points into",
                    node,
                )
            if loc.kind != kind:
                _reject_at(
                    f"{name} is {_KIND_WORD[loc.kind]} here and {_KIND_WORD[kind]} elsewhere; "
                    "a local keeps one type",
                    node,
                )
            return loc
        if name in (self.self_name, self.event_name, "jit") or name in self.files:
            _reject_at(f"{name} cannot be assigned: it is already a name of this body", node)
        if self.flow:
            self.local_decls.append(f"{_CTYPE[kind]} v_{name};")
            loc = _Local(kind, f"S->v_{name}", self._loops)
        else:
            self.local_decls.append(f"{_CTYPE[kind]} _v_{name} = {_CINIT[kind]};")
            loc = _Local(kind, f"_v_{name}", self._loops)
        self.locals[name] = loc
        return loc

    def _new_res(self, ctype: str, base: str) -> str:
        self._res_n += 1
        if self.flow:
            self.res_decls.append(f"{ctype} {base}{self._res_n};")
            return f"S->{base}{self._res_n}"
        self.res_decls.append(f"{ctype} _{base}{self._res_n} = NULL;")
        return f"_{base}{self._res_n}"

    def _loc_text(self, node: ast.AST) -> str:
        file, line, col = self._loc(node)
        return _c_str_literal(f"{file}:{line}:{col}")

    def _fail(self, status: str, what: str, arg: str, node: ast.AST) -> str:
        self.uses.add("cap")
        return (
            f"_st = dftu_jit_fail(ctx, &_cap, {status}, {_c_str_literal(what)}, {arg}, "
            f"{self._loc_text(node)}); _unw = 1; goto {self._target()};"
        )

    # A JSON / YAML value: a local of kind ``val`` or a find() chain of one. Its
    # C form is a `const dftu_value*` that is NULL for a missing key.
    def _val_expr(self, node: ast.expr) -> str | None:
        node = unwrap_index(node)
        if isinstance(node, ast.Name):
            loc = self.locals.get(node.id)
            if loc is None or loc.kind != "val":
                return None
            self._check_origins(node.id, loc, node)
            return loc.cname
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "find"
        ):
            base = self._val_expr(node.func.value)
            if base is None:
                return None
            if len(node.args) != 1 or node.keywords:
                _reject_at("find takes one key", node)
            key = node.args[0]
            if isinstance(key, ast.Constant) and isinstance(key.value, str):
                text = _c_str_literal(key.value)
            else:
                cfg = self._cfg_field(key)
                if cfg is None or cfg.kind != "str":
                    _reject_at("the key of find is a string literal or a str config field", key)
                text = f"_c->v_{cfg.name}"
            return f"dftu_obj_get({base}, {text})"
        return None

    def _val_call(self, node: ast.expr) -> tuple[str, str] | None:
        node = unwrap_index(node)
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)):
            return None
        meth = node.func.attr
        if meth not in _VAL_METHODS:
            return None
        base = self._val_expr(node.func.value)
        if base is None:
            return None
        if meth == "find":
            found = self._val_expr(node)
            assert found is not None
            return "val", found
        args = node.args
        if node.keywords or len(args) > (0 if meth == "is_null" else 1):
            _reject_at(
                f"{meth} takes {'no' if meth == 'is_null' else 'at most one'} argument", node
            )
        if meth == "is_null":
            return "i64", f"dftu_jit_is_null({base})"
        if meth == "as_str":
            if args:
                if not (self._is_str_element(args[0]) or self._is_str_literal(args[0])):
                    _reject_at("the default of as_str is a string", args[0])
                dflt = (
                    self._str_literal(args[0].value)
                    if isinstance(args[0], ast.Constant) and isinstance(args[0].value, str)
                    else self._value(args[0])
                )
            else:
                dflt = self._str_literal("")
            return "str", f"dftu_jit_as_str(host, {base}, {dflt})"
        if meth == "as_bool":
            if args and isinstance(args[0], ast.Constant) and isinstance(args[0].value, bool):
                dflt = "1" if args[0].value else "0"
            elif args:
                dflt = f"({self._arith(args[0])[0]}) != 0"
            else:
                dflt = "0"
            return "i64", f"dftu_as_bool({base}, {dflt})"
        if meth == "as_int":
            dflt = "0"
            if args:
                expr, is_float = self._arith(args[0])
                if is_float:
                    _reject_at("the default of as_int is an int", args[0])
                dflt = f"(int64_t)({expr})"
            return "i64", f"dftu_as_i64({base}, {dflt})"
        dflt = f"(double)({self._arith(args[0])[0]})" if args else "0.0"
        return "f64", f"dftu_as_f64({base}, {dflt})"

    def _len_call(self, node: ast.expr) -> str | None:
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "len"
            and len(node.args) == 1
            and not node.keywords
        ):
            return None
        arg = node.args[0]
        val = self._val_expr(arg)
        if val is not None:
            return f"dftu_jit_count({val})"
        if self._is_view_node(arg):
            return f"(int64_t)({self._view_expr(arg)}).n"
        if (
            isinstance(arg, ast.Name)
            and arg.id in self.locals
            and self.locals[arg.id].kind == "str"
        ):
            return f"dftu_jit_slen(host, {self.locals[arg.id].cname})"
        return None

    def _is_view_node(self, node: ast.expr) -> bool:
        node = unwrap_index(node)
        if isinstance(node, ast.Name):
            loc = self.locals.get(node.id)
            return loc is not None and loc.kind == "view"
        return (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "as_str"
            and self._val_expr(node.func.value) is not None
        )

    def _view_expr(self, node: ast.expr) -> str:
        """The C ``DftuView`` of a string expression, read from its bytes. A view
        is not interned: ``_value`` interns one only where an id is needed."""
        node = unwrap_index(node)
        if isinstance(node, ast.Name) and node.id in self.locals:
            loc = self._local(node)
            if loc.kind == "view":
                return loc.cname
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "as_str"
        ):
            base = self._val_expr(node.func.value)
            if base is not None:
                if len(node.args) > 1 or node.keywords:
                    _reject_at("as_str takes at most one argument", node)
                if node.args:
                    arg = node.args[0]
                    if not (self._is_str_element(arg) or self._is_str_literal(arg)):
                        _reject_at("the default of as_str is a string", arg)
                    dflt = self._view_expr(arg)
                else:
                    dflt = 'dftu_jit_view("", 0)'
                return f"dftu_jit_as_view({base}, {dflt})"
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return f"dftu_jit_view({_c_str_literal(node.value)}, {len(node.value.encode('utf-8'))})"
        cfg = self._cfg_field(node)
        if cfg is not None and cfg.kind == "str":
            return f"dftu_jit_view_of(host, _cfgs_{cfg.name})"
        operand = self._str_operand(node)
        if operand is None:
            _reject_at("a string compared with something that is not a string field", node)
        return f"dftu_jit_view_of(host, {operand})"

    def _expr_kind(self, node: ast.expr) -> str:
        node = unwrap_index(node)
        if isinstance(node, ast.Name):
            kind = self._local(node).kind
            return "str" if kind == "view" else kind
        vc = self._val_call(node)
        if vc is not None:
            return vc[0]
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return "str"
        if self._is_str_element(node) or self._is_str_literal(node) or self._is_none_sentinel(node):
            return "str"
        _, is_float = self._arith(node)
        return "f64" if is_float else "i64"

    def _rhs_of_kind(self, node: ast.expr, kind: str) -> str:
        node = unwrap_index(node)
        if kind == "val":
            val = self._val_expr(node)
            assert val is not None
            return val
        if kind == "view":
            return self._view_expr(node)
        if kind == "str":
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                return self._str_literal(node.value)
            return self._value(node)
        expr, _ = self._arith(node)
        return expr

    def _is_parse_call(self, node: ast.expr) -> str | None:
        if isinstance(node, ast.Call):
            name = self._jit_name(node.func)
            if name in ("json", "yaml"):
                return name
        return None

    def _flow_stmt(self, stmt: ast.stmt) -> list[str] | None:
        if isinstance(stmt, ast.AsyncFor):
            return self._async_for(stmt)
        if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call):
            port = self._chan_call(stmt.value)
            if port is not None:
                return self._port_stmt(port)
        if isinstance(stmt, ast.Assign):
            if len(stmt.targets) == 1 and isinstance(stmt.targets[0], (ast.Name, ast.Tuple)):
                return self._assign(stmt.targets[0], stmt.value, stmt)
            return None
        if (
            isinstance(stmt, ast.AnnAssign)
            and isinstance(stmt.target, ast.Name)
            and stmt.value is not None
        ):
            return self._assign(stmt.target, stmt.value, stmt)
        if isinstance(stmt, ast.AugAssign) and isinstance(stmt.target, ast.Name):
            return self._aug_local(stmt)
        if isinstance(stmt, ast.With):
            return self._with(stmt)
        if isinstance(stmt, ast.For):
            return self._for(stmt)
        if isinstance(stmt, ast.Return):
            return self._return(stmt)
        if isinstance(stmt, ast.Pass):
            return []
        if isinstance(stmt, ast.Expr):
            value = stmt.value
            if isinstance(value, ast.Await):
                return self._await_stmt(None, value, stmt)
            if isinstance(value, ast.Constant):
                return []
            if isinstance(value, ast.Call):
                name = self._jit_name(value.func)
                if name == "emit":
                    return self._emit_call(value)
                if self._task_call(value) is not None:
                    _reject_at(
                        "a task call must be awaited (await self.<task>(...)) or passed to "
                        "jit.all or jit.any"
                        if self.flow
                        else "a task is started from the @jit.finalize body: on_batch returns "
                        "before the task could finish and cannot wait for it",
                        value,
                    )
        return None

    def _assign(self, target: ast.expr, value: ast.expr, stmt: ast.stmt) -> list[str]:
        if isinstance(value, ast.Await):
            return self._await_stmt(target, value, stmt)
        port = self._chan_call(value)
        if port is not None:
            return self._port_assign(target, port, stmt)
        if isinstance(target, ast.Tuple):
            _reject_at("tuple assignment needs `await jit.all(...)` on the right", target)
        assert isinstance(target, ast.Name)
        name = target.id
        parse = self._is_parse_call(value)
        if parse is not None:
            assert isinstance(value, ast.Call)
            return self._parse(name, parse, value, target)
        if isinstance(value, ast.Call) and self._jit_name(value.func) in (
            "open",
            "sleep",
            "all",
            "any",
        ):
            _reject_at(
                f"jit.{self._jit_name(value.func)} cannot be assigned"
                + (
                    " (use `with jit.open(path) as f`)"
                    if self._jit_name(value.func) == "open"
                    else " (await it)"
                ),
                value,
            )
        if (
            isinstance(value, ast.Call)
            and isinstance(value.func, ast.Attribute)
            and value.func.attr == "read"
            and isinstance(value.func.value, ast.Name)
            and value.func.value.id in self.files
        ):
            return self._read_stmt(name, value, target)
        self._rhs_node = unwrap_index(value)
        kind = self._expr_kind(value)
        if kind == "str" and self._is_view_node(value):
            kind = "view"
        fresh = name not in self.locals
        loc = self._new_local(name, kind, target)
        rhs = self._rhs_of_kind(value, kind)
        if kind in ("view", "val"):
            self._bind_origins(name, loc, fresh, self._origins_of(value), target)
        return [f"{loc.cname} = {rhs};"]

    def _aug_local(self, stmt: ast.AugAssign) -> list[str]:
        assert isinstance(stmt.target, ast.Name)
        loc = self._local(stmt.target)
        sym = ENGINE_ARITH_SYMBOL.get(type(stmt.op))
        if sym is None:
            _reject_at("augmented operator (only += -= *= /= are allowed)", stmt)
        if loc.kind not in ("i64", "f64"):
            _reject_at(
                f"{stmt.target.id} is {_KIND_WORD[loc.kind]} and cannot be accumulated", stmt
            )
        expr, is_float = self._arith(stmt.value)
        if isinstance(stmt.op, ast.Div):
            if loc.kind != "f64":
                _reject_at("/= makes a float; the local is an int", stmt)
            return [f"{loc.cname} /= (double)({expr});"]
        if is_float and loc.kind == "i64":
            _reject_at("a float expression cannot be added into an int local", stmt)
        return [f"{loc.cname} {sym}= {expr};"]

    def _parse(self, name: str, fmt: str, call: ast.Call, target: ast.expr) -> list[str]:
        if len(call.args) != 1 or call.keywords:
            _reject_at(f"jit.{fmt} takes one text", call)
        if self._expr_kind(call.args[0]) != "str":
            _reject_at(f"the text of jit.{fmt} is a string", call.args[0])
        is_view = self._is_view_node(call.args[0])
        text = self._view_expr(call.args[0]) if is_view else self._rhs_of_kind(call.args[0], "str")
        loc = self._new_local(name, "val", target)
        if self._loops:
            for other_name, other in self.locals.items():
                if (
                    other_name != name
                    and other.depth < self._loops
                    and builtins.any(o[0] == "doc" and o[1] == name for o in other.origins)
                ):
                    _reject_at(
                        f"{name} is parsed again inside a loop, but {other_name} was taken from "
                        f"it before the loop and would point into freed memory",
                        target,
                    )
        gen = self.doc_gen.get(name, -1) + 1
        self.doc_gen[name] = gen
        loc.origins = [("doc", name, gen)]
        doc = self.docs.get(name)
        if doc is None:
            doc = self._new_res("dftu_doc*", "doc")
            self.docs[name] = doc
            self.res_frees.append(
                f"if ({doc}) {{ {'_fmtj' if fmt == 'json' else '_fmty'}->doc_free({doc}); {doc} = NULL; }}"
            )
        svc = "_fmtj" if fmt == "json" else "_fmty"
        self.uses.add(fmt)
        self.uses.add("cap")
        fail = self._fail("_p", fmt, '""', call)
        if is_view:
            get_text = [
                f"    DftuView _tv = {text};",
                "    const char* _tp = _tv.p;",
                "    uint32_t _tl = _tv.n;",
            ]
        else:
            get_text = [
                "    uint32_t _tl = 0;",
                f"    const char* _tp = {text} == DFTU_STR_NONE ? NULL : host->resolve(host->h, {text}, &_tl);",
            ]
        return [
            "{",
            *get_text,
            '    if (!_tp) { _tp = ""; _tl = 0; }',
            "    _cap.has = 0;",
            "    dftu_doc* _nd = NULL;",
            f"    dftu_status _p = {svc}->parse(&_cap.c, host->h, _tp, _tl, &_nd);",
            f"    if (_p < 0) {{ {fail} }}",
            f"    if ({doc}) {svc}->doc_free({doc});",
            f"    {doc} = _nd;",
            f"    {loc.cname} = {svc}->root({doc});",
            "}",
        ]

    def _no_await(self, body: list[ast.stmt], what: str) -> None:
        for stmt in body:
            for sub in ast.walk(stmt):
                if isinstance(sub, (ast.Await, ast.AsyncFor)):
                    _reject_at(f"await inside {what}; await before or after it", sub)

    def _path_text(self, node: ast.expr) -> tuple[str, str]:
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            if "\0" in node.value:
                _reject_at("a path has no NUL byte", node)
            lit = _c_str_literal(node.value)
            return lit, lit
        cfg = self._cfg_field(node)
        if cfg is not None and cfg.kind == "str":
            return f"_c->v_{cfg.name}", f"_c->v_{cfg.name}"
        _reject_at("the path of jit.open is a string literal or a str config field", node)

    def _with(self, stmt: ast.With) -> list[str]:
        if len(stmt.items) != 1:
            _reject_at("`with` takes one jit.open(path) as f", stmt)
        item = stmt.items[0]
        call = item.context_expr
        if not (isinstance(call, ast.Call) and self._jit_name(call.func) == "open"):
            _reject(_describe(stmt), stmt)
        if len(call.args) != 1 or call.keywords:
            _reject_at("jit.open takes one path", call)
        if not isinstance(item.optional_vars, ast.Name):
            _reject_at("bind the file: with jit.open(path) as f", stmt)
        fname = item.optional_vars.id
        if fname in self.locals or fname in self.files:
            _reject_at(f"{fname} is already a name of this body", item.optional_vars)
        self._no_await(stmt.body, "a with block")
        path_c, _ = self._path_text(call.args[0])
        reads: list[int] = []
        iterated = False
        for sub in ast.walk(ast.Module(body=stmt.body, type_ignores=[])):
            if isinstance(sub, ast.For) and isinstance(sub.iter, ast.Name) and sub.iter.id == fname:
                iterated = True
            if (
                isinstance(sub, ast.Call)
                and isinstance(sub.func, ast.Attribute)
                and isinstance(sub.func.value, ast.Name)
                and sub.func.value.id == fname
            ):
                if sub.func.attr != "read":
                    _reject_at(
                        f"file method .{sub.func.attr}; use `for line in f` or f.read(n)", sub
                    )
                if len(sub.args) != 1 or sub.keywords:
                    _reject_at("f.read takes one byte count", sub)
                n = sub.args[0]
                if not (
                    isinstance(n, ast.Constant)
                    and isinstance(n.value, int)
                    and not isinstance(n.value, bool)
                    and n.value > 0
                ):
                    _reject_at("f.read takes a positive integer literal", n)
                if reads and reads[0] != n.value:
                    _reject_at("one file reads with one size", n)
                reads.append(n.value)
        if iterated and reads:
            _reject_at("one file is read with `for line in f` or with f.read(n), not both", stmt)
        mode = "chunks" if reads else "lines"
        handle = self._new_res(
            "dftu_chunk_reader*" if mode == "chunks" else "dftu_line_reader*", "fh"
        )
        self.files[fname] = (handle, mode)
        self.file_gen[fname] = self.file_gen.get(fname, 0) + 1
        self.file_path[fname] = path_c
        self.uses.add("fileio")
        self.uses.add("cap")
        self.uses.add("stats")
        outer = self._target()
        n_label = self._label_n
        self._label_n += 1
        wexit = f"_wexit{n_label}"
        open_call = (
            f"_fio->chunks_open(&_cap.c, host->h, {path_c}, {reads[0]}, &{handle})"
            if mode == "chunks"
            else f"_fio->lines_open(&_cap.c, host->h, {path_c}, 0, 0, &{handle})"
        )
        close_call = (
            f"_fio->chunks_close(host->h, {handle})"
            if mode == "chunks"
            else f"_fio->lines_close(host->h, {handle})"
        )
        self._targets.append(wexit)
        defs, body = self._compile_scope(stmt.body)
        self._targets.pop()
        del self.files[fname]
        lines = ["{", "    {", "        _cap.has = 0;", f"        dftu_status _o = {open_call};"]
        lines.append(
            f'        if (_o < 0) {{ _st = dftu_jit_fail(ctx, &_cap, _o, "open", {path_c}, '
            f"{self._loc_text(stmt)}); _unw = 1; goto {outer}; }}"
        )
        lines += ["        dftu_jit_stat_inc(2);", "    }", "    {"]
        lines.extend("        " + ln for ln in defs + body)
        lines += [
            "    }",
            f"{wexit}: ;",
            f"    {close_call};",
            f"    {handle} = NULL;",
            "    dftu_jit_stat_inc(3);",
            f"    if (_unw) goto {outer};",
            "}",
        ]
        return lines

    def _for(self, stmt: ast.For) -> list[str]:
        if stmt.orelse:
            _reject_at("`for` with `else`", stmt)
        it = stmt.iter
        # for line in f
        if isinstance(it, ast.Name) and it.id in self.files:
            handle, mode = self.files[it.id]
            if mode != "lines":
                _reject_at(f"{it.id} is read with read(n); it cannot be iterated", it)
            if not isinstance(stmt.target, ast.Name):
                _reject_at("the loop variable of `for line in f` is one name", stmt.target)
            self._no_await(stmt.body, "a for loop")
            if it.id in self._iter_files:
                _reject_at(
                    f"{it.id} is already iterated by an enclosing for loop; "
                    "a second loop over it would overwrite the line of the first",
                    it,
                )
            var = self._new_local(stmt.target.id, "view", stmt.target)
            self._loop_seq += 1
            self.file_gen[it.id] = self.file_gen.get(it.id, 0) + 1
            var.origins = [("loop", self._loop_seq), ("file", it.id, self.file_gen[it.id])]
            path_c = self.file_path[it.id]
            self._loops += 1
            self._active_loops.append(self._loop_seq)
            self._iter_files.append(it.id)
            defs, body = self._compile_scope(stmt.body)
            self._iter_files.pop()
            self._active_loops.pop()
            self._loops -= 1
            lines = [
                "{",
                "    dftu_line_batch _lb;",
                "    uint32_t _li = 0;",
                "    _lb.lines = NULL;",
                "    _lb.first_line = 0;",
                "    _lb.count = 0;",
                "    for (;;) {",
                "        if (_li >= _lb.count) {",
                "            dftu_task* _t = NULL;",
                "            _cap.has = 0;",
                f"            dftu_status _s = _fio->lines_next(&_cap.c, host->h, {handle}, 0, &_lb, &_t);",
                "            if (_s >= 0) _s = _tsk->run(&_cap.c, host->h, _t);",
                "            if (_s == DFTU_END) break;",
                f"            if (_s < 0) {{ {self._fail('_s', 'read', path_c, stmt)} }}",
                "            _li = 0;",
                "        }",
                f"        {var.cname} = dftu_jit_view((const char*)_lb.lines[_li].data, "
                "(uint32_t)_lb.lines[_li].len);",
                "        ++_li;",
            ]
            lines.extend("        " + ln for ln in defs + body)
            lines += ["    }", "}"]
            self.uses.add("task")
            return lines
        # for x in value / for k, x in value.items()
        val = self._val_expr(it)
        pairs = (
            isinstance(it, ast.Call)
            and isinstance(it.func, ast.Attribute)
            and it.func.attr == "items"
            and not it.args
            and self._val_expr(it.func.value) is not None
        )
        if pairs:
            assert isinstance(it, ast.Call) and isinstance(it.func, ast.Attribute)
            val = self._val_expr(it.func.value)
        if val is None:
            _reject(_describe(stmt), stmt)
        self._no_await(stmt.body, "a for loop")
        if pairs:
            if not (
                isinstance(stmt.target, ast.Tuple)
                and len(stmt.target.elts) == 2
                and builtins.all(isinstance(t, ast.Name) for t in stmt.target.elts)
            ):
                _reject_at("`for key, value in v.items()` takes two names", stmt.target)
            key_t, val_t = stmt.target.elts
            assert isinstance(key_t, ast.Name) and isinstance(val_t, ast.Name)
            key_loc = self._new_local(key_t.id, "view", key_t)
            item_loc = self._new_local(val_t.id, "val", val_t)
            assert isinstance(it, ast.Call) and isinstance(it.func, ast.Attribute)
            doc_origins = self._origins_of(it.func.value)
            key_loc.origins = [("loop", self._loop_seq + 1)] + doc_origins
            item_loc.origins = builtins.list(doc_origins)
            head = [
                "{",
                f"    const dftu_value* _v = {val};",
                "    uint32_t _n = (_v && _v->kind == DFTU_VAL_OBJECT) ? _v->count : 0;",
                "    for (uint32_t _k = 0; _k < _n; ++_k) {",
                f"        {key_loc.cname} = dftu_jit_view(_v->as.members[_k].key, "
                "_v->as.members[_k].key_len);",
                f"        {item_loc.cname} = _v->as.members[_k].value;",
            ]
        else:
            if not isinstance(stmt.target, ast.Name):
                _reject_at("the loop variable of `for x in v` is one name", stmt.target)
            item_loc = self._new_local(stmt.target.id, "val", stmt.target)
            item_loc.origins = self._origins_of(it)
            head = [
                "{",
                f"    const dftu_value* _v = {val};",
                "    uint32_t _n = (_v && (_v->kind == DFTU_VAL_ARRAY || _v->kind == DFTU_VAL_OBJECT)) "
                "? _v->count : 0;",
                "    for (uint32_t _k = 0; _k < _n; ++_k) {",
                f"        {item_loc.cname} = _v->kind == DFTU_VAL_ARRAY ? &_v->as.items[_k] "
                ": _v->as.members[_k].value;",
            ]
        self._loop_seq += 1
        self._loops += 1
        self._active_loops.append(self._loop_seq)
        defs, body = self._compile_scope(stmt.body)
        self._active_loops.pop()
        self._loops -= 1
        head.extend("        " + ln for ln in defs + body)
        return head + ["    }", "}"]

    def _read_stmt(self, name: str, call: ast.Call, target: ast.expr) -> list[str]:
        assert isinstance(call.func, ast.Attribute) and isinstance(call.func.value, ast.Name)
        fname = call.func.value.id
        handle, mode = self.files[fname]
        if mode != "chunks":
            _reject_at(f"{fname} is iterated with `for`; it has no read(n)", call)
        fresh = name not in self.locals
        loc = self._new_local(name, "view", target)
        self.file_gen[fname] = self.file_gen.get(fname, 0) + 1
        self._bind_origins(name, loc, fresh, [("file", fname, self.file_gen[fname])], target)
        path_c = self.file_path[fname]
        self.uses.add("task")
        return [
            "{",
            "    dftu_bytes _cb;",
            "    _cb.data = NULL;",
            "    _cb.len = 0;",
            "    _cb.free_fn = NULL;",
            "    _cb.ud = NULL;",
            "    dftu_task* _t = NULL;",
            "    _cap.has = 0;",
            f"    dftu_status _s = _fio->chunks_next(&_cap.c, host->h, {handle}, &_cb, &_t);",
            "    if (_s >= 0) _s = _tsk->run(&_cap.c, host->h, _t);",
            f'    if (_s == DFTU_END) {{ {loc.cname} = dftu_jit_view("", 0); }}',
            f"    else if (_s < 0) {{ {self._fail('_s', 'read', path_c, call)} }}",
            f"    else {{ {loc.cname} = dftu_jit_view((const char*)_cb.data, (uint32_t)_cb.len); }}",
            "}",
        ]

    def _emit_call(self, call: ast.Call) -> list[str]:
        if not self.flow or self.in_task:
            _reject_at("jit.emit is a statement of a @jit.finalize body", call)
        if len(call.args) != 2 or call.keywords:
            _reject_at("jit.emit takes a result name and a value", call)
        name = call.args[0]
        if not (isinstance(name, ast.Constant) and isinstance(name.value, str) and name.value):
            _reject_at("the result name of jit.emit is a string literal", name)
        if name.value in self.emit_names:
            _reject_at(f"jit.emit('{name.value}') twice; a result is emitted once", name)
        if name.value in self.maps:
            _reject_at(f"'{name.value}' is a map of this plugin; emit another name", name)
        self.emit_names.append(name.value)
        kind = self._expr_kind(call.args[1])
        if kind == "val":
            _reject_at("a JSON value is emitted through as_int, as_str, ...", call.args[1])
        value = self._rhs_of_kind(call.args[1], kind)
        self.uses.add("result")
        body: list[str]
        if kind == "str" and self._is_view_node(call.args[1]):
            body = [
                f"    DftuView _ev = {self._view_expr(call.args[1])};",
                "    _ep = _ev.p;",
                "    _en = _ev.n;",
                '    if (!_ep) { _ep = ""; _en = 0; }',
            ]
        elif kind == "str":
            body = [
                f"    _ep = host->resolve(host->h, {value}, &_en);",
                '    if (!_ep) { _ep = ""; _en = 0; }',
            ]
        elif kind == "f64":
            body = [f'    _en = (uint32_t)snprintf(_eb, sizeof(_eb), "%.17g", (double)({value}));']
        else:
            body = [
                f'    _en = (uint32_t)snprintf(_eb, sizeof(_eb), "%lld", (long long)({value}));'
            ]
        return [
            "{",
            "    char _eb[48];",
            "    const char* _ep = _eb;",
            "    uint32_t _en = 0;",
            *body,
            "    dftu_result_value _rv;",
            "    memset(&_rv, 0, sizeof(_rv));",
            "    _rv.kind = DFTU_RESULT_KIND_BYTES;",
            "    _rv.u.bytes.data = _ep;",
            "    _rv.u.bytes.len = _en;",
            f"    dftu_status _er = _res->emit(ctx, host->h, {_c_str_literal(name.value)}, &_rv);",
            f"    if (_er < 0) {{ _st = _er; _unw = 1; goto {self._target()}; }}",
            "}",
        ]

    def _return(self, stmt: ast.Return) -> list[str]:
        if not self.flow:
            _reject(_describe(stmt), stmt)
        lines: list[str] = []
        if stmt.value is not None:
            if self.task_ret is None:
                _reject_at("this body returns nothing; use a bare `return`", stmt)
            kind = self._expr_kind(stmt.value)
            if kind not in ("i64", "f64"):
                _reject_at("a task returns an int or a float", stmt.value)
            expr, is_float = self._arith(stmt.value)
            if is_float and self.task_ret == "i64":
                _reject_at("this task returns an int; the value is a float", stmt.value)
            ctype = "int64_t" if self.task_ret == "i64" else "double"
            lines.append(f"if (S->out) *({ctype}*)S->out = ({ctype})({expr});")
        elif self.task_ret is not None:
            _reject_at("this task returns a value; write `return <value>`", stmt)
        lines.append(f"_unw = 1; goto {self._target()};")
        return ["{"] + ["    " + ln for ln in lines] + ["}"]

    def _task_call(self, node: ast.expr) -> tuple[_TaskSig, list[ast.expr]] | None:
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == self.self_name
            and node.func.attr in self.tasks
        ):
            return None
        return self.tasks[node.func.attr], builtins.list(node.args)

    def _job_of(
        self, node: ast.expr, lines: list[str], nested: bool
    ) -> tuple[str, list[str | None], bool]:
        """Build the job of an awaitable into ``lines``: its C variable, the kinds
        of its outputs and whether it is a bare sleep."""
        node = unwrap_index(node)
        if not isinstance(node, ast.Call):
            _reject_at("await of something that is not a call", node)
        self._job_n += 1
        var = f"_j{self._job_n}"
        name = self._jit_name(node.func)
        if name == "sleep":
            if nested:
                _reject_at(
                    "jit.sleep inside jit.all or jit.any: race a task that sleeps and returns a "
                    "value instead",
                    node,
                )
            if len(node.args) != 1 or node.keywords:
                _reject_at("jit.sleep takes the seconds to wait", node)
            secs, _ = self._arith(node.args[0])
            lines += [
                f"dftu_job* {var} = NULL;",
                "{",
                f"    double _sv = (double)({secs});",
                "    uint64_t _ns = _sv > 0 ? (uint64_t)(_sv * 1e9) : 0;",
                f"    dftu_status _m = _tsk->sleep(ctx, host->h, _ns, &{var});",
                f"    if (_m < 0) {{ _st = _m; _unw = 1; goto {self._target()}; }}",
                "}",
            ]
            return var, [], True
        call = self._task_call(node)
        if call is not None:
            sig, args = call
            if len(args) != len(sig.params) or node.keywords:
                _reject_at(f"{sig.name} takes {len(sig.params)} argument(s), got {len(args)}", node)
            cargs: list[str] = []
            for arg, (pname, pkind) in zip(args, sig.params):
                expr, is_float = self._arith(arg)
                if is_float and pkind == "i64":
                    _reject_at(
                        f"parameter {pname} of {sig.name} is an int; the value is a float", arg
                    )
                cargs.append(f"({'int64_t' if pkind == 'i64' else 'double'})({expr})")
            lines += [
                f"dftu_job* {var} = NULL;",
                "{",
                f"    dftu_status _m = _mkjob_{sig.name}(ctx, R, {''.join(c + ', ' for c in cargs)}&{var});",
                f"    if (_m < 0) {{ _st = _m; _unw = 1; goto {self._target()}; }}",
                "}",
            ]
            self.uses.add("task")
            return var, [sig.ret], False
        if name in ("all", "any"):
            if nested:
                _reject_at(f"jit.{name} inside jit.all or jit.any", node)
            if len(node.args) < 2 or node.keywords:
                _reject_at(f"jit.{name} takes two or more task calls", node)
            kids: list[str] = []
            kinds: list[str | None] = []
            for arg in node.args:
                kid, kk, _ = self._job_of(arg, lines, True)
                if kk[0] is None:
                    _reject_at(f"jit.{name} takes tasks that return a value", arg)
                kids.append(kid)
                kinds.append(kk[0])
            if name == "any" and builtins.any(k != kinds[0] for k in kinds):
                _reject_at("the tasks of jit.any share one return type", node)
            lines += [
                f"dftu_job* {var} = NULL;",
                "{",
                f"    dftu_job* _ks[{len(kids)}] = {{{', '.join(kids)}}};",
                f"    dftu_status _m = _tsk->{name}(ctx, host->h, _ks, {len(kids)}, &{var});",
                f"    if (_m < 0) {{ _st = _m; _unw = 1; goto {self._target()}; }}",
                "}",
            ]
            return var, (kinds if name == "all" else [kinds[0]]), False
        _reject_at(
            "await of this call; the awaitables are jit.sleep(s), jit.all(...), jit.any(...), "
            "a task of this class, a store get, and a channel send or receive",
            node,
        )

    def _await_stmt(self, target: ast.expr | None, aw: ast.Await, stmt: ast.stmt) -> list[str]:
        if not self.flow or not self.is_async:
            _reject_at("await outside an `async def` @jit.finalize or @jit.task body", aw)
        inner = unwrap_index(aw.value)
        port = self._chan_call(inner)
        if port is not None:
            return self._port_await(target, port, stmt)
        if isinstance(inner, ast.Call) and self._store_method(inner) is not None:
            if not isinstance(target, ast.Name):
                _reject_at("assign the value of a store get: x = await self.<store>.get(k, d)", aw)
            return self._assign(target, inner, stmt)
        jl: list[str] = []
        var, kinds, sleeping = self._job_of(inner, jl, False)
        self._await_n += 1
        k = self._await_n
        outer = self._target()
        has_out = builtins.any(kd is not None for kd in kinds)
        if has_out:
            self.ob_size = builtins.max(self.ob_size, 8 * len(kinds))
        decode: list[str] = []
        if target is not None:
            if not has_out:
                _reject_at("this awaitable gives no value to assign", target)
            names: list[ast.expr] = (
                builtins.list(target.elts) if isinstance(target, ast.Tuple) else [target]
            )
            if len(names) != len(kinds):
                _reject_at(f"{len(kinds)} result(s) to unpack into {len(names)} name(s)", target)
            for idx, (tn, kd) in enumerate(zip(names, kinds)):
                if not isinstance(tn, ast.Name):
                    _reject_at("a result is assigned to a name", tn)
                assert kd is not None
                loc = self._new_local(tn.id, kd, tn)
                decode.append(f"memcpy(&{loc.cname}, S->ob + {8 * idx}, 8);")
        cancel = _c_str_literal(
            f"jit.Cancelled [{self._loc_text_plain(stmt)}]: cancelled at an await"
        )
        out = "S->ob" if has_out else "NULL"
        lines = ["{"]
        lines.extend("    " + ln for ln in jl)
        lines += [
            f"    dftu_status _w = _tsk->start(ctx, host->h, {var}, NULL, {out}, &wait[0]);",
            f"    if (_w < 0) {{ _st = _w; _unw = 1; goto {outer}; }}",
            "    *n_wait = 1;",
            "    S->tk = wait[0];",
            f"    S->pc = {k};",
            "    return DFTU_OK;",
            "}",
            f"case {k}: ;",
            "{",
            "    if (dftu_check(ctx) < 0) {",
            "        dftu_jit_stat_inc(6);",
            f"        _st = dftu_fail(ctx, DFTU_E_CANCELLED, {cancel}); _unw = 1; goto {outer};",
            "    }",
            "    dftu_status _w = _tsk->status(ctx, host->h, S->tk);",
            "    S->tk = NULL;",
        ]
        if sleeping:
            lines.append("    if (_w == DFTU_E_TIMEOUT) _w = DFTU_OK;")
        lines.append(f"    if (_w < 0) {{ _st = _w; _unw = 1; goto {outer}; }}")
        lines.extend("    " + ln for ln in decode)
        lines.append("}")
        return lines

    def _loc_text_plain(self, node: ast.AST) -> str:
        file, line, col = self._loc(node)
        return f"{file}:{line}:{col}"

    task_ret: Optional[str] = None

    def _cfg_field(self, node: ast.expr) -> CfgField | None:
        """The config field ``self.config.<name>`` names, else None."""
        if not (
            self.config_fields
            and isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Attribute)
            and node.value.attr == "config"
            and isinstance(node.value.value, ast.Name)
            and node.value.value.id == self.self_name
        ):
            return None
        field = self.config_fields.get(node.attr)
        if field is None:
            _reject(f"unknown config field self.config.{node.attr}", node)
        return field

    # ---- plain-container scan state and jit.Store ----------------------------

    def _self_base(self, node: ast.expr) -> str | None:
        """The attribute name of the ``self.<attr>`` at the base of a chain of
        subscripts, attributes and calls, else None."""
        cur: ast.expr = node
        while True:
            if isinstance(cur, ast.Subscript):
                cur = cur.value
            elif isinstance(cur, ast.Call):
                cur = cur.func
            elif isinstance(cur, ast.Attribute):
                if isinstance(cur.value, ast.Name) and cur.value.id == self.self_name:
                    return cur.attr
                cur = cur.value
            else:
                return None

    def _plain_attr(self, node: ast.expr) -> str | None:
        base = self._self_base(node)
        return base if base is not None and base in self.plain else None

    def _mentions_state(self, node: ast.AST) -> bool:
        for sub in ast.walk(node):
            if isinstance(sub, ast.Attribute) and isinstance(sub.value, ast.Name):
                if sub.value.id == self.self_name and (
                    sub.attr in self.plain or sub.attr in self.stores
                ):
                    return True
        return False

    def _merge_error(self, field: PlainField, node: ast.AST, what: str) -> NoReturn:
        _reject_at(
            f"cannot infer how worker slices merge self.{field.name} from {what}; "
            f"a {field.kind} is merged only when every update is {_STATE_FORMS[field.kind]} "
            "(an explicit merge() for any other update is not implemented yet)",
            node,
        )

    def _reject_expr(self, node: ast.expr) -> NoReturn:
        if isinstance(node, ast.Await):
            _reject_at(
                "await is a statement of its own, or the whole right side of an assignment "
                "(x = await ...); it cannot sit inside a condition or an expression",
                node,
            )
        base = self._self_base(node)
        if base is not None and base in self.plain:
            _reject_at(
                f"self.{base} is scan state and is write-only in the body: each worker "
                "holds only its own slice, so a read would see a partial value",
                node,
            )
        if base is not None and base in self.stores:
            _reject_at(
                f"self.{base} is a jit.Store: use self.{base}.get(key, default) "
                f"and self.{base}.put(key, value)",
                node,
            )
        if base is not None and base in self.chans:
            _reject_at(
                f"self.{base} is a port: a call on it is a statement of its own "
                "(self.out.send(v)), or the right side of an assignment "
                "(ok = self.out.try_send(v), x = self.latest.get(0), x = await self.inp.recv()); "
                "it cannot sit inside an expression or a condition",
                node,
            )
        _reject(_describe(node), node)

    def _dict_keys(self, field: PlainField, target: ast.expr) -> list[ast.expr]:
        if not (
            isinstance(target, ast.Subscript)
            and isinstance(target.value, ast.Attribute)
            and target.value.attr == field.name
        ):
            self._merge_error(field, target, "this target")
        keys = self._keys(target.slice)
        if len(keys) != len(field.key_types):
            _reject_at(
                f"self.{field.name} takes {len(field.key_types)} key components, got {len(keys)}",
                target,
            )
        return keys

    def _state_call(
        self, field: PlainField, node: ast.expr, method: str, key_dump: str
    ) -> list[ast.expr] | None:
        """The arguments of ``self.<field>.<method>(key, ...)`` for that key."""
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == method
            and self._plain_attr(node.func.value) == field.name
            and isinstance(node.func.value, ast.Attribute)
            and len(node.args) == 2
            and not node.keywords
            and ast.dump(unwrap_index(node.args[0])) == key_dump
        ):
            return None
        return builtins.list(node.args)

    def _dict_assign(self, field: PlainField, stmt: ast.Assign) -> _Update:
        target = stmt.targets[0]
        keys = self._dict_keys(field, target)
        assert isinstance(target, ast.Subscript)
        key_dump = ast.dump(unwrap_index(target.slice))
        value = stmt.value
        if isinstance(value, ast.BinOp) and isinstance(value.op, ast.Add):
            for state_side, other in ((value.left, value.right), (value.right, value.left)):
                got = self._state_call(field, state_side, "get", key_dump)
                zero = got[1] if got is not None else None
                if (
                    zero is not None
                    and isinstance(zero, ast.Constant)
                    and not isinstance(zero.value, bool)
                    and isinstance(zero.value, (int, float))
                    and zero.value == 0
                ):
                    return _Update(field.name, "sum", keys, other, False)
        if (
            isinstance(value, ast.Call)
            and isinstance(value.func, ast.Name)
            and value.func.id in ("max", "min")
            and len(value.args) == 2
            and not value.keywords
        ):
            form = value.func.id
            for state_side, other in (
                (value.args[0], value.args[1]),
                (value.args[1], value.args[0]),
            ):
                if (
                    isinstance(state_side, ast.Subscript)
                    and self._plain_attr(state_side.value) == field.name
                    and isinstance(state_side.value, ast.Attribute)
                    and ast.dump(unwrap_index(state_side.slice)) == key_dump
                ):
                    if not field.factory:
                        _reject_at(
                            f"self.{field.name}[k] raises KeyError for a missing key on a plain "
                            f"dict: declare {field.name} = defaultdict({_STATE_PY[field.elem]}) "
                            f"or write self.{field.name}[k] = {form}(self.{field.name}.get(k, x), x)",
                            state_side,
                        )
                    return _Update(field.name, form, keys, other, True)
                got = self._state_call(field, state_side, "get", key_dump)
                if got is not None and ast.dump(got[1]) == ast.dump(other):
                    return _Update(field.name, form, keys, other, False)
        self._merge_error(field, stmt, "this assignment")

    def _plain_update(self, stmt: ast.stmt) -> _Update | None:
        """The merge-inferable update ``stmt`` makes to a plain-container
        field, None when ``stmt`` touches none, and a compile error when it
        touches one in a form whose merge cannot be inferred."""
        if isinstance(stmt, ast.AugAssign):
            attr = self._plain_attr(stmt.target)
            if attr is None:
                return None
            field = self.plain[attr]
            if not isinstance(stmt.op, ast.Add):
                self._merge_error(field, stmt, "an augmented operator other than +=")
            if field.kind == "scalar":
                if not (isinstance(stmt.target, ast.Attribute) and stmt.target.attr == attr):
                    self._merge_error(field, stmt, "this target")
                return _Update(attr, "sum", [], stmt.value, False)
            if field.kind == "dict":
                keys = self._dict_keys(field, stmt.target)
                if not field.factory:
                    _reject_at(
                        f"self.{attr}[k] += x raises KeyError for a missing key on a plain "
                        f"dict: declare {attr} = defaultdict({_STATE_PY[field.elem]}) or write "
                        f"self.{attr}[k] = self.{attr}.get(k, 0) + x",
                        stmt,
                    )
                return _Update(attr, "sum", keys, stmt.value, False)
            self._merge_error(field, stmt, "+=")
        if isinstance(stmt, ast.Assign):
            for tgt in stmt.targets:
                attr = self._plain_attr(tgt)
                if attr is None:
                    continue
                field = self.plain[attr]
                if len(stmt.targets) != 1 or field.kind != "dict":
                    self._merge_error(field, stmt, "this assignment")
                return self._dict_assign(field, stmt)
            return None
        if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call):
            call = stmt.value
            func = call.func
            attr = self._plain_attr(call)
            if attr is None:
                return None
            field = self.plain[attr]
            method = {"set": "add", "list": "append"}.get(field.kind)
            if (
                isinstance(func, ast.Attribute)
                and method is not None
                and func.attr == method
                and isinstance(func.value, ast.Attribute)
                and func.value.attr == attr
                and len(call.args) == 1
                and not call.keywords
            ):
                return _Update(
                    attr, "add" if method == "add" else "append", [], call.args[0], False
                )
            self._merge_error(field, stmt, "this call")
        if _child_blocks(stmt) is None and self._mentions_state(stmt):
            attr = self._plain_attr_in(stmt)
            if attr is not None:
                self._merge_error(self.plain[attr], stmt, "this statement")
        return None

    def _plain_attr_in(self, node: ast.AST) -> str | None:
        for sub in ast.walk(node):
            if isinstance(sub, ast.Attribute) and isinstance(sub.value, ast.Name):
                if sub.value.id == self.self_name and sub.attr in self.plain:
                    return sub.attr
        return None

    def _infer_plain(self, stmts: list[ast.stmt]) -> None:
        """Pass one: the merge of each plain field from its updates, which must
        agree, then its accumulator declaration."""
        for stmt in stmts:
            blocks = _child_blocks(stmt)
            if blocks is not None:
                for block in blocks:
                    self._infer_plain(block)
                continue
            self._cur = stmt
            update = self._plain_update(stmt)
            if update is None:
                continue
            sig = (update.form, update.identity)
            prev = self.plain_forms.get(update.attr)
            if prev is not None and prev != sig:
                _reject_at(
                    f"self.{update.attr} is updated as {prev[0]} elsewhere and as {sig[0]} here "
                    "(or with a different missing-key rule); a field has one merge",
                    stmt,
                )
            self.plain_forms[update.attr] = sig

    def _declare_plain(self) -> None:
        for attr, (form, _) in self.plain_forms.items():
            field = self.plain[attr]
            monoid: Monoid
            if form == "sum":
                monoid = (
                    Counter("DFTU_AGG_SUM", elem="i64")
                    if field.elem == "i64"
                    else Sum("DFTU_AGG_SUM", elem="f64")
                )
            elif form == "max":
                monoid = Max("DFTU_AGG_MAX", elem=field.elem)
            elif form == "min":
                monoid = Min("DFTU_AGG_MIN", elem=field.elem)
            elif form == "add":
                monoid = SetV("DFTU_AGG_SET_UNION", elem=field.elem)
            else:
                monoid = ListV("DFTU_AGG_LIST_SORTED", needs_by=True, elem=field.elem)
            self.maps[attr] = Map(field.key_types, (monoid,), False)

    def _check_family(self, node: ast.expr, kind: str, what: str) -> None:
        is_str = self._is_str_element(node)
        if kind == "str" and not is_str:
            _reject_at(f"{what} is a str and needs a string field such as e.name", node)
        if kind != "str" and is_str:
            _reject_at(f"{what} is a number and cannot be a string field", node)

    def _plain_stmt(self, stmt: ast.stmt) -> list[str] | None:
        """Pass two: the contribution rows of one plain-container update."""
        update = self._plain_update(stmt)
        if update is None:
            return None
        field = self.plain[update.attr]
        decl = self.maps[update.attr]
        monoid = decl.values[0]
        for kn, kt in zip(update.keys, decl.key_types):
            self._check_family(kn, "str" if kt.dft == "DFTU_T_STR" else "i64", "this key component")
        if update.form in ("add", "append"):
            self._check_family(update.value, field.elem, "this element")
        lines: list[str] = []
        if update.identity:
            zero = ast.copy_location(ast.Constant(0), update.value)
            lines += self._emit_row(update.attr, update.keys, 0, monoid, zero, None)
        by: ast.expr | None = None
        if update.form == "append":
            by = ast.copy_location(
                ast.Attribute(ast.Name(self.event_name, ast.Load()), "ts", ast.Load()),
                update.value,
            )
        lines += self._emit_row(update.attr, update.keys, 0, monoid, update.value, by)
        return lines

    def _store_method(self, node: ast.expr) -> tuple[StoreField, str] | None:
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Attribute)
            and isinstance(node.func.value.value, ast.Name)
            and node.func.value.value.id == self.self_name
            and node.func.value.attr in self.stores
        ):
            return None
        return self.stores[node.func.value.attr], node.func.attr

    def _store_key_args(self, field: StoreField, node: ast.expr) -> list[str]:
        keys = self._keys(node)
        if len(keys) != len(field.key_kinds):
            _reject_at(
                f"self.{field.name} takes {len(field.key_kinds)} key components, got {len(keys)}",
                node,
            )
        out: list[str] = []
        for kn, kind in zip(keys, field.key_kinds):
            self._check_family(kn, kind, "this key component")
            if kind == "str":
                out.append(f"(dftu_str)({self._value(kn)})")
            else:
                expr, is_float = self._arith(kn)
                if is_float:
                    _reject_at("a float expression cannot be an int store key", kn)
                out.append(f"(int64_t)({expr})")
        return out

    def _store_get(self, node: ast.expr) -> tuple[str, bool] | None:
        found = self._store_method(node)
        if found is None:
            return None
        field, method = found
        assert isinstance(node, ast.Call)
        if self.in_task:
            _reject_at(
                "a task runs beside others and cannot use the store; use it in the "
                "@jit.finalize body and pass the value as an argument",
                node,
            )
        if method != "get":
            if method == "put":
                _reject_at("self.<store>.put(...) is a statement, not a value", node)
            _reject_at(f"store method .{method}; use get(key, default) and put(key, value)", node)
        if len(node.args) != 2 or node.keywords:
            _reject_at(f"self.{field.name}.get takes a key and a default value", node)
        keys = self._store_key_args(field, node.args[0])
        self._check_family(node.args[1], "i64", "this default")
        default = self._rhs(node.args[1], field.value_kind)
        if self._targets and node is not self._rhs_node:
            _reject_at(
                "inside a try body a store get is the whole right side of an assignment "
                "(v = self.<store>.get(k, d)), so a failed get leaves no update half done",
                node,
            )
        self.store_gets.add(field.name)
        self._store_read = True
        args = ", ".join(["&_se", "host", *keys, default])
        return f"_st_get_{field.name}({args})", field.value_kind == "f64"

    def _store_put(self, call: ast.Call) -> list[str] | None:
        found = self._store_method(call)
        if found is None:
            return None
        field, method = found
        if self.in_task:
            _reject_at(
                "a task runs beside others and cannot use the store; use it in the "
                "@jit.finalize body and pass the value as an argument",
                call,
            )
        if method != "put":
            _reject_at(
                f"store method .{method} as a statement; use put(key, value) (get returns a value)",
                call,
            )
        if len(call.args) != 2 or call.keywords:
            _reject_at(f"self.{field.name}.put takes a key and a value", call)
        keys = self._store_key_args(field, call.args[0])
        self._check_family(call.args[1], "i64", "this value")
        value = self._rhs(call.args[1], field.value_kind)
        self.store_puts.add(field.name)
        return [f"_st_put_{field.name}({', '.join(['&_se', 'host', *keys, value])});"]

    # ---- ports: channels, latest values and phase results --------------------

    def _chan_call(self, node: ast.expr) -> tuple[_PortDecl, str, ast.Call] | None:
        """``(port, method, call)`` for ``self.<port>.<method>(...)``, else None."""
        node = unwrap_index(node)
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Attribute)
            and isinstance(node.func.value.value, ast.Name)
            and node.func.value.value.id == self.self_name
            and node.func.value.attr in self.chans
        ):
            return None
        return self.chans[node.func.value.attr], node.func.attr, node

    def _port_where(self) -> str:
        if not self.flow:
            return "the per-event body"
        return "a @jit.task" if self.in_task else "the @jit.finalize body"

    def _port_fail(self, text: str, node: ast.AST) -> str:
        msg = _c_str_literal(f"jit.Error [{self._loc_text_plain(node)}]: {text}")
        return f"_st = dftu_fail(ctx, DFTU_E_INVALID, {msg}); _unw = 1; goto {self._target()};"

    def _port_handle(self, decl: _PortDecl) -> str:
        self.chan_uses.add(decl.attr)
        self.uses.add("ports")
        return f"R->ch_{decl.attr}" if self.flow else f"_ch_{decl.attr}"

    def _port_value(self, decl: _PortDecl, node: ast.expr, what: str) -> str:
        self._check_family(node, "i64", what)
        expr, is_float = self._arith(node)
        if decl.elem == "i64":
            if is_float:
                _reject_at(
                    f"self.{decl.attr} carries an int; {what} is a float (round it first)", node
                )
            return f"(int64_t)({expr})"
        return f"(double)({expr})"

    def _port_args(self, decl: _PortDecl, method: str, call: ast.Call, count: int) -> None:
        if call.keywords or len(call.args) != count:
            _reject_at(
                f"self.{decl.attr}.{method}() takes "
                + ("no argument" if count == 0 else f"{count} argument(s)"),
                call,
            )

    def _port_checks(self, decl: _PortDecl, method: str, call: ast.Call, awaited: bool) -> None:
        """The rules a call of ``method`` on ``decl`` follows in this body."""
        attr = decl.attr
        methods = {
            "channel": ("send", "try_send", "close", "recv"),
            "latest": ("set", "get"),
            "phase": ("contribute", "seal", "result"),
        }[decl.kind]
        if method not in methods:
            _reject_at(
                f"self.{attr}.{method}(); a {decl.kind} port has " + ", ".join(methods), call
            )
        if decl.kind == "channel":
            sends = method in ("send", "try_send", "close")
            if sends and decl.role == "recv":
                _reject_at(f"self.{attr} is a receiver; it has recv(), not {method}()", call)
            if not sends and decl.role == "send":
                _reject_at(f"self.{attr} is a sender; it has send(), try_send() and close()", call)
            if decl.policy == "block" and not self.flow:
                _reject_at(
                    f"self.{attr} is a Block channel, used from an async @jit.finalize or "
                    "@jit.task, never from the per-event body: a full channel would stop the "
                    "scan loop that has to drain it",
                    call,
                )
            if method in ("recv", "close") and not self.flow:
                _reject_at(
                    f"self.{attr}.{method}() is a call of the @jit.finalize body or a @jit.task"
                    + (": it waits" if method == "recv" else ": it ends the sends of every worker"),
                    call,
                )
            if method == "send" and awaited and not (self.flow and self.is_async):
                _reject_at("await outside an `async def` @jit.finalize or @jit.task body", call)
            if method == "send" and not awaited and decl.policy == "block":
                _reject_at(
                    f"self.{attr} is a Block channel: a send waits for a credit; write "
                    f"`await self.{attr}.send(v)` in an async @jit.finalize or @jit.task body",
                    call,
                )
            if method in ("try_send", "close") and awaited:
                _reject_at(f"self.{attr}.{method}() never waits; do not await it", call)
            if method == "recv" and not awaited:
                _reject_at(
                    f"self.{attr}.recv() waits for a value: `await self.{attr}.recv()`", call
                )
        elif method in ("seal", "result") and not self.flow:
            _reject_at(
                f"self.{attr}.{method}() is a call of the @jit.finalize body or a @jit.task: "
                "a phase is open to contributions while the scan runs",
                call,
            )
        elif awaited:
            _reject_at(f"self.{attr}.{method}() never waits; do not await it", call)

    def _port_frame_fields(self) -> None:
        if not self._port_frame:
            self._port_frame = True
            self.local_decls += ["int32_t pst;", "uint32_t plen;", "int64_t pval;"]

    def _port_stmt(self, call_info: tuple[_PortDecl, str, ast.Call]) -> list[str]:
        """A statement that is a port call whose value, if any, is unused."""
        decl, method, call = call_info
        self._port_checks(decl, method, call, False)
        if method in ("get", "result"):
            _reject_at(f"the value of self.{decl.attr}.{method}() is unused; assign it", call)
        return self._port_lines(decl, method, call, None)

    def _port_assign(
        self, target: ast.expr, call_info: tuple[_PortDecl, str, ast.Call], stmt: ast.stmt
    ) -> list[str]:
        decl, method, call = call_info
        self._port_checks(decl, method, call, False)
        if method not in ("try_send", "get", "result"):
            _reject_at(f"self.{decl.attr}.{method}() gives no value to assign", call)
        if not isinstance(target, ast.Name):
            _reject_at("a result is assigned to a name", target)
        kind = "i64" if method == "try_send" else decl.elem
        assert kind is not None
        loc = self._new_local(target.id, kind, target)
        return self._port_lines(decl, method, call, loc.cname)

    def _port_lines(
        self, decl: _PortDecl, method: str, call: ast.Call, dest: str | None
    ) -> list[str]:
        self.uses.add("ports")
        attr, name = decl.attr, decl.full
        tgt = self._target()
        ctype = "int64_t" if decl.elem == "i64" else "double"
        err = f"{{ _st = _s; _unw = 1; goto {tgt}; }}"
        if method in ("send", "try_send"):
            self._port_args(decl, method, call, 1)
            value = self._port_value(decl, call.args[0], "the value")
            h = self._port_handle(decl)
            lines = [
                "{",
                f"    {ctype} _pv = {value};",
                "    dftu_port_status _ps = DFTU_PORT_OK;",
                f"    dftu_status _s = _prt->channel_try_send(ctx, host->h, {h}, &_pv, 8, &_ps);",
                f"    if (_s < 0) {err}",
            ]
            if method == "try_send":
                if dest is not None:
                    lines.append(f"    {dest} = (_ps == DFTU_PORT_OK) ? 1 : 0;")
            else:
                here = self._loc_text_plain(call)
                full = _c_str_literal(f"jit.Error [{here}]: channel '{name}' is full")
                closed = _c_str_literal(f"jit.Error [{here}]: channel '{name}' is closed")
                lines.append(
                    "    if (_ps != DFTU_PORT_OK) { _st = dftu_fail(ctx, DFTU_E_INVALID, "
                    f"_ps == DFTU_PORT_FULL ? {full} : {closed}); _unw = 1; goto {tgt}; }}"
                )
            return lines + ["}"]
        if method == "close":
            self._port_args(decl, method, call, 0)
            h = self._port_handle(decl)
            return [
                "{",
                f"    dftu_status _s = _prt->channel_close(ctx, host->h, {h});",
                f"    if (_s < 0) {err}",
                "}",
            ]
        if method == "set":
            self._port_args(decl, method, call, 1)
            value = self._port_value(decl, call.args[0], "the value")
            return [
                "{",
                f"    {ctype} _pv = {value};",
                "    uint64_t _gen = 0;",
                f"    dftu_status _s = _prt->latest_set(ctx, host->h, {_c_str_literal(name)}, &_pv, 8, &_gen);",
                f"    if (_s < 0) {err}",
                "}",
            ]
        if method == "contribute":
            self._port_args(decl, method, call, 1)
            value = self._port_value(decl, call.args[0], "the value")
            ty = "DFTU_T_I64" if decl.elem == "i64" else "DFTU_T_F64"
            assert decl.merge is not None
            sealed = self._port_fail(f"the phase result '{name}' is sealed", call)
            return [
                "{",
                f"    {ctype} _pv = {value};",
                "    dftu_port_status _ps = DFTU_PORT_OK;",
                f"    dftu_status _s = _prt->phase_contribute(ctx, host->h, {_c_str_literal(name)}, 1, "
                f"{_channels.MERGES[decl.merge]}, {ty}, &_pv, 8, &_ps);",
                f"    if (_s < 0) {err}",
                f"    if (_ps != DFTU_PORT_OK) {{ {sealed} }}",
                "}",
            ]
        if method == "seal":
            self._port_args(decl, method, call, 0)
            return [
                "{",
                f"    dftu_status _s = _prt->phase_seal(ctx, host->h, {_c_str_literal(name)}, 1);",
                f"    if (_s < 0) {err}",
                "}",
            ]
        assert dest is not None and method in ("get", "result")
        if method == "get":
            self._port_args(decl, method, call, 1)
            default = self._port_value(decl, call.args[0], "the default")
            read = (
                f"_prt->latest_get(ctx, host->h, {_c_str_literal(name)}, _pb, 8, &_pl, &_gen, &_ps)"
            )
            absent = f"{dest} = {default};"
            blocked = ""
        else:
            if call.keywords or len(call.args) > 1:
                _reject_at(f"self.{attr}.result() takes at most a default value", call)
            default = (
                self._port_value(decl, call.args[0], "the default")
                if call.args
                else ("0" if decl.elem == "i64" else "0.0")
            )
            read = (
                f"_prt->phase_result(ctx, host->h, {_c_str_literal(name)}, 1, _pb, 8, &_pl, &_ps)"
            )
            absent = self._port_fail(
                f"the phase result '{name}' is still open: call seal() first", call
            )
            blocked = f"if (_ps == DFTU_PORT_NOT_READY) {{ {absent} }}"
        odd = self._port_fail(f"'{name}' holds a value that is not an 8-byte int or float", call)
        lines = [
            "{",
            "    unsigned char _pb[8];",
            "    uint32_t _pl = 0;",
            *(["    uint64_t _gen = 0;"] if method == "get" else []),
            "    dftu_port_status _ps = DFTU_PORT_OK;",
            f"    dftu_status _s = {read};",
            f"    if (_s < 0) {err}",
        ]
        if method == "get":
            lines += [
                f"    if (_ps == DFTU_PORT_NOT_READY) {{ {dest} = {default}; }}",
                f"    else if (_ps == DFTU_PORT_OK && _pl == 8) {{ memcpy(&{dest}, _pb, 8); }}",
                f"    else {{ {odd} }}",
            ]
        else:
            lines += [
                f"    {blocked}",
                f"    else if (_ps == DFTU_PORT_OK && _pl == 0) {{ {dest} = {default}; }}",
                f"    else if (_ps == DFTU_PORT_OK && _pl == 8) {{ memcpy(&{dest}, _pb, 8); }}",
                f"    else {{ {odd} }}",
            ]
        return lines + ["}"]

    def _port_wait(self, start: list[str], resume: list[str], node: ast.AST) -> list[str]:
        """The lines of one await on a task the ports service started: start it,
        park the frame, and on resume check the cancel, take the status and run
        ``resume``."""
        self._port_frame_fields()
        self.uses.add("task")
        self._await_n += 1
        k = self._await_n
        outer = self._target()
        cancel = _c_str_literal(
            f"jit.Cancelled [{self._loc_text_plain(node)}]: cancelled at an await"
        )
        return [
            "{",
            *["    " + ln for ln in start],
            f"    if (_w < 0) {{ _st = _w; _unw = 1; goto {outer}; }}",
            "    *n_wait = 1;",
            "    S->tk = wait[0];",
            f"    S->pc = {k};",
            "    return DFTU_OK;",
            "}",
            f"case {k}: ;",
            "{",
            "    if (dftu_check(ctx) < 0) {",
            "        dftu_jit_stat_inc(6);",
            f"        _st = dftu_fail(ctx, DFTU_E_CANCELLED, {cancel}); _unw = 1; goto {outer};",
            "    }",
            "    dftu_status _w = _tsk->status(ctx, host->h, S->tk);",
            "    S->tk = NULL;",
            f"    if (_w < 0) {{ _st = _w; _unw = 1; goto {outer}; }}",
            *["    " + ln for ln in resume],
            "}",
        ]

    def _port_recv_start(self, decl: _PortDecl) -> list[str]:
        h = self._port_handle(decl)
        return [
            "S->pst = DFTU_PORT_OK;",
            "S->plen = 0;",
            "S->pval = 0;",
            "dftu_status _w = _prt->channel_recv_exact(ctx, host->h, "
            f"{h}, &S->pval, 8, &S->plen, &S->pst, &wait[0]);",
        ]

    def _port_await(
        self,
        target: ast.expr | None,
        call_info: tuple[_PortDecl, str, ast.Call],
        stmt: ast.stmt,
    ) -> list[str]:
        decl, method, call = call_info
        self._port_checks(decl, method, call, True)
        if decl.kind != "channel" or method not in ("send", "recv"):
            _reject_at(f"self.{decl.attr}.{method}() is not awaited", call)
        name = decl.full
        here = self._loc_text_plain(stmt)
        self.uses.add("ports")
        if method == "send":
            self._port_args(decl, method, call, 1)
            if target is not None:
                _reject_at("a send gives no value to assign", target)
            ctype = "int64_t" if decl.elem == "i64" else "double"
            value = self._port_value(decl, call.args[0], "the value")
            h = self._port_handle(decl)
            start = [
                "S->pst = DFTU_PORT_OK;",
                f"{ctype} _pv = {value};",
                "memcpy(&S->pval, &_pv, 8);",
                f"dftu_status _w = _prt->channel_send(ctx, host->h, {h}, &S->pval, 8, &S->pst, &wait[0]);",
            ]
            tgt = self._target()
            full = _c_str_literal(f"jit.Error [{here}]: channel '{name}' is full")
            closed = _c_str_literal(f"jit.Error [{here}]: channel '{name}' is closed")
            resume = [
                "if (S->pst != DFTU_PORT_OK) { _st = dftu_fail(ctx, DFTU_E_INVALID, "
                f"S->pst == DFTU_PORT_FULL ? {full} : {closed}); _unw = 1; goto {tgt}; }}"
            ]
            return self._port_wait(start, resume, stmt)
        self._port_args(decl, method, call, 0)
        decode: list[str] = []
        if target is not None:
            if not isinstance(target, ast.Name):
                _reject_at("a result is assigned to a name", target)
            assert decl.elem is not None
            loc = self._new_local(target.id, decl.elem, target)
            decode.append(f"memcpy(&{loc.cname}, &S->pval, 8);")
        tgt = self._target()
        closed_msg = _c_str_literal(f"jit.Closed [{here}]: channel '{name}' is closed and drained")
        start = self._port_recv_start(decl)
        resume = [
            "if (S->pst == DFTU_PORT_CLOSED) {",
            f"    _pclosed = 1; _st = dftu_fail(ctx, DFTU_E_NOT_FOUND, {closed_msg}); _unw = 1; goto {tgt};",
            "}",
            "if (S->pst != DFTU_PORT_OK) { "
            + self._port_fail(
                f"channel '{name}' carried a message that is not an 8-byte int or float", stmt
            )
            + " }",
            *decode,
        ]
        return self._port_wait(start, resume, stmt)

    def _async_for(self, stmt: ast.AsyncFor) -> list[str]:
        if not (self.flow and self.is_async):
            _reject_at("`async for` needs an `async def` @jit.finalize or @jit.task body", stmt)
        if stmt.orelse:
            _reject_at("`async for` with `else`", stmt)
        it = stmt.iter
        decl = (
            self.chans.get(it.attr)
            if isinstance(it, ast.Attribute)
            and isinstance(it.value, ast.Name)
            and it.value.id == self.self_name
            else None
        )
        if decl is None or decl.kind != "channel" or decl.role == "send":
            _reject_at(
                "`async for` takes a receiver or channel of this plugin: "
                "`async for v in self.inp:`",
                it,
            )
        if not isinstance(stmt.target, ast.Name):
            _reject_at("the loop variable of `async for` is one name", stmt.target)
        assert decl.elem is not None
        loc = self._new_local(stmt.target.id, decl.elem, stmt.target)
        self._loop_seq += 1
        self._loops += 1
        self._active_loops.append(self._loop_seq)
        defs, body = self._compile_scope(stmt.body)
        self._active_loops.pop()
        self._loops -= 1
        odd = self._port_fail(
            f"channel '{decl.full}' carried a message that is not an 8-byte int or float", stmt
        )
        self.uses.add("ports")
        start = self._port_recv_start(decl)
        resume = [
            "if (S->pst == DFTU_PORT_CLOSED) break;",
            f"if (S->pst != DFTU_PORT_OK) {{ {odd} }}",
            f"memcpy(&{loc.cname}, &S->pval, 8);",
        ]
        lines = ["for (;;) {"]
        lines += ["    " + ln for ln in self._port_wait(start, resume, stmt)]
        lines += ["    " + ln for ln in defs + body]
        lines.append("}")
        return lines

    def _slice_elts(self, sl: ast.expr) -> list[ast.expr]:
        sl = unwrap_index(sl)
        return builtins.list(sl.elts) if isinstance(sl, ast.Tuple) else [sl]

    # Hoist each expression used more than once in this straight-line scope into
    # a local computed once, then compile the scope's statements against those
    # locals. Nested guard bodies are their own scopes (a guarded expression is
    # not hoisted above its guard, preserving its execution condition).
    def _compile_scope(self, stmts: list[ast.stmt]) -> tuple[list[str], list[str]]:
        outer = self._cse
        self._cse = dict(outer)
        counts: dict[str, int] = {}
        reps: dict[str, ast.expr] = {}
        for stmt in stmts:
            root = stmt.test if isinstance(stmt, ast.If) else stmt
            for node in _unconditional_nodes(root):
                if isinstance(node, ast.expr) and self._is_cse_candidate(node):
                    d = ast.dump(node)
                    counts[d] = counts.get(d, 0) + 1
                    reps.setdefault(d, node)
        cands = [reps[d] for d, c in counts.items() if c >= 2 and d not in self._cse]
        # Define inner (smaller) expressions first so an outer local can name
        # them.
        order = sorted(range(len(cands)), key=lambda i: builtins.sum(1 for _ in ast.walk(cands[i])))
        shared = [cands[i] for i in order]
        defs: list[str] = []
        for node in shared:
            d = ast.dump(node)
            if d in self._cse:
                continue
            self._cur = node
            expr, is_float = self._arith(node)
            name = f"_cse{self._cse_n}"
            self._cse_n += 1
            defs.append(f"{'double' if is_float else 'int64_t'} {name} = ({expr});")
            self._cse[d] = (name, is_float)
        lines: list[str] = []
        for stmt in stmts:
            lines.extend(self._stmt(stmt))
        self._cse = outer
        return defs, lines

    def _mentions_local(self, node: ast.AST) -> bool:
        known = (self.self_name, self.event_name, "jit")
        for sub in ast.walk(node):
            if isinstance(sub, ast.Name) and sub.id not in known and sub.id not in self.ops:
                return True
        return False

    def _is_cse_candidate(self, node: ast.expr) -> bool:
        if not self._cse_ok or self._mentions_local(node):
            return False
        if self._mentions_state(node):
            return False
        if isinstance(node, ast.BinOp) and type(node.op) in ENGINE_ARITH_SYMBOL:
            return True
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
            f = node.func
            if isinstance(f.value, ast.Name):
                if f.value.id == "jit" and f.attr in _PRIM_C:
                    return True
                if f.value.id == self.event_name and f.attr in _ARG_METHODS:
                    return True
        return False

    def _target(self) -> str:
        return self._targets[-1] if self._targets else self._exit_label

    _exit_label = "_ev_exit"

    def _loc(self, node: ast.AST) -> tuple[str, int, int]:
        src = self._source
        assert src is not None
        return (
            src.file,
            src.first + getattr(node, "lineno", 1) - 1,
            getattr(node, "col_offset", 0) + src.margin + 1,
        )

    def _jit_name(self, node: ast.expr) -> str | None:
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id == "jit"
        ):
            return node.attr
        return None

    def _raise(self, stmt: ast.Raise) -> list[str]:
        if stmt.exc is None:
            _reject_at("a bare `raise` re-raises a caught exception; raise jit.Fatal(...)", stmt)
        if stmt.cause is not None:
            _reject_at("`raise ... from ...` is not supported; raise jit.Fatal(...)", stmt)
        call = stmt.exc
        kind = self._jit_name(call.func) if isinstance(call, ast.Call) else None
        if not isinstance(call, ast.Call) or kind is None:
            _reject_at(
                f"raise of {_describe(call)}; a compiled body raises only jit.Fatal(...), "
                "jit.SkipRest() or jit.Cancelled()",
                call,
            )
        if kind not in _RAISABLE:
            _reject_at(
                f"raise of jit.{kind}; a compiled body raises only jit.Fatal(...), "
                "jit.SkipRest() or jit.Cancelled() (jit.Error and its subclasses are what a "
                "failed call raises, and `except` catches them)",
                call,
            )
        if call.keywords or len(call.args) > 1:
            _reject_at(f"jit.{kind} takes at most one string-literal message", call)
        message = kind
        if call.args:
            arg = call.args[0]
            if not (isinstance(arg, ast.Constant) and isinstance(arg.value, str)):
                _reject_at(f"the message of jit.{kind} must be a string literal", arg)
            message = arg.value
        code = _RAISABLE[kind]
        target = self._target()
        if kind == "SkipRest":
            return [f"{{ _unw = 1; goto {target}; }}"]
        file, line, col = self._loc(stmt)
        text = _c_str_literal(f"jit.{kind} [{file}:{line}:{col}]: {message}")
        return [f"{{ _st = dftu_fail(ctx, {code}, {text}); _unw = 1; goto {target}; }}"]

    def _except_cond(self, node: ast.expr) -> str:
        names = (
            [self._jit_name(e) for e in node.elts]
            if isinstance(node, ast.Tuple)
            else [self._jit_name(node)]
        )
        conds: list[str] = []
        for name, at in zip(names, node.elts if isinstance(node, ast.Tuple) else [node]):
            if name is None:
                _reject_at(
                    "`except` names a Python class; catch jit.Error or one of its subclasses "
                    "(jit.NotFound, jit.Closed, jit.IoError, jit.ParseError)",
                    at,
                )
            if name in _UNCATCHABLE:
                _reject_at(
                    f"jit.{name} ends the run and cannot be caught; {_UNCATCHABLE[name]}", at
                )
            if name == "Closed" and not (self.flow and self.is_async):
                _reject_at(
                    "jit.Closed comes from `await self.<channel>.recv()`; catch it in an async "
                    "@jit.finalize or @jit.task body",
                    at,
                )
            cond = _CATCH_COND.get(name)
            if cond is None:
                _reject_at(
                    f"`except jit.{name}`; catch jit.Error or one of its subclasses "
                    "(jit.NotFound, jit.IoError, jit.ParseError)",
                    at,
                )
            conds.append(cond)
        return " || ".join(conds)

    def _try(self, stmt: ast.Try) -> list[str]:
        if stmt.orelse or stmt.finalbody:
            _reject_at("`try` with `else` or `finally`; use try and except only", stmt)
        if not stmt.handlers:
            _reject_at("`try` needs an `except` clause", stmt)
        n = self._label_n
        self._label_n += 1
        catch, end = f"_catch{n}", f"_tend{n}"
        self._targets.append(catch)
        defs, body = self._compile_scope(stmt.body)
        self._targets.pop()
        outer = self._target()
        lines = ["{"]
        lines.append("    {")
        lines.extend("        " + ln for ln in defs + body)
        lines.append("    }")
        lines += [f"    goto {end};", f"{catch}:", "    if (_unw) {"]
        branch = "if"
        for handler in stmt.handlers:
            if handler.type is None:
                _reject_at("a bare `except` catches everything; catch jit.Error", handler)
            if handler.name is not None:
                _reject_at(
                    "`except ... as name` is not supported; the handler cannot read it", handler
                )
            cond = self._except_cond(handler.type)
            hdefs, hbody = self._compile_scope(handler.body)
            lines.append(f"        {branch} (_st < 0 && ({cond})) {{")
            lines.append("            _unw = 0;")
            lines.append("            _st = DFTU_OK;")
            if self.flow:
                lines.append("            _pclosed = 0;")
            lines.extend("            " + ln for ln in hdefs + hbody)
            lines.append("        }")
            branch = "else if"
        lines += [f"        else goto {outer};", "    }", f"{end}: ;", "}"]
        return lines

    def _stmt(self, stmt: ast.stmt) -> list[str]:
        self._cur = stmt
        if isinstance(stmt, ast.Raise):
            return self._raise(stmt)
        if isinstance(stmt, ast.Try):
            return self._try(stmt)
        if (self._targets or self.flow) and self.stores:
            self._store_read = False
            lines = self._stmt_plain(stmt)
            if self._store_read:
                lines.append(
                    f"if (_se.st < 0) {{ _st = _se.st; _se.st = DFTU_OK; _unw = 1; "
                    f"goto {self._target()}; }}"
                )
            return lines
        return self._stmt_plain(stmt)

    def _stmt_plain(self, stmt: ast.stmt) -> list[str]:
        if isinstance(stmt, ast.If):
            guard = self._cond(stmt.test)
            defs, body = self._compile_scope(stmt.body)
            lines = [f"if ({guard}) {{"]
            lines.extend("    " + ln for ln in defs)
            lines.extend("    " + ln for ln in body)
            if not stmt.orelse:
                lines.append("}")
                return lines
            if len(stmt.orelse) == 1 and isinstance(stmt.orelse[0], ast.If):
                chained = self._stmt(stmt.orelse[0])
                lines.append("} else " + chained[0])
                lines.extend(chained[1:])
                return lines
            edefs, ebody = self._compile_scope(stmt.orelse)
            lines.append("} else {")
            lines.extend("    " + ln for ln in edefs)
            lines.extend("    " + ln for ln in ebody)
            lines.append("}")
            return lines
        return self._add_stmt(stmt)

    def _add_stmt(self, stmt: ast.stmt) -> list[str]:
        if self.plain:
            plain_lines = self._plain_stmt(stmt)
            if plain_lines is not None:
                return plain_lines
        flow_lines = self._flow_stmt(stmt)
        if flow_lines is not None:
            return flow_lines
        if isinstance(stmt, ast.AugAssign):
            return self._aug(stmt)
        if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call):
            if self.stores:
                store_lines = self._store_put(stmt.value)
                if store_lines is not None:
                    return store_lines
            func = stmt.value.func
            if isinstance(func, ast.Attribute) and func.attr == "append":
                return self._append(stmt.value)
            return self._observe(stmt.value)
        if isinstance(stmt, (ast.Assign, ast.AnnAssign)) and stmt.value is not None:
            attr = self._plain_attr_in(stmt.value)
            if attr is not None:
                _reject_at(
                    f"self.{attr} is scan state and is write-only in the body: each worker "
                    "holds only its own slice, so a read would see a partial value",
                    stmt.value,
                )
        _reject(_describe(stmt), stmt)

    def _cond(self, node: ast.expr) -> str:
        """C text for a per-event condition: ``and`` / ``or`` / ``not``,
        comparisons (chained, ``in`` a literal tuple, ``is None``) and a bare
        flag or number as its own truth value."""
        if isinstance(node, ast.BoolOp):
            sym = "&&" if isinstance(node.op, ast.And) else "||"
            return "(" + f" {sym} ".join(self._cond(v) for v in node.values) + ")"
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return f"!({self._cond(node.operand)})"
        if isinstance(node, ast.Compare):
            parts = []
            left = node.left
            for op, right in zip(node.ops, node.comparators):
                parts.append(self._cmp_pair(left, op, right, node))
                left = right
            return parts[0] if len(parts) == 1 else "(" + " && ".join(parts) + ")"
        if isinstance(node, ast.NamedExpr):
            _reject("assignment expression (:=)", node)
        flag = self._cfg_field(node)
        if flag is not None and flag.kind == "bool":
            return f"_cfg_{flag.name} != 0"
        if self._is_str_element(node) or self._is_str_literal(node):
            _reject_at("a string is not a truth value; compare it with ==, != or `is None`", node)
        if (
            isinstance(node, ast.Attribute)
            and node.attr == "has_dur"
            and self._is_event(node.value)
        ):
            return self._field("has_dur")
        expr, _ = self._arith(node)
        return f"({expr}) != 0"

    def _is_event(self, node: ast.expr) -> bool:
        return isinstance(node, ast.Name) and node.id == self.event_name

    def _cmp_pair(self, left: ast.expr, op: ast.cmpop, right: ast.expr, whole: ast.expr) -> str:
        op_t = type(op)
        if isinstance(op, (ast.Is, ast.IsNot)):
            return self._is_none(left, right, isinstance(op, ast.Is), whole)
        if isinstance(op, (ast.In, ast.NotIn)):
            if not isinstance(right, (ast.Tuple, ast.List, ast.Set)):
                _reject_at("`in` needs a literal tuple, list or set", right)
            if not builtins.all(isinstance(e, ast.Constant) for e in right.elts):
                _reject_at("`in` takes only literals", right)
            if not right.elts:
                return "0" if isinstance(op, ast.In) else "1"
            alts = [self._cmp_pair(left, ast.Eq(), e, whole) for e in right.elts]
            joined = "(" + " || ".join(alts) + ")"
            return joined if isinstance(op, ast.In) else f"!{joined}"
        sym = COMPARE_SYMBOL.get(op_t)
        if sym is None:
            _reject("comparison operator", whole)
        str_cmp = self._str_compare(left, right, op_t)
        if str_cmp is not None:
            return str_cmp
        lexpr, _ = self._arith(left)
        rexpr, _ = self._arith(right)
        return f"{lexpr} {sym} {rexpr}"

    def _is_none(self, left: ast.expr, right: ast.expr, positive: bool, whole: ast.expr) -> str:
        if not (isinstance(right, ast.Constant) and right.value is None):
            _reject("`is` comparison (only `is None` / `is not None`)", whole)
        opt = self._cfg_field(left)
        if opt is not None:
            if not opt.optional:
                _reject_at(f"self.config.{opt.name} is not optional, so it is never None", left)
            return f"!_has_{opt.name}" if positive else f"_has_{opt.name}"
        operand = self._str_operand(left)
        if operand is not None:
            return f"{operand} {'==' if positive else '!='} DFTU_STR_NONE"
        _reject_at(
            "`is None` applies to an optional self.config field, a string field such as "
            "e.fhash or e.arg_str(...); a numeric field is never None",
            left,
        )

    def _is_none_sentinel(self, node: ast.expr) -> bool:
        return (
            isinstance(node, ast.Attribute)
            and node.attr == "NONE"
            and isinstance(node.value, ast.Name)
            and node.value.id == "jit"
        )

    def _str_compare(self, a: ast.expr, b: ast.expr, op_t: type) -> str | None:
        a_none = self._is_none_sentinel(a)
        b_none = self._is_none_sentinel(b)
        a_str = a_none or self._is_str_literal(a) or self._is_str_element(a)
        b_str = b_none or self._is_str_literal(b) or self._is_str_element(b)
        if not a_str and not b_str:
            return None
        if a_str != b_str:
            _reject(
                "comparison between a string and a number (a string field compares only with a string)",
                a,
            )
        if op_t not in (ast.Eq, ast.NotEq):
            _reject("string comparison operator other than == / !=", a)
        if (self._is_view_node(a) or self._is_view_node(b)) and not (a_none or b_none):
            eq = f"dftu_jit_veq({self._view_expr(a)}, {self._view_expr(b)})"
            return eq if op_t is ast.Eq else f"!{eq}"
        sides = []
        for side, is_none in ((a, a_none), (b, b_none)):
            if is_none:
                sides.append("DFTU_STR_NONE")
                continue
            cfg = self._cfg_field(side)
            if cfg is not None:
                sides.append(f"_cfgs_{cfg.name}")
            elif isinstance(side, ast.Constant):
                assert isinstance(side.value, str)
                sides.append(self._str_literal(side.value))
            else:
                operand = self._str_operand(side)
                if operand is None:
                    _reject_at("a string compared with something that is not a string field", side)
                sides.append(operand)
        return f"{sides[0]} {COMPARE_SYMBOL[op_t]} {sides[1]}"

    def _is_str_literal(self, node: ast.expr) -> bool:
        """A string literal, or a str config field (an interned constant)."""
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return True
        cfg = self._cfg_field(node)
        return cfg is not None and cfg.kind == "str"

    def _str_operand(self, node: ast.expr) -> str | None:
        if isinstance(node, ast.Name) and node.id in self.locals:
            loc = self._local(node)
            if loc.kind == "view":
                return f"dftu_jit_vid(host, &{loc.cname})"
            return loc.cname if loc.kind == "str" else None
        vc = self._val_call(node)
        if vc is not None:
            return vc[1] if vc[0] == "str" else None
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id == self.event_name
            and node.attr in _STR_FIELDS
        ):
            return self._field(node.attr)
        arg = self._arg_call(node)
        if arg is not None and arg[0] == "arg_str":
            expr, _ = self._lower_arg(*arg)
            return expr
        return None

    def _arg_call(self, node: ast.expr) -> tuple[str, str] | None:
        """(method, arg-name) for an ``e.arg_*("NAME")`` call; None if not an event
        method call, JitError for an unknown method or a non-literal name."""
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == self.event_name
        ):
            return None
        method = node.func.attr
        if method not in _ARG_METHODS:
            _reject(f"event method e.{method}(...); use arg_i64 / arg_f64 / arg_str", node)
        if len(node.args) != 1 or node.keywords:
            _reject(f"e.{method} takes exactly one string-literal arg name", node)
        name_node = node.args[0]
        if not (isinstance(name_node, ast.Constant) and isinstance(name_node.value, str)):
            _reject(f"e.{method} arg name must be a string literal", name_node)
        return method, name_node.value

    def _prim_call(self, node: ast.expr) -> tuple[str, str, list[ast.expr]] | None:
        """(c-function, arg-cast, arg-nodes) for a ``jit.<prim>(...)`` numeric
        primitive; None when not a ``jit.`` call at all, JitError for a bad
        prim or arity."""
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "jit"
        ):
            return None
        prim = node.func.attr
        spec = _PRIM_C.get(prim)
        if spec is None:
            _reject(f"jit.{prim}(...) in an expression; expected a numeric primitive", node)
        c_fn, arity, cast = spec
        if len(node.args) != arity or node.keywords:
            _reject(f"jit.{prim}() takes exactly {arity} argument(s)", node)
        return c_fn, cast, builtins.list(node.args)

    def _op_call(self, node: ast.expr) -> tuple[str, bool] | None:
        """(c-expr, is_float) for a call to a referenced ``@jit.op`` transform,
        inlining its C body on first use; None when not an op call. A
        ``self.<store>.get(...)`` call is lowered here too."""
        if self.stores:
            store_get = self._store_get(node)
            if store_get is not None:
                return store_get
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id in self.ops
        ):
            return None
        name = node.func.id
        op_obj = self.ops[name]
        if len(node.args) != 1 or node.keywords:
            _reject(f"jit.op '{name}' takes exactly one positional argument", node)
        c_name = f"dftu_jitop_{name}"
        if name not in self.op_emitted:
            self.op_defs.append(_jit_op.emit_inline(op_obj, c_name))
            self.op_emitted.add(name)
        arg_expr, _ = self._arith(node.args[0])
        in_ct = _jit_op.c_type(op_obj.in_type)
        return f"{c_name}(({in_ct})({arg_expr}))", _jit_op.is_float_type(op_obj.out_type)

    def _lower_arg(self, method: str, name: str) -> tuple[str, bool]:
        """Lower ``e.arg_*("NAME")`` to a read of the batch-resolved "args.NAME"
        dyn column, returning (expr, is_float); the column is resolved once per
        batch in on_batch, by name, not re-scanned per row."""
        self.arg_helpers.add(method)
        if name not in self.arg_keys:
            self.arg_keys.append(name)
        col = f"_argcol{self.arg_keys.index(name)}"
        if method == "arg_str":
            return f"dftu_jit_arg_str(host, &{col}, i)", False
        fn, _dflt = _ARG_METHODS[method]
        return f"{fn}(&{col}, i)", method == "arg_f64"

    def _str_literal(self, s: str) -> str:
        if self.flow:
            return f"dftu_jit_lit(host, {_c_str_literal(s)}, {len(s.encode('utf-8'))})"
        if s not in self.str_literals:
            self.str_literals.append(s)
        return f"lit_{self.str_literals.index(s)}"

    def _resolve_accum(self, target: ast.expr) -> tuple[str, list[ast.expr], int, Monoid]:
        comp_name: str | None = None
        node = target
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Subscript):
            comp_name = node.attr
            node = node.value
        if not isinstance(node, ast.Subscript):
            _reject("assignment target (expected self.<map>[(...)])", target)
        subs: list[ast.expr] = []
        cur: ast.expr = node
        while isinstance(cur, ast.Subscript):
            subs.append(unwrap_index(cur.slice))
            cur = cur.value
        subs.reverse()
        base = cur
        if not (
            isinstance(base, ast.Attribute)
            and isinstance(base.value, ast.Name)
            and base.value.id == self.self_name
        ):
            _reject("assignment target (expected self.<map>[(...)])", target)
        decl = self.maps.get(base.attr)
        if decl is None:
            _reject(f"unknown map self.{base.attr}", base)
        return self._resolve_flat(base.attr, decl, subs, comp_name)

    def _resolve_flat(
        self, attr: str, decl: AnyMap, subs: list[ast.expr], comp_name: str | None
    ) -> tuple[str, list[ast.expr], int, Monoid]:
        if decl.is_product:
            keys = self._keys(subs[0])
            comp_node = subs[1] if len(subs) > 1 else None
            if len(subs) > 2:
                _reject(f"too many subscripts for product map self.{attr}", subs[2])
            if comp_name is not None:
                comp = self._named_comp(comp_name, decl, attr)
            elif comp_node is not None:
                comp = self._comp(comp_node, decl, attr)
            else:
                _reject(f"product map self.{attr} add without a component", subs[0])
        else:
            if comp_name is not None:
                _reject(f"single-value map self.{attr} add with a component", subs[0])
            keys = []
            for sl in subs:
                keys.extend(self._keys(sl))
            comp = 0
        if len(keys) != len(decl.key_types):
            _reject(
                f"self.{attr} takes {len(decl.key_types)} key components, got {len(keys)}", subs[0]
            )
        return attr, keys, comp, decl.values[comp]

    def _publish_target(self, target: ast.expr) -> str | None:
        if (
            isinstance(target, ast.Attribute)
            and isinstance(target.value, ast.Name)
            and target.value.id == self.self_name
            and target.attr in self.ports
        ):
            return target.attr
        return None

    def _publish(self, name: str, value: ast.expr) -> list[str]:
        port = self.ports[name]
        if port.role != "publish":
            _reject(f"self.{name} is a jit.consume port; read its value, do not += into it", value)
        expr, saw_float = self._arith(value)
        if not port.is_f64 and saw_float:
            _reject("float value into a u64 publish port", value)
        cast = "double" if port.is_f64 else "uint64_t"
        return [f"_pub_{name} += ({cast})({expr});"]

    def _consume_ref(self, name: str) -> str:
        port = self.ports[name]
        if port.role != "consume":
            _reject(f"self.{name} is a jit.publish port; publish into it with +=, do not read it")
        return f"_sub_{name}"

    def _aug(self, stmt: ast.AugAssign) -> list[str]:
        if not isinstance(stmt.op, ast.Add):
            _reject("augmented operator other than +=", stmt)
        pub = self._publish_target(stmt.target)
        if pub is not None:
            return self._publish(pub, stmt.value)
        attr, keys, comp, monoid = self._resolve_accum(stmt.target)
        if monoid.dft in _LIST_OPS:
            _reject("+= on an ordered-list monoid; use .append(elem, order_by=<expr>)", stmt)
        if monoid.dft not in _ADDITIVE_OPS:
            _reject(f"+= on the non-additive monoid {monoid.dft}; use .observe(v) instead", stmt)
        return self._emit_row(attr, keys, comp, monoid, stmt.value, None)

    def _observe(self, call: ast.Call) -> list[str]:
        func = call.func
        if not (isinstance(func, ast.Attribute) and func.attr == "observe"):
            _reject("method call other than .observe(v)", call)
        attr, keys, comp, monoid = self._resolve_accum(func.value)
        if monoid.dft in _LIST_OPS:
            _reject(".observe on an ordered-list monoid; use .append(elem, order_by=<expr>)", call)
        by: ast.expr | None = None
        if monoid.needs_by:
            if len(call.args) != 1:
                _reject(".observe takes one payload and by=<expr>", call)
            for kw in call.keywords:
                if kw.arg == "by":
                    by = kw.value
                else:
                    _reject(f".observe keyword other than by ({kw.arg})", kw.value)
            if by is None:
                _reject_at(
                    f"self.{attr} .observe requires by=<expr>; the by key ranks the payloads", call
                )
        elif len(call.args) != 1 or call.keywords:
            _reject(".observe expects exactly one value argument", call)
        self._check_element(call.args[0], monoid)
        return self._emit_row(attr, keys, comp, monoid, call.args[0], by)

    def _append(self, call: ast.Call) -> list[str]:
        func = call.func
        assert isinstance(func, ast.Attribute)
        if len(call.args) != 1:
            _reject(".append expects exactly one element argument", call)
        order: ast.expr | None = None
        for kw in call.keywords:
            if kw.arg == "order_by":
                order = kw.value
            else:
                _reject(f".append keyword other than order_by ({kw.arg})", kw.value)
        attr, keys, comp, monoid = self._resolve_accum(func.value)
        if monoid.dft not in _LIST_OPS:
            _reject(f".append on the non-list monoid {monoid.dft}; use += or .observe(v)", call)
        if order is None:
            _reject_at(
                "ordered list requires order_by=<expr>; order-by-arrival is not parallel-safe", call
            )
        self._check_element(call.args[0], monoid)
        return self._emit_row(attr, keys, comp, monoid, call.args[0], order)

    def _check_element(self, node: ast.expr, monoid: Monoid) -> None:
        if monoid.elem not in ("str", "i64"):
            return
        if monoid.dft not in (_SET_OPS | _LIST_OPS | _ARGBY_OPS | _TOPK_OPS) and monoid.dft not in (
            "DFTU_AGG_APPROX_TOPK",
            "DFTU_AGG_SAMPLE",
        ):
            return
        is_str = self._is_str_element(node)
        if monoid.elem == "str" and not is_str:
            _reject_at(
                "a str-valued element must be a string field such as e.name; "
                "use of=jit.i64 for an int64 value",
                node,
            )
        if monoid.elem == "i64" and is_str:
            _reject_at(
                "an i64-valued element must be an int64 expr such as e.dur; "
                "use of=jit.str_ for a string value",
                node,
            )

    def _is_str_element(self, node: ast.expr) -> bool:
        if isinstance(node, ast.Name) and node.id in self.locals:
            return self.locals[node.id].kind in ("str", "view")
        vc = self._val_call(node)
        if vc is not None:
            return vc[0] == "str"
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id == self.event_name
            and node.attr in _STR_FIELDS
        ):
            return True
        arg = self._arg_call(node)
        return arg is not None and arg[0] == "arg_str"

    def _emit_row(
        self,
        attr: str,
        keys: list[ast.expr],
        comp: int,
        monoid: Monoid,
        value: ast.expr,
        by: ast.expr | None,
    ) -> list[str]:
        """Append one contribution row to ``attr``'s per-batch column buffers;
        the buffers are folded into the DFTU_SVC_AGG accumulator after the loop."""
        decl = self.maps[attr]
        if monoid.elem != "str" and monoid.dft != "DFTU_AGG_DISTINCT":
            self._check_family(value, "i64", "this value")
        self.rows_per_event[attr] = self.rows_per_event.get(attr, 0) + 1
        lines = ["{"]
        if self._has_loops:
            lines += self._grow_rows(attr, decl)
        lines.append(f"    uint32_t _r = _n_{attr}++;")
        for idx, kn in enumerate(keys):
            ktype = decl.key_types[idx].dft
            if ktype == "DFTU_T_BYTES":
                _reject("a bytes key; e.<field> exposes no raw byte blob, author it raw")
            slot = f"_k{idx}_{attr}[_r]"
            if ktype == "DFTU_T_STR":
                lines.append(f"    {slot} = (dftu_str)({self._value(kn)});")
            elif is_float_dft(ktype):
                expr, _ = self._arith(kn)
                lines.append(f"    {slot} = (double)({expr});")
            else:
                lines.append(f"    {slot} = (int64_t)({self._value(kn)});")
        if decl.is_product:
            for c, m in enumerate(decl.values):
                lines.append(f"    _v{c}_{attr}[_r] = ({kind_type(m.elem).ctype})0;")
                lines.append(f"    _o{c}_{attr}[_r] = 0;")
                if m.needs_by:
                    lines.append(f"    _b{c}_{attr}[_r] = 0.0;")
            lines.append(f"    _o{comp}_{attr}[_r] = 1;")
        lines.append(f"    _v{comp}_{attr}[_r] = {self._rhs(value, monoid.elem)};")
        if by is not None:
            by_expr, _ = self._arith(by)
            lines.append(f"    _b{comp}_{attr}[_r] = (double)({by_expr});")
        lines.append("}")
        return lines

    def _grow_rows(self, attr: str, decl: AnyMap) -> list[str]:
        """Room for one more row of ``attr``: a loop appends an unknown number of
        rows per event, so the batch-sized buffers grow when they fill."""
        bufs: list[tuple[str, str]] = []
        for idx, kt in enumerate(decl.key_types):
            bufs.append((f"_k{idx}_{attr}", key_type(kt.dft).ctype))
        for c, m in enumerate(decl.values):
            bufs.append((f"_v{c}_{attr}", kind_type(m.elem).ctype))
            if decl.is_product:
                bufs.append((f"_o{c}_{attr}", "uint8_t"))
            if m.needs_by:
                bufs.append((f"_b{c}_{attr}", "double"))
        fail = (
            f'{{ _st = dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory"); _unw = 1; '
            f"goto {self._target()}; }}"
        )
        lines = [
            f"    if (_n_{attr} >= _cap_{attr}) {{",
            f"        if (_cap_{attr} > 0x3fffffffu) {fail}",
            f"        uint32_t _nc = _cap_{attr} ? _cap_{attr} * 2u : 64u;",
            "        void* _gp;",
        ]
        for name, ctype in bufs:
            lines += [
                f"        _gp = realloc({name}, sizeof({ctype}) * (size_t)_nc);",
                f"        if (!_gp) {fail}",
                f"        {name} = ({ctype}*)_gp;",
            ]
        lines += [f"        _cap_{attr} = _nc;", "    }"]
        return lines

    def _comp(self, node: ast.expr, decl: AnyMap, attr: str) -> int:
        node = unwrap_index(node)
        if (
            not isinstance(node, ast.Constant)
            or not isinstance(node.value, int)
            or isinstance(node.value, bool)
        ):
            _reject("component index (expected an integer literal)", node)
        comp = node.value
        if comp < 0 or comp >= len(decl.values):
            _reject(f"component index {comp} out of range for self.{attr}", node)
        return comp

    def _named_comp(self, name: str, decl: AnyMap, attr: str) -> int:
        names = decl.value_names
        if names is None or name not in names:
            _reject(f"unknown value component .{name} for self.{attr}")
        return names.index(name)

    def _keys(self, sl: ast.AST) -> list[ast.expr]:
        sl = unwrap_index(sl)
        if isinstance(sl, ast.Tuple):
            return builtins.list(sl.elts)
        return [sl]

    def _value(self, node: ast.expr) -> str:
        node = unwrap_index(node)
        hoisted = self._cse.get(ast.dump(node))
        if hoisted is not None:
            return hoisted[0]
        if isinstance(node, ast.Name):
            loc = self._local(node)
            if loc.kind == "val":
                _reject_at(f"{node.id} is a JSON value; read it with as_int, as_str, ...", node)
            if loc.kind == "view":
                return f"dftu_jit_vid(host, &{loc.cname})"
            return loc.cname
        vc = self._val_call(node)
        if vc is not None:
            if vc[0] == "val":
                _reject_at(
                    "a JSON value is not a number or a string; read it with as_int, as_str, ...",
                    node,
                )
            return vc[1]
        opc = self._op_call(node)
        if opc is not None:
            return opc[0]
        prim = self._prim_call(node)
        if prim is not None:
            c_fn, cast, arg_nodes = prim
            parts = [f"({cast})({self._value(a)})" for a in arg_nodes]
            return f"{c_fn}({', '.join(parts)})"
        arg = self._arg_call(node)
        if arg is not None:
            expr, _ = self._lower_arg(*arg)
            return expr
        cfg = self._cfg_field(node)
        if cfg is not None:
            return f"_cfgs_{cfg.name}" if cfg.kind == "str" else f"_cfg_{cfg.name}"
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
            if node.value.id == self.self_name and node.attr in self.ports:
                return self._consume_ref(node.attr)
            if node.value.id == self.event_name:
                return self._field(node.attr)
            if node.attr == "NONE":
                return "DFTU_STR_NONE"
        if isinstance(node, ast.Constant):
            return self._const(node)
        self._reject_expr(node)

    def _rhs(self, node: ast.expr, elem: str) -> str:
        """Cast an expression into a value buffer of family ``elem``."""
        if elem == "str":
            return f"(dftu_str)({self._value(node)})"
        expr, saw_float = self._arith(node)
        if elem != "f64" and saw_float:
            _reject("float value into a u64 monoid component", node)
        return f"({'double' if elem == 'f64' else 'int64_t'})({expr})"

    def _arith(self, node: ast.expr) -> tuple[str, bool]:
        node = unwrap_index(node)
        hoisted = self._cse.get(ast.dump(node))
        if hoisted is not None:
            return hoisted[0], hoisted[1]
        if isinstance(node, ast.BinOp):
            sym = ENGINE_ARITH_SYMBOL.get(type(node.op))
            if sym is None:
                _reject("arithmetic operator (only + - * / are allowed)", node)
            for operand in (node.left, node.right):
                if self._is_str_element(operand):
                    _reject_at("a string field cannot be an arithmetic operand", operand)
            left, lf = self._arith(node.left)
            right, rf = self._arith(node.right)
            if isinstance(node.op, ast.Div):
                return f"((double)({left}) / (double)({right}))", True
            return f"({left} {sym} {right})", lf or rf
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.USub, ast.UAdd)):
            inner, inner_float = self._arith(node.operand)
            return (f"(-{inner})" if isinstance(node.op, ast.USub) else inner), inner_float
        if isinstance(node, ast.Name):
            loc = self._local(node)
            if loc.kind in ("i64", "f64"):
                return loc.cname, loc.kind == "f64"
            _reject_at(
                f"{node.id} is a {_KIND_WORD[loc.kind]} and cannot be used as a number", node
            )
        vc = self._val_call(node)
        if vc is not None:
            if vc[0] in ("i64", "f64"):
                return vc[1], vc[0] == "f64"
            _reject_at(f"a {_KIND_WORD[vc[0]]} cannot be used as a number", node)
        length = self._len_call(node)
        if length is not None:
            return length, False
        opc = self._op_call(node)
        if opc is not None:
            return opc
        prim = self._prim_call(node)
        if prim is not None:
            c_fn, cast, arg_nodes = prim
            parts = [f"({cast})({self._arith(a)[0]})" for a in arg_nodes]
            return f"{c_fn}({', '.join(parts)})", False
        arg = self._arg_call(node)
        if arg is not None:
            return self._lower_arg(*arg)
        cfg = self._cfg_field(node)
        if cfg is not None:
            if cfg.kind == "str":
                _reject(f"string config field self.config.{cfg.name} in arithmetic", node)
            return f"_cfg_{cfg.name}", cfg.kind == "float"
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
            if node.value.id == self.self_name and node.attr in self.ports:
                port = self.ports[node.attr]
                return self._consume_ref(node.attr), port.is_f64
            if node.value.id == self.event_name:
                return self._field(node.attr), False
        if isinstance(node, ast.Constant):
            v = node.value
            if isinstance(v, bool):
                _reject("boolean literal", node)
            if isinstance(v, int):
                return int_literal(v), False
            if isinstance(v, float):
                return repr(v), True
            _reject("literal (only integer or float literals are allowed)", node)
        self._reject_expr(node)

    def _const(self, node: ast.Constant) -> str:
        v = node.value
        if isinstance(v, bool):
            _reject("boolean literal", node)
        if isinstance(v, int):
            return int_literal(v)
        if isinstance(v, float):
            _reject("float literal", node)
        _reject("literal (only integer literals are allowed)", node)

    def _field(self, attr: str) -> str:
        expr = _FIELD_EXPR.get(attr)
        if expr is None:
            _reject(f"event field e.{attr}")
        self.fields_used.add(_FIELD_COLUMN[attr])
        return expr
