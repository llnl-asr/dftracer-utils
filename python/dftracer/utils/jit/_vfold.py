# SPDX-License-Identifier: MIT
"""A per-batch fold on columns."""

from __future__ import annotations

import ast
import builtins
from typing import (
    TYPE_CHECKING,
    Callable,
    List,
    cast,
)

from .._c_text import c_str_literal as _c_str_literal
from .._jit_errors import JitError, Source, class_error
from ._compiler import unwrap_index
from ._emit import emit_name_lists, quantile_name
from ._maps import AnyMap, EachBatch, Map
from ._monoids import Counter, Monoid, Quantiles
from ._plugin import JitPlugin
from ._types import C

if TYPE_CHECKING:
    pass


# ---- vfold: a per-batch fold on columns -----------------------------------

# Reducer sugar accepted on a scalar accumulator; each must match the
# accumulator's own reduction.
_VFOLD_REDUCERS = {
    "sum": "DFTU_AGG_SUM",
    "min": "DFTU_AGG_MIN",
    "max": "DFTU_AGG_MAX",
}
_VFOLD_TOP_FIELDS = frozenset({"name", "cat", "pid", "tid", "ts", "dur", "ph", "fhash", "hhash"})
# Numeric top-level columns; an ordering (by) column must be one of these.
_VFOLD_NUMERIC = frozenset({"pid", "tid", "ts", "dur", "ph"})
_VFOLD_STRING_FIELDS = frozenset({"name", "cat", "fhash", "hhash"})


def _vfold_df_field(node: ast.expr) -> str:
    if not (
        isinstance(node, ast.Subscript)
        and isinstance(node.value, ast.Name)
        and node.value.id == "df"
    ):
        raise JitError('@jit.vfold body must be self.<acc> += df["<field>"].<reducer>()')
    key = unwrap_index(node.slice)
    if not (isinstance(key, ast.Constant) and isinstance(key.value, str)):
        raise JitError('@jit.vfold: df subscript must be a string field name, df["dur"]')
    return key.value


def _vfold_specs(opd: dict[str, object]) -> list[tuple[str, str, str, str, str]]:
    """The (op, value, out, param, by) dftu_agg_col rows for one vfold op."""
    mon = cast(Monoid, opd["mon"])
    vfield = opd["vfield"]
    byfield = opd["byfield"]
    value = _c_str_literal(cast(str, vfield)) if vfield is not None else "NULL"
    by = _c_str_literal(cast(str, byfield)) if byfield is not None else "NULL"
    if isinstance(mon, Quantiles):
        specs = [("DFTU_AGG_COUNT", "NULL", _c_str_literal("count"), "0.0", "NULL")]
        for q in mon.qs:
            specs.append(("DFTU_AGG_PCT", value, _c_str_literal(quantile_name(q)), repr(q), "NULL"))
        return specs
    if vfield is None:
        return [("DFTU_AGG_COUNT", "NULL", _c_str_literal("value"), "0.0", "NULL")]
    return [(mon.dft, value, _c_str_literal("value"), repr(mon.param), by)]


def _compile_vfold(
    fn: Callable[..., object],
    accums: dict[str, Monoid],
    maps: dict[str, AnyMap],
) -> tuple[list[dict[str, object]], builtins.set[str]]:
    source = Source(fn)
    try:
        return _compile_vfold_body(source, accums, maps)
    except JitError as exc:
        raise source.relocate(exc, source.cur) from None


