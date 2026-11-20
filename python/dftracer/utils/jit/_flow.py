# SPDX-License-Identifier: MIT
"""The authoring surface of the flow constructs of ``@jit.plugin``: tasks and the
finalize step (``async`` / ``await``), files (``with jit.open``) and JSON / YAML
values (``jit.json``, ``jit.yaml``).

Every name here is a marker. The compiler in ``dftracer.utils.jit`` lowers the
body that uses it to C against the plugin ABI; calling one in plain Python raises
:class:`~dftracer.utils.jit.JitError`, as for the numeric primitives.
"""

from __future__ import annotations

import ast
from typing import (
    Awaitable,
    Callable,
    ContextManager,
    Iterator,
    NoReturn,
    Protocol,
    TypeVar,
    overload,
)

from .._jit_errors import JitError
from ._channels import (
    PortDecl,
    close_without_finalize,
    root_close_lines,
    root_fields,
    root_open_lines,
)

_AF = TypeVar("_AF", bound=Callable[..., Awaitable[object]])
_FF = TypeVar("_FF", bound=Callable[..., object])
_T = TypeVar("_T")
_T1 = TypeVar("_T1")
_T2 = TypeVar("_T2")
_T3 = TypeVar("_T3")
_T4 = TypeVar("_T4")

TASK_ATTR = "_jit_task"
FINALIZE_ATTR = "_jit_finalize"


def task(fn: _AF) -> _AF:
    """Mark an ``async def`` method of a :func:`~dftracer.utils.jit.plugin` as a
    task body. It takes ``int`` and ``float`` parameters, returns an ``int``, a
    ``float`` or nothing, and is started by ``await self.<task>(args)`` (or as an
    argument of :func:`all` and :func:`any`) from a :func:`finalize` body or
    another task. The compiler lowers it to a ``dftu_step_fn`` state machine."""
    setattr(fn, TASK_ATTR, True)
    return fn


def finalize(fn: _FF) -> _FF:
    """Mark the one method of a :func:`~dftracer.utils.jit.plugin` that runs
    when the scan ends, on the merged result: ``def`` runs to the end at once,
    ``async def`` may ``await`` (see :func:`sleep`, :func:`all`, :func:`any`
    and a task). Results leave through :func:`emit`."""
    setattr(fn, FINALIZE_ATTR, True)
    return fn


def _marker(name: str) -> NoReturn:
    raise JitError(
        f"jit.{name} is a plugin construct; use it inside a @jit.finalize, @jit.task or "
        "@jit.each_event body, where it lowers to C."
    )


def sleep(seconds: float) -> Awaitable[None]:
    """``await jit.sleep(s)``: wait ``s`` seconds, then go on. A cancelled task
    stops at the ``await``."""
    _marker("sleep")


@overload
def all(a: Awaitable[_T1], b: Awaitable[_T2], /) -> Awaitable[tuple[_T1, _T2]]: ...
@overload
def all(
    a: Awaitable[_T1], b: Awaitable[_T2], c: Awaitable[_T3], /
) -> Awaitable[tuple[_T1, _T2, _T3]]: ...
@overload
def all(
    a: Awaitable[_T1], b: Awaitable[_T2], c: Awaitable[_T3], d: Awaitable[_T4], /
) -> Awaitable[tuple[_T1, _T2, _T3, _T4]]: ...
def all(*tasks: Awaitable[object]) -> Awaitable[tuple[object, ...]]:
    """``a, b = await jit.all(self.t1(), self.t2())``: run the tasks at once and
    give their results in order. The first failure cancels the others and is the
    result."""
    _marker("all")


def any(first: Awaitable[_T], second: Awaitable[_T], /, *rest: Awaitable[_T]) -> Awaitable[_T]:
    """``x = await jit.any(self.t1(), self.t2())``: run the tasks at once; the
    first to finish decides (its result, or its failure) and the others are
    cancelled. The tasks share one return type."""
    _marker("any")


