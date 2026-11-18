# SPDX-License-Identifier: MIT
"""Compile an ``@jit.each_batch`` body of ``@jit.plugin`` to column operations.

The body works on whole columns, never on one event: ``df["dur"]`` is a column,
arithmetic and comparisons build lazy column expressions that the engine
evaluates fused in one pass, ``s[mask]`` filters, and a reduction folds a column
into scan state. This module lowers the AST to C that builds ``dftu_expr``
trees, evaluates them over the batch columns, filters and reduces. There is no
per-event loop in the emitted code.
"""

from __future__ import annotations

import ast
from typing import Any, Callable, Iterable, NoReturn

from .._c_text import c_str_literal
from .._eval import ARITH_CODE, COMPARE_CODE
from .._jit_errors import JitError, Source

VOCABULARY = (
    'df["col"], numbers, + - * /, unary -, comparisons (a < x <= b), & | ~ on masks, '
    "s[mask], s.where(mask, other), abs(s) or s.abs(), s.clip(lo, hi), s.isnull(), "
    "s.notnull(), s.fill_null(v), and the reductions s.sum(), s.min(), s.max(), "
    "s.count() and s.mean()"
)

_STRING_COLUMNS = frozenset({"name", "cat", "fhash", "hhash"})
_INT_COLUMNS = frozenset({"pid", "tid", "ts", "dur", "ph"})
# Scan columns the trace reader stores unsigned. In arithmetic they are read as
# int64, so ``dur - 20`` is negative as in Python instead of wrapping.
_U64_COLUMNS = frozenset({"pid", "tid", "ts", "dur"})

_CMP_FLIP = {0: 2, 1: 3, 2: 0, 3: 1, 4: 4, 5: 5}
_ABS, _NEGATE = 0, 8

_REDUCERS = {
    "sum": "DFTU_REDUCE_SUM",
    "min": "DFTU_REDUCE_MIN",
    "max": "DFTU_REDUCE_MAX",
    "count": None,
    "mean": None,
}
_NEEDS_VALUES = frozenset({"min", "max"})
_PORT_TAG = "@port:"

# The reductions a fold target takes. A batch value is folded as the target's
# own merge would fold the same rows, so only matching pairs are exact: sums add,
# a min of mins is the min, a max of maxes is the max. A mean is not additive, so
# a jit.mean() observes the selected elements (one row each) and the engine
# merges it exactly.
_TARGET_REDUCERS = {
    "PLAIN": ("sum", "count"),
    "DFTU_AGG_SUM": ("sum", "count"),
    "DFTU_AGG_MIN": ("min",),
    "DFTU_AGG_MAX": ("max",),
    "DFTU_AGG_MEAN": ("mean",),
}

HELPERS = [
    "static dftu_scalar dftu_jb_si(int64_t v) {",
    "    dftu_scalar s;",
    "    s.kind = DFTU_SCALAR_TAG_I64;",
    "    s.len = 0;",
    "    s.value.i = v;",
    "    return s;",
    "}",
    "",
    "static dftu_scalar dftu_jb_sf(double v) {",
    "    dftu_scalar s;",
    "    s.kind = DFTU_SCALAR_TAG_F64;",
    "    s.len = 0;",
    "    s.value.d = v;",
    "    return s;",
    "}",
    "",
    "static double dftu_jb_to_f64(dftu_scalar s) {",
    "    if (s.kind == DFTU_SCALAR_TAG_F64) return s.value.d;",
    "    if (s.kind == DFTU_SCALAR_TAG_U64) return (double)s.value.u;",
    "    return (double)s.value.i;",
    "}",
    "",
    "static int dftu_jb_to_i64(dftu_scalar s, int64_t* out) {",
    "    if (s.kind == DFTU_SCALAR_TAG_F64) return 0;",
    "    *out = s.kind == DFTU_SCALAR_TAG_U64 ? (int64_t)s.value.u : s.value.i;",
    "    return 1;",
    "}",
    "",
]


def _scalar_c(lit: _V) -> str:
    """C text for a number literal as a ``dftu_scalar``."""
    value = lit.value
    assert isinstance(value, (int, float))
    if isinstance(value, int):
        return f"dftu_jb_si({value}LL)"
    return f"dftu_jb_sf({value!r})"


def _fail(reason: str, node: ast.AST | None) -> NoReturn:
    err = JitError(reason, locate=False)
    err.node = node
    raise err


def _unwrap(node: ast.expr) -> ast.expr:
    index = getattr(ast, "Index", None)
    if index is not None and isinstance(node, index):
        value = getattr(node, "value", node)
        assert isinstance(value, ast.expr)
        return value
    return node


class _V:
    """One node of the lazy column dataflow: a column, a literal, an
    elementwise operation, a filter or a reduction.

    ``shape`` is ``series``, ``mask`` (a Bool series), ``scalar`` (a
    reduction) or ``lit``. ``num`` is the numeric family the engine will
    produce, ``int``, ``float`` or ``any`` when it depends on the column's
    runtime type. ``domain`` numbers the row set the series is over, so a
    filtered series cannot be combined with an unfiltered one."""

    __slots__ = ("kind", "args", "shape", "num", "domain", "key", "value", "name")

    def __init__(
        self,
        kind: str,
        shape: str,
        num: str,
        domain: int,
        args: tuple[_V, ...] = (),
        value: object = None,
        name: str = "",
    ) -> None:
        self.kind = kind
        self.shape = shape
        self.num = num
        self.domain = domain
        self.args = args
        self.value = value
        self.name = name
        self.key: str = f"{kind}:{name}:{value!r}({','.join(a.key for a in args)})"