def _compile_vfold_body(
    source: Source,
    accums: dict[str, Monoid],
    maps: dict[str, AnyMap],
) -> tuple[list[dict[str, object]], builtins.set[str]]:
    tree = ast.parse(source.text)
    func = tree.body[0]
    source.cur = func
    if not isinstance(func, ast.FunctionDef):
        raise JitError("@jit.each_batch must decorate a function")
    ops: list[dict[str, object]] = []
    fields: builtins.set[str] = builtins.set()
    shape = (
        '@jit.vfold body must be self.<acc> += df["f"].<reducer>() '
        'or self.<map>[df["k"]] += df["v"]'
    )
    for stmt in func.body:
        source.cur = stmt
        if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Constant):
            continue  # docstring
        if not (isinstance(stmt, ast.AugAssign) and isinstance(stmt.op, ast.Add)):
            raise JitError(shape)
        tgt = stmt.target
        rhs = stmt.value
        keyfields: list[str] = []
        if (
            isinstance(tgt, ast.Subscript)
            and isinstance(tgt.value, ast.Attribute)
            and isinstance(tgt.value.value, ast.Name)
            and tgt.value.value.id == "self"
        ):
            attr = tgt.value.attr
            if attr not in maps:
                raise JitError(f"@jit.vfold: '{attr}' is not a declared jit.map")
            decl = maps[attr]
            sl = unwrap_index(tgt.slice)
            elts = builtins.list(sl.elts) if isinstance(sl, ast.Tuple) else [sl]
            keyfields = [_vfold_df_field(k) for k in elts]
            if len(keyfields) != len(decl.key_types):
                raise JitError(
                    f"@jit.vfold: '{attr}' takes {len(decl.key_types)} key columns, "
                    f"got {len(keyfields)}"
                )
            for kt, kf in zip(decl.key_types, keyfields):
                if kf not in _VFOLD_TOP_FIELDS:
                    raise JitError(f"@jit.vfold: key column '{kf}' is not a scan column")
                if (kf in _VFOLD_STRING_FIELDS) != (kt.dft == "DFTU_T_STR"):
                    raise JitError(
                        f"@jit.vfold: key column '{kf}' does not match the declared key type; "
                        "a string column needs jit.str_, a numeric one an integer type"
                    )
            mon = decl.values[0]
        elif (
            isinstance(tgt, ast.Attribute)
            and isinstance(tgt.value, ast.Name)
            and tgt.value.id == "self"
        ):
            attr = tgt.attr
            if attr not in accums:
                raise JitError(f"@jit.vfold: '{attr}' is not a declared accumulator")
            mon = accums[attr]
        else:
            raise JitError(shape)
        vfield: str | None = None
        byfield: str | None = None
        if isinstance(rhs, ast.Call):
            # Scalar sugar: df["f"].sum() / .min() / .max().
            if not (not rhs.args and isinstance(rhs.func, ast.Attribute)):
                raise JitError('@jit.vfold: scalar value must be df["<field>"].<reducer>()')
            reducer = rhs.func.attr
            if reducer not in _VFOLD_REDUCERS:
                raise JitError(f"@jit.vfold: unsupported reducer '.{reducer}()' (use sum/min/max)")
            if _VFOLD_REDUCERS[reducer] != mon.dft:
                raise JitError(
                    f"@jit.vfold: '.{reducer}()' does not match accumulator '{attr}' "
                    f"(a jit.{reducer}() accumulator)"
                )
            vfield = _vfold_df_field(rhs.func.value)
        elif mon.needs_by:
            if not (isinstance(rhs, ast.Tuple) and len(rhs.elts) == 2):
                raise JitError(
                    f"@jit.vfold: '{attr}' needs a (value, by) column pair, "
                    'e.g. self.m[df["pid"]] += df["ts"], df["dur"]'
                )
            vfield = _vfold_df_field(rhs.elts[0])
            byfield = _vfold_df_field(rhs.elts[1])
            if byfield not in _VFOLD_NUMERIC:
                raise JitError(
                    f"@jit.vfold: the by column '{byfield}' must be numeric (pid/tid/ts/dur)"
                )
        elif isinstance(rhs, ast.Constant) and isinstance(rhs.value, int):
            if not isinstance(mon, Counter) or rhs.value != 1:
                raise JitError(
                    f"@jit.vfold: '{attr}' reduces a value column; only a jit.count() "
                    "accumulator takes the constant 1"
                )
        elif isinstance(rhs, ast.Tuple):
            raise JitError(f"@jit.vfold: '{attr}' takes a single value column, not a pair")
        else:
            vfield = _vfold_df_field(rhs)
        if isinstance(mon, Counter) and vfield is not None:
            raise JitError(
                f"@jit.vfold: a jit.count() accumulator ('{attr}') counts rows; "
                "write += 1, not a column"
            )
        if vfield is not None:
            if vfield not in _VFOLD_TOP_FIELDS:
                raise JitError(f"@jit.vfold: column '{vfield}' is not a scan column")
            want_str = mon.elem == "str"
            if (vfield in _VFOLD_STRING_FIELDS) != want_str:
                raise JitError(
                    f"@jit.vfold: '{attr}' reduces a "
                    f"{'string' if want_str else 'numeric'} column, but '{vfield}' is not one"
                )
            fields.add(vfield)
        if byfield is not None:
            fields.add(byfield)
        fields.update(keyfields)
        ops.append(
            {
                "attr": attr,
                "mon": mon,
                "keyfields": keyfields,
                "vfield": vfield,
                "byfield": byfield,
            }
        )
    if not ops:
        raise JitError("@jit.vfold: the @jit.each_batch body is empty")
    return ops, fields