def emit(name: str, value: int | float | str, /) -> None:
    """Emit the named result ``name`` of the run: the text of ``value`` as bytes
    (an ``int`` in decimal, a ``float`` with 17 significant digits, a ``str`` as
    UTF-8). Only a :func:`finalize` body emits."""
    _marker("emit")


class File(Protocol):
    """A file opened by :func:`open`, valid inside its ``with`` block.

    ``for line in f`` gives the text lines without their ``\\n`` (a gzip file is
    inflated); ``f.read(n)`` gives the next ``n`` raw bytes of the file as a
    string, and ``""`` at its end. One handle uses one of the two."""

    def __iter__(self) -> Iterator[str]: ...
    def read(self, n: int, /) -> str: ...


def open(path: str, /) -> ContextManager[File]:
    """``with jit.open(path) as f:`` opens a file for reading. ``path`` is a
    string literal or a ``str`` config field. The file closes on every way out of
    the block, a ``raise`` included. A missing file raises
    :class:`~dftracer.utils.jit.NotFound` with the path."""
    _marker("open")


class Value(Protocol):
    """A JSON or YAML value, valid in the body that parsed it.

    ``find`` of a missing key, of a non-object and of a value of the wrong kind
    all give a value for which ``is_null()`` is true; ``as_*`` give the default
    for a null, a missing or a mistyped value (a JSON number converts between
    ``int`` and ``float``, a ``bool`` is an ``int``, a YAML scalar is text)."""

    def find(self, key: str, /) -> Value: ...
    def as_int(self, default: int = 0, /) -> int: ...
    def as_float(self, default: float = 0.0, /) -> float: ...
    def as_str(self, default: str = "", /) -> str: ...
    def as_bool(self, default: bool = False, /) -> bool: ...
    def is_null(self) -> bool: ...
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[Value]: ...
    def items(self) -> Iterator[tuple[str, Value]]: ...


def json(text: str, /) -> Value:
    """Parse JSON text, such as a line read from a file. A parse error raises
    :class:`~dftracer.utils.jit.ParseError` with the line and column."""
    _marker("json")


def yaml(text: str, /) -> Value:
    """Parse YAML text. A scalar is text, so ``as_int`` of ``"7"`` is 0: read it
    with ``as_str`` first. A parse error raises
    :class:`~dftracer.utils.jit.ParseError` with the line and column."""
    _marker("yaml")


# ---- C helpers emitted into a plugin that uses the flow constructs ----------

# A ctx of the plugin's own that keeps the message of the host call it is passed
# to, so a failed call can be reported with its path or position in the text
# `jit.<Kind>: <what> <arg>: <host message> [<file>:<line>:<col>]`.
C_CAPTURE = [
    "struct DftuCap {",
    "    dftu_ctx c;",
    "    dftu_ctx* outer;",
    "    int has;",
    "    char msg[400];",
    "};",
    "",
    "static dftu_status dftu_cap_set(dftu_ctx* c, dftu_status code, const char* m, uint32_t n) {",
    "    DftuCap* k = (DftuCap*)c;",
    "    if (n >= sizeof(k->msg)) n = (uint32_t)sizeof(k->msg) - 1;",
    "    if (n) memcpy(k->msg, m, n);",
    "    k->msg[n] = 0;",
    "    k->has = 1;",
    "    return code;",
    "}",
    "",
    "static dftu_status dftu_cap_check(dftu_ctx* c) { return dftu_check(((DftuCap*)c)->outer); }",
    "",
    "static const dftu_ctx_vt dftu_cap_vt = {sizeof(dftu_ctx_vt), dftu_cap_set, dftu_cap_check};",
    "",
    "static void dftu_cap_init(DftuCap* k, dftu_ctx* outer) {",
    "    memset(k, 0, sizeof(*k));",
    "    k->c.struct_size = sizeof(dftu_ctx);",
    "    k->c.vt = &dftu_cap_vt;",
    "    k->outer = outer;",
    "}",
    "",
    "static dftu_status dftu_jit_fail(dftu_ctx* ctx, const DftuCap* cap, dftu_status st,",
    "                                 const char* what, const char* arg, const char* loc) {",
    '    const char* tag = st == DFTU_E_NOT_FOUND ? "NotFound"',
    '                      : st == DFTU_E_IO      ? "IoError"',
    '                      : st == DFTU_E_PARSE   ? "ParseError"',
    '                      : st == DFTU_E_CANCELLED ? "Cancelled"',
    '                                             : "Error";',
    "    char msg[4608];",
    '    snprintf(msg, sizeof(msg), "jit.%s [%s]: %s%s%s%s%s", tag, loc, what, arg[0] ? " " : "", arg,',
    '             cap->has ? ": " : "", cap->has ? cap->msg : "");',
    "    return dftu_fail(ctx, st, msg);",
    "}",
    "",
]