class BatchHook:
    """The lowered body: the C statements, the columns it reads, and the
    plain-state or descriptor fields it folds into."""

    def __init__(self) -> None:
        self.lines: list[str] = []
        self.columns: list[str] = []
        self.rows: dict[str, int] = {}
        self.updated: list[str] = []
        self.chan_uses: set[str] = set()
        self.sends = 0


class _Lowering:
    def __init__(
        self,
        maps: dict[str, object],
        plains: dict[str, object],
        monoids: tuple[type, ...],
        chans: dict[str, Any],
    ) -> None:
        self.maps = maps
        self.plains = plains
        self.chans = chans
        self.self_name = "self"
        self.df_name = "df"
        self.locals: dict[str, _V] = {}
        self.columns: list[str] = []
        self.hook = BatchHook()
        self.domains: dict[tuple[int, str], int] = {}
        # Emission state.
        self.body: list[str] = []
        self.leaf_of_col: dict[str, int] = {}
        self.leaf_count = 0
        self.leaf_series: dict[str, int] = {}
        self.exprs: dict[str, str] = {}
        self.series: dict[str, str] = {}
        self.expr_count = 0
        self.temp_count = 0
        self.fold_count = 0
        self.cur: ast.AST | None = None
        # (attribute, element family, reduction) in statement order; emitted after
        # the whole body is read so independent column expressions share a program.
        self.folds: list[tuple[str, str, _V]] = []

    # ---- front end -------------------------------------------------------

    def lower(self, fn_ast: ast.FunctionDef) -> BatchHook:
        params = fn_ast.args.args
        if len(params) != 2:
            _fail("each_batch must take exactly (self, df)", fn_ast)
        self.self_name = params[0].arg
        self.df_name = params[1].arg
        for stmt in fn_ast.body:
            self.cur = stmt
            self._stmt(stmt)
        if not self.hook.updated and not self.hook.sends:
            _fail("the each_batch body folds nothing into scan state", fn_ast)
        self._eval_independent([v for _, _, v in self.folds])
        for attr, elem, v in self.folds:
            if attr.startswith(_PORT_TAG):
                self._emit_port(attr[len(_PORT_TAG) :], elem, v)
            else:
                self._emit_fold(attr, elem, v)
        prologue: list[str] = []
        c_n = max(len(self.columns), 1)
        prologue.append(f"dftu_series* _C[{c_n}] = {{0}};")
        if self.leaf_count > 0:
            prologue.append(f"dftu_series* _L[{self.leaf_count}] = {{0}};")
        prologue.append(f"dftu_series* _T[{max(self.temp_count, 1)}] = {{0}};")
        prologue.append(f"dftu_expr* _X[{max(self.expr_count, 1)}] = {{0}};")
        for i, name in enumerate(self.columns):
            prologue.append(f"_C[{i}] = dftu_dataframe_column(df, {c_str_literal(name)});")
            prologue.append(
                f"if (!_C[{i}]) {{ _st = dftu_fail(ctx, DFTU_E_INTERNAL, "
                f'"the host did not project column {name}"); goto _bdone; }}'
            )
        epilogue = ["_bdone:;"]
        epilogue.append(
            f"for (int _k = 0; _k < {max(self.temp_count, 1)}; ++_k) dftu_series_free(_T[_k]);"
        )
        epilogue.append(f"for (int _k = 0; _k < {c_n}; ++_k) dftu_series_free(_C[_k]);")
        epilogue.append(
            f"for (int _k = 0; _k < {max(self.expr_count, 1)}; ++_k) dftu_expr_free(_X[_k]);"
        )
        self.hook.lines = ["{"] + ["    " + ln for ln in prologue + self.body + epilogue] + ["}"]
        self.hook.columns = sorted(self.columns)
        return self.hook

    def _stmt(self, stmt: ast.stmt) -> None:
        if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Constant):
            if isinstance(stmt.value.value, str):
                return
        if isinstance(stmt, ast.Pass):
            return
        if isinstance(stmt, (ast.Assign, ast.AnnAssign)):
            self._assign(stmt)
            return
        if isinstance(stmt, ast.AugAssign):
            self._fold(stmt.target, stmt.value, "+=" if isinstance(stmt.op, ast.Add) else "", stmt)
            return
        if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call):
            self._call_stmt(stmt.value)
            return
        if isinstance(stmt, ast.Return):
            if stmt.value is not None:
                _fail(
                    "a batch hook returns nothing: a Series cannot be returned, because it "
                    "is valid only inside this call; fold it into scan state with a reduction",
                    stmt,
                )
            _fail("return statement in a batch hook; the body is straight-line column code", stmt)
        _fail(
            f"{type(stmt).__name__} in a batch hook; the body is straight-line column code. "
            f"Use a mask (s[mask]) for a condition. Vocabulary: {VOCABULARY}",
            stmt,
        )

    def _is_state(self, node: ast.expr) -> bool:
        cur = node
        while isinstance(cur, (ast.Subscript, ast.Attribute, ast.Call)):
            if isinstance(cur, ast.Subscript):
                cur = cur.value
            elif isinstance(cur, ast.Call):
                cur = cur.func
            else:
                if isinstance(cur.value, ast.Name) and cur.value.id == self.self_name:
                    return True
                cur = cur.value
        return False

    def _assign(self, stmt: ast.Assign | ast.AnnAssign) -> None:
        if isinstance(stmt, ast.Assign):
            if len(stmt.targets) != 1:
                _fail("chained assignment in a batch hook", stmt)
            target = stmt.targets[0]
        else:
            target = stmt.target
        value = stmt.value
        if value is None:
            _fail("an annotation without a value in a batch hook", stmt)
        if self._is_state(target):
            v = self._value(value)
            if v.shape in ("series", "mask"):
                _fail(
                    "a Series stored on self escapes the batch: it is valid only inside "
                    "this call. Fold it into scan state with a reduction such as "
                    "self.total += s.sum()",
                    stmt,
                )
            _fail(
                "assigning a reduction to scan state has no merge: write self.<field> += "
                "<reduction> (a worker slice starts at zero and slices are added)",
                stmt,
            )
        if not isinstance(target, ast.Name):
            _fail("assignment target in a batch hook (expected a local name)", target)
        self.locals[target.id] = self._value(value)

    def _port_call(self, call: ast.Call) -> bool:
        """``self.<port>.send(<reduction>)``, ``.try_send``, ``.set`` or
        ``.contribute``: the value is a batch reduction (sum, min, max or count of a
        column expression), sent or contributed once per batch."""
        func = call.func
        if not (
            isinstance(func, ast.Attribute)
            and isinstance(func.value, ast.Attribute)
            and isinstance(func.value.value, ast.Name)
            and func.value.value.id == self.self_name
            and func.value.attr in self.chans
        ):
            return False
        decl = self.chans[func.value.attr]
        method = func.attr
        usable = {"channel": ("send", "try_send"), "latest": ("set",), "phase": ("contribute",)}
        if method not in usable[decl.kind]:
            _fail(
                f"self.{decl.attr}.{method}() in an each_batch body; a batch hook has "
                + ", ".join(f"{m}()" for m in usable[decl.kind])
                + " of a reduction (seal, result, recv and close are calls of @jit.finalize "
                "or a @jit.task of an @jit.each_event plugin)",
                call,
            )
        if decl.kind == "channel" and decl.policy == "block":
            _fail(
                f"self.{decl.attr} is a Block channel, used from an async @jit.finalize or "
                "@jit.task of an @jit.each_event plugin, never from a batch hook",
                call,
            )
        if len(call.args) != 1 or call.keywords:
            _fail(f"self.{decl.attr}.{method}() takes one reduction", call)
        v = self._value(call.args[0])
        if v.shape != "scalar" or v.name not in _REDUCERS:
            _fail(
                f'self.{decl.attr}.{method}() sends a reduction such as df["dur"].sum(), '
                "not a column or a bare number",
                call.args[0],
            )
        if decl.elem == "i64" and v.num == "float":
            _fail(
                f"self.{decl.attr} carries an int and this reduction is a float; "
                "declare the port with float",
                call.args[0],
            )
        self.hook.sends += 1
        self.hook.chan_uses.add(decl.attr)
        self.folds.append((f"{_PORT_TAG}{decl.attr}:{method}", decl.elem, v))
        return True

    def _call_stmt(self, call: ast.Call) -> None:
        if self._port_call(call):
            return
        func = call.func
        if (
            isinstance(func, ast.Attribute)
            and func.attr in ("observe", "append", "add")
            and self._is_state(func.value)
        ):
            if len(call.args) != 1 or call.keywords:
                _fail(f".{func.attr} takes exactly one value", call)
            if func.attr != "observe":
                v = self._value(call.args[0])
                if v.shape in ("series", "mask"):
                    _fail(
                        "a Series stored on self escapes the batch: it is valid only inside "
                        "this call. Fold it into scan state with a reduction",
                        call,
                    )
                _fail(
                    f".{func.attr}() on scan state is a per-event update; a batch hook folds "
                    "a reduction with += or .observe(<reduction>)",
                    call,
                )
            self._fold(func.value, call.args[0], "observe", call)
            return
        _fail(
            "a call statement in a batch hook; the only statements are "
            "name = <column expression> and self.<field> += <reduction>. "
            f"Vocabulary: {VOCABULARY}",
            call,
        )

    # ---- fold targets ----------------------------------------------------

    def _target(self, node: ast.expr, form: str) -> tuple[str, str, str]:
        """(attribute, element family, monoid op) of a fold target."""
        if isinstance(node, ast.Attribute):
            base = node
        elif isinstance(node, ast.Subscript):
            sl = _unwrap(node.slice)
            if not (
                isinstance(node.value, ast.Attribute) and isinstance(sl, ast.Tuple) and not sl.elts
            ):
                _fail(
                    "a keyed update in a batch hook: a batch hook folds into a scalar field "
                    "or a jit.map with key=(); use @jit.each_event or @jit.vfold to key by "
                    "column",
                    node,
                )
            base = node.value
        else:
            _fail("fold target in a batch hook (expected self.<field>)", node)
        attr = base.attr
        plain = self.plains.get(attr)
        if plain is not None:
            kind = getattr(plain, "kind")
            if kind != "scalar":
                _fail(
                    f"self.{attr} is a {kind}: a batch hook folds into a number; a {kind} "
                    "takes per-event updates (@jit.each_event)",
                    node,
                )
            if form != "+=":
                _fail(f"self.{attr} is a number: fold into it with += <reduction>", node)
            return attr, getattr(plain, "elem"), "PLAIN"
        decl = self.maps.get(attr)
        if decl is None:
            _fail(f"unknown state field self.{attr}", node)
        if getattr(decl, "key_types") or getattr(decl, "is_product"):
            _fail(
                f"self.{attr} is keyed or a product: a batch hook folds into a jit.map with "
                "key=() and one value",
                node,
            )
        monoid = getattr(decl, "values")[0]
        if monoid.elem not in ("i64", "f64") or monoid.needs_by:
            _fail(f"self.{attr} does not fold a single number", node)
        if monoid.dft not in _TARGET_REDUCERS:
            _fail(
                f"self.{attr} keeps more than one number per batch; a batch hook folds into "
                "jit.sum(), jit.count(), jit.min(), jit.max() and jit.mean()",
                node,
            )
        if form == "+=":
            if monoid.dft != "DFTU_AGG_SUM":
                _fail(f"+= on the non-additive reduction {monoid.dft}; use .observe(...)", node)
        elif monoid.dft == "DFTU_AGG_SUM":
            _fail(f"self.{attr} is additive: fold into it with += <reduction>", node)
        return attr, monoid.elem, monoid.dft

    def _fold(self, target: ast.expr, value: ast.expr, form: str, stmt: ast.AST) -> None:
        if form not in ("+=", "observe"):
            _fail("augmented operator other than += in a batch hook", stmt)
        if not self._is_state(target):
            _fail("assignment target in a batch hook (expected self.<field>)", target)
        attr, elem, dft = self._target(target, form)
        v = self._value(value)
        if v.shape in ("series", "mask"):
            _fail(
                "a Series cannot be folded into scan state: reduce it first with "
                ".sum(), .min(), .max(), .count() or .mean()",
                value,
            )
        if v.shape != "scalar":
            _fail("fold a reduction such as s.sum(), not a bare number", value)
        if v.name not in _TARGET_REDUCERS[dft]:
            if v.name == "mean" and dft == "PLAIN":
                _fail(
                    f"a mean is not additive across batches, so self.{attr} cannot add it: "
                    f"fold .sum() and .count(), or observe it in a jit.map(key=(), "
                    "value=jit.mean())",
                    value,
                )
            want = " or ".join(f".{r}()" for r in _TARGET_REDUCERS[dft])
            _fail(
                f"self.{attr} folds {want}, not .{v.name}(): a batch value merges exactly only "
                "with the matching reduction",
                value,
            )
        if elem == "i64" and v.num == "float":
            _fail(
                f"self.{attr} is an integer field and this reduction is a float; declare "
                f"{attr}: float = 0.0",
                value,
            )
        if attr not in self.hook.updated:
            self.hook.updated.append(attr)
        self.hook.rows[attr] = self.hook.rows.get(attr, 0) + 1
        self.folds.append((attr, elem, v))

    # ---- expressions -----------------------------------------------------

    def _new_domain(self, parent: int, mask_key: str) -> int:
        key = (parent, mask_key)
        if key not in self.domains:
            self.domains[key] = len(self.domains) + 1
        return self.domains[key]

    def _series_args(self, nodes: Iterable[_V], at: ast.AST) -> int:
        domain: int | None = None
        for v in nodes:
            if v.shape == "lit":
                continue
            if v.shape == "scalar":
                _fail("a reduction inside a column expression; fold it with a statement", at)
            if domain is None:
                domain = v.domain
            elif v.domain != domain:
                _fail(
                    "these columns are over different row sets (one is filtered by a mask "
                    "the other is not), so their rows do not line up",
                    at,
                )
        assert domain is not None
        return domain

    @staticmethod
    def _num_of(a: _V, b: _V, div: bool = False) -> str:
        if div or "float" in (a.num, b.num):
            return "float"
        if "any" in (a.num, b.num):
            return "any"
        return "int"

    def _lit(self, node: ast.expr) -> _V | None:
        node = _unwrap(node)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.USub, ast.UAdd)):
            inner = self._lit(node.operand)
            if inner is not None:
                value = inner.value
                assert isinstance(value, (int, float))
                return _V(
                    "lit",
                    "lit",
                    inner.num,
                    0,
                    value=-value if isinstance(node.op, ast.USub) else value,
                )
            return None
        if isinstance(node, ast.Constant):
            v = node.value
            if isinstance(v, bool):
                _fail("a boolean literal in a column expression", node)
            if isinstance(v, int):
                return _V("lit", "lit", "int", 0, value=v)
            if isinstance(v, float):
                return _V("lit", "lit", "float", 0, value=v)
            _fail("a literal other than a number in a column expression", node)
        return None

    def _value(self, node: ast.expr) -> _V:
        node = _unwrap(node)
        lit = self._lit(node)
        if lit is not None:
            return lit
        if isinstance(node, ast.Name):
            if node.id in self.locals:
                return self.locals[node.id]
            if node.id == self.df_name:
                _fail(f'{node.id} is the batch: read a column with {node.id}["name"]', node)
            _fail(
                f"unknown name {node.id}; a batch hook sees only {self.df_name}, its locals "
                "and numbers",
                node,
            )
        if isinstance(node, ast.Subscript):
            return self._subscript(node)
        if isinstance(node, ast.BinOp):
            return self._binop(node)
        if isinstance(node, ast.UnaryOp):
            return self._unary(node)
        if isinstance(node, ast.Compare):
            return self._compare(node)
        if isinstance(node, ast.BoolOp):
            sym = "&" if isinstance(node.op, ast.And) else "|"
            _fail(
                f"`and` / `or` do not combine columns: write (a) {sym} (b) with parentheses",
                node,
            )
        if isinstance(node, ast.Call):
            return self._call(node)
        if isinstance(node, ast.Attribute) and self._is_state(node):
            _fail(
                "scan state is write-only in the body: each worker holds only its own slice, "
                "so a read would see a partial value",
                node,
            )
        _fail(
            f"{type(node).__name__} in a column expression. Vocabulary: {VOCABULARY}",
            node,
        )

    def _column(self, name: str, node: ast.AST) -> _V:
        if name in _STRING_COLUMNS:
            _fail(
                f'column "{name}" is a string; a batch hook works on numeric columns '
                "(use @jit.each_event for strings)",
                node,
            )
        if name not in self.columns:
            self.columns.append(name)
        num = "int" if name in _INT_COLUMNS else "any"
        return _V("col", "series", num, 0, name=name)

    def _subscript(self, node: ast.Subscript) -> _V:
        sl = _unwrap(node.slice)
        base = _unwrap(node.value)
        if isinstance(base, ast.Name) and base.id == self.df_name:
            if not (isinstance(sl, ast.Constant) and isinstance(sl.value, str)):
                _fail(f'{self.df_name}[...] takes a column name string, {self.df_name}["dur"]', sl)
            return self._column(sl.value, node)
        series = self._value(base)
        if series.shape == "scalar" or series.shape == "lit":
            _fail("only a column can be indexed with a mask", node)
        mask = self._value(sl)
        if mask.shape != "mask":
            _fail("a column is indexed by a mask built from comparisons, s[s > 3]", sl)
        self._series_args((series, mask), node)
        domain = self._new_domain(series.domain, mask.key)
        return _V("filter", series.shape, series.num, domain, args=(series, mask))

    def _binop(self, node: ast.BinOp) -> _V:
        a = self._value(node.left)
        b = self._value(node.right)
        if isinstance(node.op, (ast.BitAnd, ast.BitOr)):
            if a.shape != "mask" or b.shape != "mask":
                _fail("& and | combine masks; compare the columns first, (a > 1) & (b < 2)", node)
            domain = self._series_args((a, b), node)
            kind = "and" if isinstance(node.op, ast.BitAnd) else "or"
            return _V(kind, "mask", "int", domain, args=(a, b))
        code = ARITH_CODE.get(type(node.op))
        if code is None:
            _fail(
                f"operator {type(node.op).__name__} in a column expression. "
                f"Vocabulary: {VOCABULARY}",
                node,
            )
        for v in (a, b):
            if v.shape == "mask":
                _fail("arithmetic on a mask; use & | ~ to combine masks", node)
        if a.shape == "lit" and b.shape == "lit":
            _fail("a constant expression has no column", node)
        domain = self._series_args((a, b), node)
        return _V(
            "bin",
            "series",
            self._num_of(a, b, code == 3),
            domain,
            args=(a, b),
            value=code,
        )

    def _unary(self, node: ast.UnaryOp) -> _V:
        if isinstance(node.op, ast.Not):
            _fail("`not` does not apply to a column: write ~(mask)", node)
        a = self._value(node.operand)
        if isinstance(node.op, ast.Invert):
            if a.shape != "mask":
                _fail("~ inverts a mask built from comparisons", node)
            return _V("not", "mask", "int", a.domain, args=(a,))
        if isinstance(node.op, ast.USub):
            if a.shape != "series":
                _fail("unary - applies to a numeric column", node)
            return _V("neg", "series", a.num, a.domain, args=(a,))
        _fail("unary operator in a column expression", node)

    def _compare(self, node: ast.Compare) -> _V:
        parts: list[_V] = []
        left = node.left
        for op, right in zip(node.ops, node.comparators):
            code = COMPARE_CODE.get(type(op))
            if code is None:
                _fail(
                    f"`{type(op).__name__}` on columns. Vocabulary: {VOCABULARY}",
                    node,
                )
            a = self._value(left)
            b = self._value(right)
            for v in (a, b):
                if v.shape == "mask":
                    _fail("a comparison of a mask; compare the numeric columns", node)
            if a.shape == "lit" and b.shape == "lit":
                _fail("a comparison of two constants has no column", node)
            if a.shape == "lit":
                a, b, code = b, a, _CMP_FLIP[code]
            domain = self._series_args((a, b), node)
            parts.append(_V("cmp", "mask", "int", domain, args=(a, b), value=code))
            left = right
        result = parts[0]
        for nxt in parts[1:]:
            domain = self._series_args((result, nxt), node)
            result = _V("and", "mask", "int", domain, args=(result, nxt))
        return result

    def _call(self, node: ast.Call) -> _V:
        func = node.func
        if node.keywords:
            _fail("keyword arguments are not part of the batch vocabulary", node)
        if isinstance(func, ast.Name):
            if func.id == "abs" and len(node.args) == 1:
                return self._abs(self._value(node.args[0]), node)
            _fail(
                f"call to {func.id}() in a batch hook. Vocabulary: {VOCABULARY}",
                node,
            )
        if not isinstance(func, ast.Attribute):
            _fail(f"call in a batch hook. Vocabulary: {VOCABULARY}", node)
        name = func.attr
        if name in _REDUCERS:
            if node.args:
                _fail(f".{name}() takes no arguments", node)
            s = self._value(func.value)
            if s.shape not in ("series", "mask"):
                _fail(f".{name}() reduces a column", node)
            num = {"count": "int", "mean": "float"}.get(name, "int" if s.shape == "mask" else s.num)
            return _V("red", "scalar", num, s.domain, args=(s,), name=name)
        s = self._value(func.value)
        if s.shape not in ("series", "mask"):
            _fail(f".{name}() applies to a column. Vocabulary: {VOCABULARY}", node)
        if name == "abs":
            if node.args:
                _fail(".abs() takes no arguments", node)
            return self._abs(s, node)
        if name in ("isnull", "notnull"):
            if node.args:
                _fail(f".{name}() takes no arguments", node)
            return _V(
                "isnull", "mask", "int", s.domain, args=(s,), value=1 if name == "isnull" else 0
            )
        if name == "fill_null":
            if len(node.args) != 1:
                _fail(".fill_null() takes one number", node)
            fill = self._lit(node.args[0])
            if fill is None:
                _fail(".fill_null() takes a number literal", node.args[0])
            return _V("fill", "series", self._num_of(s, fill), s.domain, args=(s, fill))
        if name == "clip":
            if len(node.args) != 2:
                _fail(".clip() takes two number literals, clip(lo, hi)", node)
            lo = self._lit(node.args[0])
            hi = self._lit(node.args[1])
            if lo is None or hi is None:
                _fail(".clip() takes number literals", node)
            if "float" in (lo.num, hi.num):
                s = _V("tofloat", "series", "float", s.domain, args=(s,))
            return _V("clip", "series", s.num, s.domain, args=(s, lo, hi))
        if name == "where":
            if len(node.args) != 2:
                _fail(".where() takes (mask, other)", node)
            mask = self._value(node.args[0])
            if mask.shape != "mask":
                _fail(".where() takes a mask built from comparisons", node.args[0])
            other = self._value(node.args[1])
            if other.shape == "mask" or other.shape == "scalar":
                _fail(".where() other must be a number or a numeric column", node.args[1])
            domain = self._series_args((s, mask, other), node)
            return _V("where", "series", self._num_of(s, other), domain, args=(mask, s, other))
        _fail(f".{name}() is outside the batch vocabulary: {VOCABULARY}", node)

    def _abs(self, s: _V, node: ast.AST) -> _V:
        if s.shape != "series":
            _fail("abs() applies to a numeric column", node)
        return _V("abs", "series", s.num, s.domain, args=(s,))

    # ---- emission --------------------------------------------------------

    def _line(self, text: str) -> None:
        self.body.append(text)

    def _check(self, handle: str, what: str) -> None:
        self._line(
            f'if (!{handle}) {{ _st = dftu_fail(ctx, DFTU_E_NO_MEMORY, "{what}"); goto _bdone; }}'
        )

    def _leaf(self, v: _V) -> int:
        """The index in ``_L`` of the series ``v`` as an input column."""
        if v.key in self.leaf_series:
            return self.leaf_series[v.key]
        if v.kind == "col":
            slot = self.columns.index(v.name)
            handle = f"_C[{slot}]"
        else:
            handle = self._materialized(v)
        idx = self.leaf_count
        self.leaf_count += 1
        self._line(f"_L[{idx}] = {handle};")
        self.leaf_series[v.key] = idx
        return idx

    def _materialized(self, v: _V) -> str:
        sel = self._series(v)
        t = self.temp_count
        self.temp_count += 1
        self._line(f"_T[{t}] = dftu_series_materialize({sel});")
        self._check(f"_T[{t}]", "out of memory")
        return f"_T[{t}]"

    @staticmethod
    def _is_unsigned(v: _V) -> bool:
        base = v.args[0] if v.kind == "filter" else v
        return base.kind == "col" and base.name in _U64_COLUMNS

    def _expr(self, v: _V, raw: bool = False) -> str:
        """The name of the ``dftu_expr*`` computing the elementwise node ``v``.
        ``raw`` keeps an unsigned scan column as stored, for a comparison with a
        non-negative literal, which the engine runs as a direct kernel."""
        key = v.key + ("#raw" if raw else "")
        if key in self.exprs:
            return self.exprs[key]
        x = self.expr_count
        self.expr_count += 1
        name = f"_X[{x}]"
        if v.kind in ("col", "filter"):
            if self._is_unsigned(v) and not raw:
                self._line(
                    f"{name} = dftu_expr_cast(DFTU_TYPE_INT64, dftu_expr_col({self._leaf(v)}));"
                )
            else:
                self._line(f"{name} = dftu_expr_col({self._leaf(v)});")
        elif v.kind == "lit":
            assert isinstance(v.value, (int, float))
            if isinstance(v.value, int):
                self._line(f"{name} = dftu_expr_lit_i64({v.value}LL);")
            else:
                self._line(f"{name} = dftu_expr_lit_f64({v.value!r});")
        elif v.kind == "bin":
            a, b = (self._expr(c) for c in v.args)
            self._line(f"{name} = dftu_expr_binary({v.value}, {a}, {b});")
        elif v.kind == "tofloat":
            self._line(f"{name} = dftu_expr_cast(DFTU_TYPE_FLOAT64, {self._expr(v.args[0])});")
        elif v.kind == "neg":
            self._line(f"{name} = dftu_expr_unary({_NEGATE}, {self._expr(v.args[0])});")
        elif v.kind == "abs":
            self._line(f"{name} = dftu_expr_unary({_ABS}, {self._expr(v.args[0])});")
        elif v.kind == "cmp":
            left, right = v.args
            if right.shape == "lit":
                plain = isinstance(right.value, int) and right.value >= 0
                self._line(
                    f"{name} = dftu_expr_cmp({v.value}, {self._expr(left, plain)}, "
                    f"{_scalar_c(right)});"
                )
            else:
                self._line(
                    f"{name} = dftu_expr_cmp_expr({v.value}, {self._expr(left)}, "
                    f"{self._expr(right)});"
                )
        elif v.kind in ("and", "or"):
            a, b = (self._expr(c) for c in v.args)
            op = "DFTU_LOGICAL_AND" if v.kind == "and" else "DFTU_LOGICAL_OR"
            self._line(f"{name} = dftu_expr_logical({op}, {a}, {b});")
        elif v.kind == "not":
            self._line(f"{name} = dftu_expr_not({self._expr(v.args[0])});")
        elif v.kind == "isnull":
            self._line(f"{name} = dftu_expr_is_null({self._expr(v.args[0])}, {v.value});")
        elif v.kind == "fill":
            a, fill = (self._expr(c) for c in v.args)
            self._line(f"{{ const dftu_expr* _args[2] = {{{a}, {fill}}};")
            self._line(f"  {name} = dftu_expr_coalesce(_args, 2); }}")
        elif v.kind == "clip":
            lo, hi = v.args[1], v.args[2]
            self._line(
                f"{name} = dftu_expr_clip({self._expr(v.args[0])}, {_scalar_c(lo)}, {_scalar_c(hi)});"
            )
        elif v.kind == "where":
            cond, a, b = (self._expr(c) for c in v.args)
            self._line(f"{name} = dftu_expr_select({cond}, {a}, {b});")
        else:
            raise AssertionError(v.kind)
        self._check(name, "out of memory")
        self.exprs[key] = name
        return name

    def _series(self, v: _V) -> str:
        """The name of the evaluated series for ``v`` (a column, a filter's
        selection, or an evaluated expression)."""
        if v.key in self.series:
            return self.series[v.key]
        if v.kind == "col":
            slot = self.columns.index(v.name)
            result = f"_C[{slot}]"
        elif v.kind == "filter":
            s, m = v.args
            sv, mv = self._series(s), self._series(m)
            t = self.temp_count
            self.temp_count += 1
            self._line(f"_T[{t}] = dftu_series_filter({sv}, {mv});")
            self._check(f"_T[{t}]", "the filter failed")
            result = f"_T[{t}]"
        else:
            root = self._expr(v)
            t = self.temp_count
            self.temp_count += 1
            self._line(f"_T[{t}] = dftu_expr_eval({root}, _L, {self.leaf_count});")
            self._line(
                f'if (!_T[{t}]) {{ _st = dftu_fail(ctx, DFTU_E_INVALID, "a column expression '
                f'failed to evaluate (a type the operation does not accept)"); goto _bdone; }}'
            )
            result = f"_T[{t}]"
        self.series[v.key] = result
        return result

    @staticmethod
    def _over_columns(v: _V) -> bool:
        """True when ``v`` is an elementwise tree over scan columns and literals
        only, so it can be evaluated before any filter has run."""
        if v.kind in ("col", "lit"):
            return True
        if v.kind in ("filter", "red"):
            return False
        return all(_Lowering._over_columns(a) for a in v.args)

    def _collect(self, v: _V, out: list[_V]) -> None:
        if v.kind == "red":
            self._collect(v.args[0], out)
        elif v.kind == "filter":
            for a in v.args:
                self._collect(a, out)
        elif v.kind in ("col", "lit"):
            return
        elif self._over_columns(v):
            if all(o.key != v.key for o in out):
                out.append(v)
        else:
            for a in v.args:
                self._collect(a, out)

    def _eval_independent(self, roots: list[_V]) -> None:
        """Evaluate every expression over scan columns that the folds need in one
        ``dftu_expr_eval_many`` program, so the engine compiles once and shares
        subexpressions."""
        found: list[_V] = []
        for v in roots:
            self._collect(v, found)
        if len(found) < 2:
            return
        names = [self._expr(v) for v in found]
        t = self.temp_count
        self.temp_count += len(found)
        self._line("{")
        self._line(f"    const dftu_expr* _roots[{len(found)}] = {{{', '.join(names)}}};")
        self._line(
            f"    if (dftu_expr_eval_many(_roots, {len(found)}, _L, {self.leaf_count}, "
            f"&_T[{t}]) != {len(found)}) {{ _st = dftu_fail(ctx, DFTU_E_INVALID, "
            '"a column expression failed to evaluate (a type the operation does not '
            'accept)"); goto _bdone; }'
        )
        self._line("}")
        for i, v in enumerate(found):
            self.series[v.key] = f"_T[{t + i}]"

    def _emit_observe_all(self, attr: str, sv: str, n: int) -> None:
        """One row per non-null element of ``sv`` into the buffer of ``attr``."""
        t = self.temp_count
        self.temp_count += 3
        self._line("{")
        self._line(f"    _T[{t}] = dftu_series_drop_nulls({sv});")
        self._line(f"    _T[{t + 1}] = _T[{t}] ? dftu_series_materialize(_T[{t}]) : NULL;")
        self._line(
            f'    if (!_T[{t + 1}]) {{ _st = dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory"); goto _bdone; }}'
        )
        self._line(f"    dftu_series* _e{n} = _T[{t + 1}];")
        self._line(f"    if (dftu_series_type(_e{n}) != DFTU_TYPE_FLOAT64) {{")
        self._line(f"        _T[{t + 2}] = dftu_series_cast(_e{n}, DFTU_TYPE_FLOAT64);")
        self._line(
            f"        if (!_T[{t + 2}]) {{ _st = dftu_fail(ctx, DFTU_E_INVALID, "
            f'"mean() is not defined for this column type"); goto _bdone; }}'
        )
        self._line(f"        _e{n} = _T[{t + 2}];")
        self._line("    }")
        self._line(f"    int64_t _c{n} = dftu_series_length(_e{n});")
        self._line(
            f"    if (_c{n} > 0) memcpy(_v0_{attr} + _n_{attr}, dftu_series_data(_e{n}), "
            f"sizeof(double) * (size_t)_c{n});"
        )
        self._line(f"    _n_{attr} += (uint32_t)_c{n};")
        self._line("}")

    def _emit_fold(self, attr: str, elem: str, v: _V) -> None:
        s = v.args[0]
        sv = self._series(s)
        self.fold_count += 1
        n = self.fold_count
        if v.name == "mean":
            self._emit_observe_all(attr, sv, n)
            return
        self._line("{")
        guard = v.name in _NEEDS_VALUES
        if guard:
            self._line(f"    if (dftu_series_count({sv}) > 0) {{")
        pad = "        " if guard else "    "
        if v.name == "count":
            self._line(f"{pad}dftu_scalar _r{n} = dftu_jb_si(dftu_series_count({sv}));")
        else:
            self._line(f"{pad}dftu_scalar _r{n} = dftu_series_reduce({sv}, {_REDUCERS[v.name]});")
            self._line(
                f"{pad}if (_r{n}.kind == DFTU_SCALAR_TAG_ERR) {{ "
                f'_st = dftu_fail(ctx, DFTU_E_INVALID, "{v.name}() is not defined for this '
                f'column type"); goto _bdone; }}'
            )
        self._line(f"{pad}uint32_t _w{n} = _n_{attr}++;")
        if elem == "f64":
            self._line(f"{pad}_v0_{attr}[_w{n}] = dftu_jb_to_f64(_r{n});")
        else:
            self._line(f"{pad}int64_t _i{n} = 0;")
            self._line(
                f"{pad}if (!dftu_jb_to_i64(_r{n}, &_i{n})) {{ _st = dftu_fail(ctx, "
                f'DFTU_E_INVALID, "self.{attr} is an integer field and {v.name}() of this '
                f'column is a float; declare it float"); goto _bdone; }}'
            )
            self._line(f"{pad}_v0_{attr}[_w{n}] = _i{n};")
        if guard:
            self._line("    }")
        self._line("}")

    def _emit_port(self, spec: str, elem: str, v: _V) -> None:
        """Send, set or contribute one batch reduction, once per batch."""
        attr, method = spec.split(":")
        decl = self.chans[attr]
        sv = self._series(v.args[0])
        self.fold_count += 1
        n = self.fold_count
        name = decl.full
        guard = v.name in _NEEDS_VALUES
        self._line("{")
        if guard:
            self._line(f"    if (dftu_series_count({sv}) > 0) {{")
        pad = "        " if guard else "    "
        if v.name == "count":
            self._line(f"{pad}dftu_scalar _r{n} = dftu_jb_si(dftu_series_count({sv}));")
        else:
            self._line(f"{pad}dftu_scalar _r{n} = dftu_series_reduce({sv}, {_REDUCERS[v.name]});")
            self._line(
                f"{pad}if (_r{n}.kind == DFTU_SCALAR_TAG_ERR) {{ "
                f'_st = dftu_fail(ctx, DFTU_E_INVALID, "{v.name}() is not defined for this '
                f'column type"); goto _bdone; }}'
            )
        if elem == "f64":
            self._line(f"{pad}double _pv{n} = dftu_jb_to_f64(_r{n});")
        else:
            self._line(f"{pad}int64_t _pv{n} = 0;")
            self._line(
                f"{pad}if (!dftu_jb_to_i64(_r{n}, &_pv{n})) {{ _st = dftu_fail(ctx, "
                f'DFTU_E_INVALID, "self.{attr} carries an int and {v.name}() of this '
                f'column is a float; declare the port with float"); goto _bdone; }}'
            )
        self._line(f"{pad}dftu_status _s = DFTU_OK;")
        if method in ("send", "try_send"):
            self._line(f"{pad}dftu_port_status _ps = DFTU_PORT_OK;")
            self._line(
                f"{pad}_s = _prt->channel_try_send(ctx, host->h, _ch_{attr}, &_pv{n}, 8, &_ps);"
            )
            self._line(f"{pad}if (_s < 0) {{ _st = _s; goto _bdone; }}")
            if method == "send":
                msg = c_str_literal(f"jit.Error: channel '{name}' is full or closed")
                self._line(
                    f"{pad}if (_ps != DFTU_PORT_OK) {{ _st = dftu_fail(ctx, DFTU_E_INVALID, "
                    f"{msg}); goto _bdone; }}"
                )
        elif method == "set":
            self._line(f"{pad}uint64_t _gen = 0;")
            self._line(
                f"{pad}_s = _prt->latest_set(ctx, host->h, {c_str_literal(name)}, &_pv{n}, 8, &_gen);"
            )
            self._line(f"{pad}if (_s < 0) {{ _st = _s; goto _bdone; }}")
        else:
            ty = "DFTU_T_I64" if elem == "i64" else "DFTU_T_F64"
            merge = {"sum": "DFTU_MERGE_SUM", "min": "DFTU_MERGE_MIN", "max": "DFTU_MERGE_MAX"}[
                decl.merge
            ]
            msg = c_str_literal(f"jit.Error: the phase result '{name}' is sealed")
            self._line(f"{pad}dftu_port_status _ps = DFTU_PORT_OK;")
            self._line(
                f"{pad}_s = _prt->phase_contribute(ctx, host->h, {c_str_literal(name)}, 1, "
                f"{merge}, {ty}, &_pv{n}, 8, &_ps);"
            )
            self._line(f"{pad}if (_s < 0) {{ _st = _s; goto _bdone; }}")
            self._line(
                f"{pad}if (_ps != DFTU_PORT_OK) {{ _st = dftu_fail(ctx, DFTU_E_INVALID, "
                f"{msg}); goto _bdone; }}"
            )
        if guard:
            self._line("    }")
        self._line("}")


def lower_batch(
    fn: Callable[..., object],
    maps: dict[str, object],
    plains: dict[str, object],
    chans: dict[str, object] | None = None,
) -> BatchHook:
    """Lower an ``each_batch`` function to a :class:`BatchHook`; ``maps`` is
    the plugin's ``jit.map`` declarations and ``plains`` its plain fields."""
    source = Source(fn)
    try:
        tree = ast.parse(source.text)
        funcs = [n for n in tree.body if isinstance(n, ast.FunctionDef)]
        if len(funcs) != 1:
            _fail("each_batch must decorate a single method", tree.body[0])
        lowering = _Lowering(maps, plains, (), chans or {})
        source.cur = funcs[0]
        try:
            return lowering.lower(funcs[0])
        except JitError as exc:
            raise source.relocate(exc, lowering.cur) from None
    except JitError as exc:
        raise source.relocate(exc, source.cur) from None