def _emit_vfold(
    ops: list[dict[str, object]],
    plan_duql: str | None,
) -> str:
    plan_duql_field = "plan_duql" if plan_duql is not None else "NULL"
    out: list[str] = [
        "#include <dftracer/utils/plugins/abi.h>",
        "#include <dftracer/utils/dataframe/c_api.h>",
        "",
        "#include <stdint.h>",
        "#include <stdlib.h>",
        "",
    ]
    if plan_duql is not None:
        out += [
            "static const char* plan_duql(void* self) {",
            "    (void)self;",
            f"    return {_c_str_literal(plan_duql)};",
            "}",
            "",
        ]
    name_defs, name_assigns = emit_name_lists(sorted({cast(str, opd["attr"]) for opd in ops}), [])
    out += name_defs
    out += [
        "static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {",
        "    (void)self;",
        "    *out = calloc(1, 1);",
        '    return *out ? DFTU_OK : dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
        "}",
        "",
        "static dftu_status on_batch(dftu_ctx* ctx, void* slice,",
        "                            const dftu_dataframe* df,",
        "                            const dftu_plugin_host* host) {",
        "    (void)slice;",
        "    const dftu_svc_agg* agg =",
        "        (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);",
        "    if (!agg || !agg->agg_new || !agg->agg_accumulate)",
        '        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "the host has no aggregation service");',
    ]
    for opd in ops:
        attr = cast(str, opd["attr"])
        keyfields = cast(List[str], opd["keyfields"])
        specs = _vfold_specs(opd)
        spec_rows = ", ".join("{" + ", ".join(s) + "}" for s in specs)
        out.append("    {")
        out.append(
            f"        const dftu_agg_col specs[{len(specs)}] = {{{spec_rows}}};",
        )
        if keyfields:
            key_list = ", ".join(_c_str_literal(k) for k in keyfields)
            out.append(f"        const char* keys[{len(keyfields)}] = {{{key_list}}};")
            keys_arg = "keys"
        else:
            keys_arg = "NULL"
        out += [
            "        dftu_agg* a = NULL;",
            f"        dftu_status s = agg->agg_new(ctx, host->h, {_c_str_literal(attr)}, "
            f"{keys_arg}, {len(keyfields)}u, specs, {len(specs)}u, &a);",
            "        if (s < 0) return s;",
            "        if (a) {",
            "            s = agg->agg_accumulate(ctx, host->h, a, df);",
            "            if (s < 0) return s;",
            "        }",
            "    }",
        ]
    out += [
        "    return DFTU_OK;",
        "}",
        "",
        "static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {",
        "    (void)ctx;",
        "    (void)into;",
        "    (void)other;",
        "    return DFTU_OK;",
        "}",
        "",
        "static dftu_status on_finalize(dftu_ctx* ctx, void* slice,",
        "                               const dftu_plugin_host* host, dftu_task** pending) {",
        "    (void)ctx;",
        "    (void)slice;",
        "    (void)host;",
        "    *pending = NULL;",
        "    return DFTU_OK;",
        "}",
        "",
        "static void destroy_slice(void* slice) { free(slice); }",
        "static void destroy(void* self) { (void)self; }",
        "",
        "static dftu_plugin g_plugin;",
        "",
        'DFTU_PLUGIN_INFO("dftracer.jit", NULL)',
        "",
        "DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h,",
        "                                               const dftu_value* config,",
        "                                               dftu_plugin** out) {",
        "    (void)ctx;",
        "    (void)h;",
        "    (void)config;",
        "    g_plugin.struct_size = sizeof(g_plugin);",
        "    g_plugin.self = NULL;",
        f"    g_plugin.plan_duql = {plan_duql_field};",
        "    g_plugin.make_slice = make_slice;",
        "    g_plugin.merge = merge;",
        "    g_plugin.on_finalize = on_finalize;",
        "    g_plugin.destroy_slice = destroy_slice;",
        "    g_plugin.destroy = destroy;",
        "    g_plugin.on_batch = on_batch;",
    ]
    out += name_assigns
    out += [
        "    *out = &g_plugin;",
        "    return DFTU_OK;",
        "}",
        "",
    ]
    return "\n".join(out)