# Counters a test reads through dlsym: 0 task frames started, 1 task frames
# released, 2 files opened, 3 files closed, 4 task frames completed, 5 strings
# interned, 6 task frames cancelled.
C_STATS = [
    "static std::atomic<long long> g_dftu_stat[7];",
    "",
    "static void dftu_jit_stat_inc(int k) { g_dftu_stat[k].fetch_add(1, std::memory_order_relaxed); }",
    "",
    'extern "C" DFTU_PLUGIN_EXPORT long long dftu_jit_stat(int k) {',
    "    return (k >= 0 && k < 7) ? g_dftu_stat[k].load(std::memory_order_relaxed) : -1;",
    "}",
    "",
]

C_VALUE_HELPERS = [
    "static int dftu_jit_is_null(const dftu_value* v) { return !v || v->kind == DFTU_VAL_NULL; }",
    "",
    "static int64_t dftu_jit_count(const dftu_value* v) {",
    "    return (v && (v->kind == DFTU_VAL_ARRAY || v->kind == DFTU_VAL_OBJECT)) ? (int64_t)v->count : 0;",
    "}",
    "",
    "struct DftuView {",
    "    const char* p;",
    "    uint32_t n;",
    "    dftu_str id;",
    "};",
    "",
    "static dftu_str dftu_jit_intern(const dftu_plugin_host* host, const char* p, uint32_t n) {",
    "    dftu_jit_stat_inc(5);",
    "    return host->intern(host->h, p, n);",
    "}",
    "",
    "static DftuView dftu_jit_view(const char* p, uint32_t n) {",
    "    DftuView v;",
    "    v.p = p;",
    "    v.n = n;",
    "    v.id = DFTU_STR_NONE;",
    "    return v;",
    "}",
    "",
    "static dftu_str dftu_jit_vid(const dftu_plugin_host* host, DftuView* v) {",
    '    if (v->id == DFTU_STR_NONE) v->id = dftu_jit_intern(host, v->p ? v->p : "", v->n);',
    "    return v->id;",
    "}",
    "",
    "static DftuView dftu_jit_view_of(const dftu_plugin_host* host, dftu_str id) {",
    "    uint32_t n = 0;",
    "    const char* p = id == DFTU_STR_NONE ? NULL : host->resolve(host->h, id, &n);",
    '    return p ? dftu_jit_view(p, n) : dftu_jit_view("", 0);',
    "}",
    "",
    "static int dftu_jit_veq(DftuView a, DftuView b) {",
    "    return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);",
    "}",
    "",
    "static DftuView dftu_jit_as_view(const dftu_value* v, DftuView dflt) {",
    "    uint32_t n = 0;",
    "    const char* p = dftu_as_str(v, &n);",
    "    return p ? dftu_jit_view(p, n) : dflt;",
    "}",
    "",
    "static dftu_str dftu_jit_as_str(const dftu_plugin_host* host, const dftu_value* v, dftu_str dflt) {",
    "    uint32_t n = 0;",
    "    const char* p = dftu_as_str(v, &n);",
    "    return p ? dftu_jit_intern(host, p, n) : dflt;",
    "}",
    "",
    "static int64_t dftu_jit_slen(const dftu_plugin_host* host, dftu_str id) {",
    "    uint32_t n = 0;",
    "    if (id == DFTU_STR_NONE || !host->resolve(host->h, id, &n)) return 0;",
    "    return (int64_t)n;",
    "}",
    "",
    "static dftu_str dftu_jit_lit(const dftu_plugin_host* host, const char* s, uint32_t n) {",
    "    return dftu_jit_intern(host, s, n);",
    "}",
    "",
]