def _build_vfold(cls: type[C]) -> type[C]:
    accums: dict[str, Monoid] = {}
    maps: dict[str, AnyMap] = {}
    batch: list[EachBatch] = []
    for attr, val in vars(cls).items():
        if isinstance(val, Map):
            maps[attr] = val
        elif isinstance(val, Monoid):
            accums[attr] = val
        elif isinstance(val, EachBatch):
            batch.append(val)
    for attr, m in maps.items():
        if m.is_product:
            raise class_error(
                cls,
                f"@jit.vfold: '{attr}' must be a simple jit.map(key=<type>, value=<reduction>)",
                attr,
            )
        if not m.key_types:
            raise class_error(cls, f"@jit.vfold: '{attr}' needs at least one key", attr)
    if not accums and not maps:
        raise class_error(
            cls, "@jit.vfold needs an accumulator (jit.sum()/min()/max()) or a jit.map"
        )
    if len(batch) != 1:
        raise class_error(cls, "@jit.vfold needs exactly one @jit.each_batch method")
    plan_duql = getattr(cls, "plan_duql", None)
    if plan_duql is not None and not isinstance(plan_duql, str):
        raise class_error(cls, "@jit.vfold plan_duql must be a query DSL string", "plan_duql")
    # Every batch's dyn arg columns are resolved unconditionally now (no
    # needs() negotiation), so the field set from _compile_vfold is only used
    # for its own validation, not to gate what on_batch resolves.
    ops, _fields = _compile_vfold(batch[0].fn, accums, maps)
    source = _emit_vfold(ops, plan_duql)
    setattr(cls, "_jit_plugin", JitPlugin(cls.__name__, source, {}, lowering="vfold"))
    return cls


def vfold(cls: type[C]) -> type[C]:
    """Author a vectorized fold: a per-batch fold whose body runs SIMD column
    ops on the batch and folds them into scalar accumulators or keyed maps.

    Declare scalar accumulators as a bare reduction (``jit.sum()``, ``jit.min()``,
    ...) and keyed accumulators as ``jit.map(key=<types>, value=<reduction>)``,
    plus one :func:`each_batch` method whose body is a sequence of
    ``self.<acc> += df["f"]`` (or the ``df["f"].sum()/min()/max()`` sugar) and
    ``self.<map>[df["k"]] += df["v"]`` (a multi-key map subscripts a tuple of
    columns, and ``+= 1`` counts rows into a ``jit.count()``). A reduction that
    reads a second column - argmin/argmax, top-k/bottom-k, ordered list, the
    occupancy ops and the co-moments - takes a ``(value, by)`` pair written
    ``self.<map>[df["k"]] += df["v"], df["by"]``.

    Every accumulator folds through the engine's DFTU_SVC_AGG AggState: a keyed
    map is one with key columns, a scalar accumulator one with none. The host
    merges same-named accumulators across worker slices and finalizes each to a
    native DataFrame; run it through
    :class:`dftracer.utils.plugins.Plugins` like any other jit plugin."""
    return _build_vfold(cls)