class TaskSig:
    """An ``async def`` task method: parameters as (name, kind), the return kind
    (``i64``, ``f64`` or None) and the method name."""

    __slots__ = ("name", "params", "ret")

    def __init__(self, name: str, params: list[tuple[str, str]], ret: str | None) -> None:
        self.name = name
        self.params = params
        self.ret = ret


class FlowFn:
    """One compiled flow body: its frame fields, the C of its steps and what it
    uses of the host."""

    def __init__(
        self,
        *,
        name: str,
        is_async: bool,
        sig: TaskSig | None,
        fields: list[str],
        body: list[str],
        ob_size: int,
        uses: set[str],
        res_frees: list[str],
        emit_names: list[str],
        store_gets: set[str],
        op_defs: list[str],
    ) -> None:
        self.name = name
        self.is_async = is_async
        self.sig = sig
        self.fields = fields
        self.body = body
        self.ob_size = ob_size
        self.uses = uses
        self.res_frees = res_frees
        self.emit_names = emit_names
        self.store_gets = store_gets
        self.op_defs = op_defs


_KIND_OF_ANNOTATION = {"int": "i64", "float": "f64"}


def task_signature(fn: Callable[..., object], name: str, source_text: str) -> TaskSig:
    """The parameters and return kind of a ``@jit.task`` method, read from its
    annotations (``int`` and ``float``). The method is ``async def``."""
    mod = ast.parse(source_text)
    funcs = [n for n in mod.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
    node = funcs[0]
    if not isinstance(node, ast.AsyncFunctionDef):
        raise _flow_error("@jit.task decorates an `async def` method", node)
    params: list[tuple[str, str]] = []
    for arg in node.args.args[1:]:
        ann = arg.annotation
        kind = _KIND_OF_ANNOTATION.get(ann.id) if isinstance(ann, ast.Name) else None
        if kind is None:
            raise _flow_error(
                f"parameter {arg.arg} of a @jit.task is annotated `int` or `float`", arg
            )
        params.append((arg.arg, kind))
    ret: str | None = None
    r = node.returns
    if isinstance(r, ast.Name) and r.id in _KIND_OF_ANNOTATION:
        ret = _KIND_OF_ANNOTATION[r.id]
    elif r is not None and not (isinstance(r, ast.Constant) and r.value is None):
        raise _flow_error("a @jit.task returns `int`, `float` or `None`", r)
    return TaskSig(name, params, ret)


def _flow_error(reason: str, node: ast.AST | None) -> JitError:
    err = JitError(reason, locate=False)
    err.node = node if isinstance(node, ast.AST) else None
    return err


def _frame_struct(fn: FlowFn) -> list[str]:
    return [
        f"struct DftuFrame_{fn.name} {{",
        "    int pc;",
        "    DftuRoot* root;",
        "    void* out;",
        *[f"    {f}" for f in fn.fields],
        "    dftu_task* tk;",
        f"    unsigned char ob[{max(fn.ob_size, 8)}];",
        "};",
        "",
    ]


def _step_fn(
    fn: FlowFn,
    cfg_pre: list[str],
    has_stores: bool,
    uses_cap: bool,
    close_lines: list[str] | None = None,
) -> list[str]:
    n = fn.name
    out = [
        f"static dftu_status _step_{n}(dftu_ctx* ctx, void* coro, dftu_task** wait, uint32_t cap,",
        "                              uint32_t* n_wait) {",
        f"    DftuFrame_{n}* S = (DftuFrame_{n}*)coro;",
        "    DftuRoot* R = S->root;",
        "    const dftu_plugin_host* host = R->host;",
        "    const dftu_svc_task* _tsk = R->tsk;",
        "    const dftu_svc_result* _res = R->res;",
        "    const dftu_svc_fileio* _fio = R->fio;",
        "    const dftu_svc_format* _fmtj = R->fmtj;",
        "    const dftu_svc_format* _fmty = R->fmty;",
        "    const dftu_svc_ports* _prt = R->prt;",
        "    (void)wait;",
        "    (void)cap;",
        "    (void)n_wait;",
        "    (void)_tsk;",
        "    (void)_res;",
        "    (void)_fio;",
        "    (void)_fmtj;",
        "    (void)_fmty;",
        "    (void)_prt;",
        *cfg_pre,
        "    DftuCap _cap;",
        "    dftu_cap_init(&_cap, ctx);",
        "    dftu_status _st = DFTU_OK;",
        "    int _unw = 0;",
        "    int _pclosed = 0;",
        "    (void)_cap;",
        "    (void)_unw;",
        "    (void)_pclosed;",
    ]
    if has_stores:
        out += ["    DftuStoreEnv& _se = R->se;", "    _se.ctx = ctx;", "    (void)_se;"]
    out += ["    switch (S->pc) {", "    case 0: ;"]
    out += ["    " + ln for ln in fn.body]
    out += ["    }", "_fx_exit:"]
    out += [f"    {ln}" for ln in fn.res_frees]
    out += [f"    {ln}" for ln in close_lines or []]
    out += [
        "    S->pc = -1;",
        "    if (_st < 0) return _st;",
        "    dftu_jit_stat_inc(4);",
        "    return DFTU_END;",
        "}",
        "",
    ]
    return out


def _release_fn(fn: FlowFn, is_root: bool) -> list[str]:
    n = fn.name
    out = [
        f"static void _release_{n}(void* coro) {{",
        f"    DftuFrame_{n}* S = (DftuFrame_{n}*)coro;",
        "    DftuRoot* R = S->root;",
        "    const dftu_svc_format* _fmtj = R->fmtj;",
        "    const dftu_svc_format* _fmty = R->fmty;",
        "    (void)_fmtj;",
        "    (void)_fmty;",
    ]
    out += [f"    {ln}" for ln in fn.res_frees]
    out += ["    if (S->pc != -1) dftu_jit_stat_inc(6);", "    dftu_jit_stat_inc(1);"]
    if is_root:
        out.append("    _root_free(R);")
    out += ["    free(S);", "}", ""]
    return out


def _job_fns(fn: FlowFn) -> list[str]:
    sig = fn.sig
    assert sig is not None
    n = fn.name
    fields = [f"    {'int64_t' if k == 'i64' else 'double'} p_{pn};" for pn, k in sig.params]
    params = "".join(f"{'int64_t' if k == 'i64' else 'double'} {pn}, " for pn, k in sig.params)
    out_ty = {"i64": "DFTU_T_I64", "f64": "DFTU_T_F64", None: "DFTU_T_VOID"}[sig.ret]
    out_size = "8" if sig.ret else "0"
    return [
        f"struct DftuJob_{n} {{",
        "    DftuRoot* root;",
        *fields,
        "};",
        "",
        f"static dftu_status _jobfn_{n}(dftu_ctx* ctx, void* state, const void* in, void* out,",
        "                              dftu_task** pending) {",
        "    (void)in;",
        f"    DftuJob_{n}* A = (DftuJob_{n}*)state;",
        f"    DftuFrame_{n}* F = (DftuFrame_{n}*)calloc(1, sizeof(DftuFrame_{n}));",
        '    if (!F) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
        "    F->root = A->root;",
        "    F->out = out;",
        *[f"    F->v_{pn} = A->p_{pn};" for pn, _ in sig.params],
        *(["    if (out) memset(out, 0, 8);"] if sig.ret else []),
        "    dftu_status st = A->root->tsk->drive(ctx, A->root->host->h, _step_"
        + n
        + ", F, _release_"
        + n
        + ", pending);",
        "    if (st < 0) {",
        "        free(F);",
        "        return st;",
        "    }",
        "    dftu_jit_stat_inc(0);",
        "    return DFTU_OK;",
        "}",
        "",
        f"static dftu_status _mkjob_{n}(dftu_ctx* ctx, DftuRoot* R, {params}dftu_job** out) {{",
        f"    DftuJob_{n}* A = (DftuJob_{n}*)calloc(1, sizeof(DftuJob_{n}));",
        '    if (!A) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
        "    A->root = R;",
        *[f"    A->p_{pn} = {pn};" for pn, _ in sig.params],
        "    dftu_job_desc d;",
        "    memset(&d, 0, sizeof(d));",
        "    d.struct_size = sizeof(d);",
        "    d.where = DFTU_JOB_INLINE;",
        f"    d.fn = _jobfn_{n};",
        "    d.state = A;",
        "    d.free_state = free;",
        "    d.in_ty = DFTU_T_VOID;",
        "    d.in_size = 0;",
        f"    d.out_ty = {out_ty};",
        f"    d.out_size = {out_size};",
        "    dftu_status st = R->tsk->job(ctx, R->host->h, &d, out);",
        "    if (st < 0) free(A);",
        "    return st;",
        "}",
        "",
    ]


def emit_flows(
    flows: list[FlowFn],
    *,
    has_cfg: bool,
    cfg_pre: list[str],
    has_stores: bool,
    chans: dict[str, PortDecl] | None = None,
) -> tuple[list[str], list[str], set[str]]:
    """The C of every flow body of a plugin and its ``on_finalize``: returns the
    definitions (after the config and store types), the ``on_finalize`` function
    and the host services the bodies use."""
    chans = chans or {}
    uses: set[str] = set()
    for fn in flows:
        uses |= fn.uses
    fin = [f for f in flows if f.sig is None]
    tasks = [f for f in flows if f.sig is not None]
    finalize = fin[0] if fin else None
    root = [
        "struct DftuRoot {",
        "    const dftu_plugin_host* host;",
        "    const dftu_svc_task* tsk;",
    ]
    root += [
        "    const dftu_svc_result* res;",
        "    const dftu_svc_fileio* fio;",
        "    const dftu_svc_format* fmtj;",
        "    const dftu_svc_format* fmty;",
    ]
    root += root_fields(chans)
    if has_cfg:
        root.append("    const DftuCfg* cfg;")
    if has_stores:
        root += ["    DftuStoreEnv se;", "    int se_open;"]
    root += ["};", ""]
    root_free = ["static void _root_free(DftuRoot* R) {"]
    if has_stores:
        root_free.append("    if (R->se_open) _st_close(&R->se, 0);")
    root_free += ["    free(R);", "}", ""]
    out = list(root) + root_free
    for fn in flows:
        out += _frame_struct(fn)
    for fn in tasks:
        sig = fn.sig
        assert sig is not None
        params = "".join(f"{'int64_t' if k == 'i64' else 'double'} {pn}, " for pn, k in sig.params)
        out.append(
            f"static dftu_status _mkjob_{fn.name}(dftu_ctx* ctx, DftuRoot* R, {params}dftu_job** out);"
        )
    out.append("")
    for fn in flows:
        out += _step_fn(
            fn,
            cfg_pre,
            has_stores,
            "cap" in uses,
            root_close_lines(chans) if fn.sig is None else None,
        )
        out += _release_fn(fn, fn.sig is None and fn.is_async)
    for fn in tasks:
        out += _job_fns(fn)
    # on_finalize
    fin_lines = [
        "static dftu_status on_finalize(dftu_ctx* ctx, void* slice,",
        "                               const dftu_plugin_host* host, dftu_task** pending) {",
        "    *pending = NULL;",
    ]
    if finalize is None:
        return out, close_without_finalize(chans), uses
    n = finalize.name
    fin_lines += [
        "    DftuRoot* R = (DftuRoot*)calloc(1, sizeof(DftuRoot));",
        '    if (!R) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
        "    R->host = host;",
        "    R->tsk = (const dftu_svc_task*)host->get_service(host->h, DFTU_SVC_TASK);",
        "    R->res = (const dftu_svc_result*)host->get_service(host->h, DFTU_SVC_RESULT);",
        "    R->fio = (const dftu_svc_fileio*)host->get_service(host->h, DFTU_SVC_FILEIO);",
        "    R->fmtj = (const dftu_svc_format*)host->get_service(host->h, DFTU_SVC_JSON);",
        "    R->fmty = (const dftu_svc_format*)host->get_service(host->h, DFTU_SVC_YAML);",
        "    R->prt = (const dftu_svc_ports*)host->get_service(host->h, DFTU_SVC_PORTS);",
    ]
    if has_cfg:
        fin_lines.append("    R->cfg = ((DftuSlice*)slice)->cfg;")
    else:
        fin_lines.append("    (void)slice;")
    needs = [
        ("tsk", "task", "task"),
        ("result", "res", "result"),
        ("fileio", "fio", "file"),
        ("json", "fmtj", "JSON"),
        ("yaml", "fmty", "YAML"),
        ("ports", "prt", "ports"),
    ]
    async_or_tasks = finalize.is_async or bool(tasks)
    for use, field, word in needs:
        if use in uses or (use == "task" and async_or_tasks) or (use == "ports" and chans):
            fin_lines += [
                f"    if (!R->{field}) {{",
                f'        dftu_status e = dftu_fail(ctx, DFTU_E_UNSUPPORTED, "the host has no {word} service");',
                "        _root_free(R);",
                "        return e;",
                "    }",
            ]
    fin_lines += root_open_lines(chans)
    if has_stores:
        fin_lines += [
            "    {",
            "        dftu_status so = _st_open(&R->se, ctx, host);",
            "        R->se_open = 1;",
            "        if (so < 0) {",
            "            _root_free(R);",
            "            return so;",
            "        }",
            "    }",
        ]
    if finalize.is_async:
        fin_lines += [
            f"    DftuFrame_{n}* F = (DftuFrame_{n}*)calloc(1, sizeof(DftuFrame_{n}));",
            "    if (!F) {",
            "        _root_free(R);",
            '        return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
            "    }",
            "    F->root = R;",
            f"    dftu_status st = R->tsk->drive(ctx, host->h, _step_{n}, F, _release_{n}, pending);",
            "    if (st < 0) {",
            "        free(F);",
            "        _root_free(R);",
            "        return st;",
            "    }",
            "    dftu_jit_stat_inc(0);",
            "    return DFTU_OK;",
            "}",
            "",
        ]
    else:
        fin_lines += [
            f"    DftuFrame_{n} F;",
            "    memset(&F, 0, sizeof(F));",
            "    F.root = R;",
            f"    dftu_status st = _step_{n}(ctx, &F, NULL, 0, NULL);",
            f"    _release_{n}_inline(&F);",
            "    _root_free(R);",
            "    return st < 0 ? st : DFTU_OK;",
            "}",
            "",
        ]
        # a stack frame is cleaned but not freed
        out += [
            f"static void _release_{n}_inline(DftuFrame_{n}* S) {{",
            "    DftuRoot* R = S->root;",
            "    const dftu_svc_format* _fmtj = R->fmtj;",
            "    const dftu_svc_format* _fmty = R->fmty;",
            "    (void)_fmtj;",
            "    (void)_fmty;",
            *[f"    {ln}" for ln in finalize.res_frees],
            "}",
            "",
        ]
    return out, fin_lines, uses
