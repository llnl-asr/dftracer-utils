# SPDX-License-Identifier: MIT
"""The C source of a plugin from compiled bodies."""

from __future__ import annotations

import ast
import builtins
import inspect
import textwrap
from typing import (
    TYPE_CHECKING,
    Callable,
)

from .._c_text import c_str_literal as _c_str_literal
from .._jit_errors import Source
from .._jit_op import Op
from . import _channels, _flow
from ._channels import PortDecl as _PortDecl
from ._compiler import (
    ARG_HELPER_DEFS,
    COLBUF_HELPERS,
    ERROR_HELPER,
    RAW_EVENT_SHIM_POPULATE,
    RAW_EVENT_SHIM_TYPE,
    out_name,
)
from ._maps import AnyMap, CfgField, PortHandle, StoreField
from ._monoids import Quantiles
from ._types import key_type, kind_type

if TYPE_CHECKING:
    from ._batch import BatchHook


def _port_ctype(port: PortHandle) -> tuple[str, str]:
    return ("double", "0.0") if port.is_f64 else ("uint64_t", "0")


def _wire_id(port: PortHandle) -> str:
    """A port's resolved wire id: non-None by the time emit_plugin runs, since
    build_plugin resolves every publish port's name before emitting, and
    consume() never returns a port without one."""
    assert port.name is not None
    return port.name


def emit_name_lists(provides: list[str], consumes: list[str]) -> tuple[list[str], list[str]]:
    """The dftu_plugin::provides / ::consumes definitions and the two factory
    assignment lines. Both lists are derived, never author-declared: a plugin
    produces the ports it publishes and the accumulators it creates, and reads
    the ports it consumes."""
    defs: list[str] = []
    assigns: list[str] = []
    for slot, names in (("provides", provides), ("consumes", consumes)):
        if not names:
            assigns.append(f"    g_plugin.{slot} = NULL;")
            continue
        rows = ", ".join(_c_str_literal(n) for n in names)
        defs += [
            f"static const char* const _{slot}_names[{len(names) + 1}] = {{{rows}, NULL}};",
            f"static const char* const* {slot}(void* self) {{",
            "    (void)self;",
            f"    return _{slot}_names;",
            "}",
            "",
        ]
        assigns.append(f"    g_plugin.{slot} = {slot};")
    return defs, assigns


def _emit_port_pre(
    pub_ports: list[tuple[str, PortHandle]], sub_ports: list[tuple[str, PortHandle]]
) -> list[str]:
    """Batch-scoped port setup emitted before the event loop: fetch the ports
    extension, read each consume port's value for this batch, and zero each
    publish port's per-batch accumulator."""
    out: list[str] = [
        "    const dftu_svc_ports* _ports =",
        "        (const dftu_svc_ports*)host->get_service(host->h, DFTU_SVC_PORTS);",
        "    (void)_ports;",
    ]
    for name, port in sub_ports:
        ctype, zero = _port_ctype(port)
        cap = _c_str_literal(_wire_id(port))
        out += [
            f"    {ctype} _sub_{name} = {zero};",
            "    if (_ports && _ports->consume) {",
            "        uint32_t _sub_len = 0;",
            f"        const void* _sub_p = _ports->consume(host->h, _ports->port_key(host->h, {cap}), &_sub_len);",
            f"        if (_sub_p && _sub_len == 8u) std::memcpy(&_sub_{name}, _sub_p, 8);",
            "    }",
            f"    (void)_sub_{name};",
        ]
    for name, port in pub_ports:
        ctype, zero = _port_ctype(port)
        out.append(f"    {ctype} _pub_{name} = {zero};")
    return out


def _emit_port_flush(pub_ports: list[tuple[str, PortHandle]]) -> list[str]:
    """Publish each publish port's per-batch accumulator after the event loop."""
    out: list[str] = []
    for name, port in pub_ports:
        cap = _c_str_literal(_wire_id(port))
        out += [
            "    if (_ports && _ports->publish) {",
            f"        _ports->publish(host->h, _ports->port_key(host->h, {cap}), &_pub_{name}, 8u);",
            "    }",
        ]
    return out


_STR_COL_HELPER = [
    "static dftu_series* dftu_jit_str_col(const dftu_plugin_host* host, const dftu_str* ids,",
    "                                     uint32_t n) {",
    "    int32_t* off = (int32_t*)malloc(sizeof(int32_t) * (size_t)(n + 1u));",
    "    if (!off) return NULL;",
    "    uint32_t tot = 0;",
    "    for (uint32_t i = 0; i < n; ++i) {",
    "        uint32_t len = 0;",
    "        const char* s = host->resolve(host->h, ids[i], &len);",
    "        if (!s) len = 0;",
    "        off[i] = (int32_t)tot;",
    "        tot += len;",
    "    }",
    "    off[n] = (int32_t)tot;",
    "    char* buf = (char*)malloc(tot ? tot : 1u);",
    "    if (!buf) { free(off); return NULL; }",
    "    for (uint32_t i = 0; i < n; ++i) {",
    "        uint32_t len = 0;",
    "        const char* s = host->resolve(host->h, ids[i], &len);",
    "        if (s && len) std::memcpy(buf + off[i], s, len);",
    "    }",
    "    dftu_series* col = dftu_series_new_string(DFTU_TYPE_STRING, off, buf, (int64_t)n, NULL);",
    "    free(buf);",
    "    free(off);",
    "    return col;",
    "}",
    "",
]

_BITMAP_HELPER = [
    "static uint8_t* dftu_jit_bitmap(const uint8_t* flags, uint32_t n) {",
    "    uint32_t nb = (n + 7u) / 8u;",
    "    uint8_t* bm = (uint8_t*)calloc(nb ? nb : 1u, 1);",
    "    if (!bm) return NULL;",
    "    for (uint32_t i = 0; i < n; ++i)",
    "        if (flags[i]) bm[i >> 3] |= (uint8_t)(1u << (i & 7u));",
    "    return bm;",
    "}",
    "",
]


def quantile_name(q: float) -> str:
    return f"p{q * 100:g}"


def _agg_specs(decl: AnyMap) -> list[tuple[str, str, str, str, str]]:
    """The (op, value, out, param, by) dftu_agg_col rows for one accumulator."""
    specs: list[tuple[str, str, str, str, str]] = []
    for c, m in enumerate(decl.values):
        if isinstance(m, Quantiles):
            specs.append(("DFTU_AGG_COUNT", "NULL", _c_str_literal("count"), "0.0", "NULL"))
            for q in m.qs:
                specs.append(
                    (
                        "DFTU_AGG_PCT",
                        _c_str_literal(f"v{c}"),
                        _c_str_literal(quantile_name(q)),
                        repr(q),
                        "NULL",
                    )
                )
            continue
        specs.append(
            (
                m.dft,
                _c_str_literal(f"v{c}"),
                _c_str_literal(out_name(decl, c)),
                repr(m.param),
                _c_str_literal(f"b{c}") if m.needs_by else "NULL",
            )
        )
    return specs


def _emit_buffers(attr: str, decl: AnyMap, rows: int) -> list[str]:
    """Per-batch row buffers for one accumulator, sized to the batch."""
    out = [
        f"    uint32_t _cap_{attr} = (uint32_t)n * {rows}u;",
        f"    uint32_t _n_{attr} = 0;",
    ]
    for idx, kt in enumerate(decl.key_types):
        ct = key_type(kt.dft).ctype
        out.append(
            f"    {ct}* _k{idx}_{attr} = ({ct}*)malloc(sizeof({ct}) * "
            f"(size_t)(_cap_{attr} ? _cap_{attr} : 1u));"
        )
        out.append(f"    if (!_k{idx}_{attr}) _alloc_ok = 0;")
    for c, m in enumerate(decl.values):
        ct = kind_type(m.elem).ctype
        out.append(
            f"    {ct}* _v{c}_{attr} = ({ct}*)malloc(sizeof({ct}) * "
            f"(size_t)(_cap_{attr} ? _cap_{attr} : 1u));"
        )
        out.append(f"    if (!_v{c}_{attr}) _alloc_ok = 0;")
        if decl.is_product:
            out.append(
                f"    uint8_t* _o{c}_{attr} = (uint8_t*)malloc("
                f"(size_t)(_cap_{attr} ? _cap_{attr} : 1u));"
            )
            out.append(f"    if (!_o{c}_{attr}) _alloc_ok = 0;")
        if m.needs_by:
            out.append(
                f"    double* _b{c}_{attr} = (double*)malloc(sizeof(double) * "
                f"(size_t)(_cap_{attr} ? _cap_{attr} : 1u));"
            )
            out.append(f"    if (!_b{c}_{attr}) _alloc_ok = 0;")
    return out


def _emit_flush(attr: str, decl: AnyMap, wire_name: str) -> list[str]:
    """Build one batch frame from the row buffers and fold it into the
    DFTU_SVC_AGG accumulator named ``wire_name`` (the package-qualified id;
    ``attr`` only ever names local C buffers)."""
    cols: list[tuple[str, list[str]]] = []
    for idx, kt in enumerate(decl.key_types):
        dtype = key_type(kt.dft).column
        if kt.dft == "DFTU_T_STR":
            cols.append((f"k{idx}", [f"dftu_jit_str_col(host, _k{idx}_{attr}, _n_{attr})"]))
        else:
            cols.append(
                (
                    f"k{idx}",
                    [f"dftu_series_new_flat({dtype}, _k{idx}_{attr}, (int64_t)_n_{attr}, NULL)"],
                )
            )
    for c, m in enumerate(decl.values):
        dtype = kind_type(m.elem).column
        if m.elem == "str":
            cols.append((f"v{c}", [f"dftu_jit_str_col(host, _v{c}_{attr}, _n_{attr})"]))
        elif decl.is_product:
            cols.append(
                (
                    f"v{c}",
                    [
                        f"dftu_series_new_flat({dtype}, _v{c}_{attr}, (int64_t)_n_{attr}, "
                        f"_bm_{attr})"
                    ],
                )
            )
        else:
            cols.append(
                (
                    f"v{c}",
                    [f"dftu_series_new_flat({dtype}, _v{c}_{attr}, (int64_t)_n_{attr}, NULL)"],
                )
            )
        if m.needs_by:
            cols.append(
                (
                    f"b{c}",
                    [
                        f"dftu_series_new_flat(DFTU_TYPE_FLOAT64, _b{c}_{attr}, "
                        f"(int64_t)_n_{attr}, NULL)"
                    ],
                )
            )
    n_cols = len(cols)
    specs = _agg_specs(decl)
    n_keys = len(decl.key_types)
    # The accumulator is created for every batch, empty ones included, so a map
    # that never collected a row still finalizes to a zero-row result.
    out = ["    {"]
    if n_keys:
        key_list = ", ".join(_c_str_literal(f"k{i}") for i in range(n_keys))
        out.append(f"        const char* _keys[{n_keys}] = {{{key_list}}};")
        keys_arg = "_keys"
    else:
        keys_arg = "NULL"
    spec_rows = ", ".join("{" + ", ".join(s) + "}" for s in specs)
    out += [
        f"        const dftu_agg_col _specs[{len(specs)}] = {{{spec_rows}}};",
        "        dftu_agg* _a = NULL;",
        f"        dftu_status _as = _agg->agg_new(ctx, host->h, {_c_str_literal(wire_name)}, "
        f"{keys_arg}, {n_keys}u, _specs, {len(specs)}u, &_a);",
        "        if (_as < 0 && _st >= 0) _st = _as;",
        f"        if (_as >= 0 && _a && _n_{attr} > 0) {{",
        f"            dftu_series* _cols[{n_cols}];",
        f"            const char* _cnames[{n_cols}];",
    ]
    if decl.is_product:
        out.append(f"            uint8_t* _bm_{attr} = NULL;")
    ci = 0
    for cname, col_expr in cols:
        if decl.is_product and cname.startswith("v"):
            comp = cname[1:]
            out.append(f"            _bm_{attr} = dftu_jit_bitmap(_o{comp}_{attr}, _n_{attr});")
        out.append(f"            _cols[{ci}] = {col_expr[0]};")
        out.append(f"            _cnames[{ci}] = {_c_str_literal(cname)};")
        if decl.is_product and cname.startswith("v"):
            out.append(f"            free(_bm_{attr});")
            out.append(f"            _bm_{attr} = NULL;")
        ci += 1
    out += [
        "            int _cok = 1;",
        f"            for (int _ci = 0; _ci < {n_cols}; ++_ci)",
        "                if (!_cols[_ci]) _cok = 0;",
        "            if (_cok) {",
        f"                dftu_dataframe* _df = dftu_dataframe_new(_cnames, _cols, {n_cols});",
        "                if (_df) {",
        "                    _as = _agg->agg_accumulate(ctx, host->h, _a, _df);",
        "                    if (_as < 0 && _st >= 0) _st = _as;",
        "                    dftu_dataframe_free(_df);",
        "                }",
        "            } else {",
        f"                for (int _ci = 0; _ci < {n_cols}; ++_ci)",
        "                    if (_cols[_ci]) dftu_series_free(_cols[_ci]);",
        "            }",
        "        }",
        "    }",
    ]
    return out


def _emit_frees(attr: str, decl: AnyMap) -> list[str]:
    out = [f"    free(_k{idx}_{attr});" for idx in range(len(decl.key_types))]
    for c, m in enumerate(decl.values):
        out.append(f"    free(_v{c}_{attr});")
        if decl.is_product:
            out.append(f"    free(_o{c}_{attr});")
        if m.needs_by:
            out.append(f"    free(_b{c}_{attr});")
    return out


# The store codec of dftracer.utils.store in C, for int, float, str and
# tuple keys and int and float values, so a Python Namespace reads and writes
# what compiled code does.
_STORE_HELPERS = [
    "struct DftuBuf {",
    "    uint8_t* p;",
    "    uint32_t n;",
    "    uint32_t cap;",
    "    int bad;",
    "};",
    "",
    "struct DftuStoreEnv {",
    "    dftu_ctx* ctx;",
    "    void* h;",
    "    const dftu_svc_store* e;",
    "    dftu_store_snapshot_t* snap;",
    "    dftu_status st;",
    "    DftuBuf kb;",
    "    DftuBuf vb;",
    "    dftu_store_ns_t* ns[DFTU_ST_COUNT];",
    "    dftu_store_batch_t* batch[DFTU_ST_COUNT];",
    "};",
    "",
    "static void dftu_buf_byte(DftuBuf* b, uint8_t c) {",
    "    if (b->bad) return;",
    "    if (b->n == b->cap) {",
    "        uint32_t cap = b->cap ? b->cap * 2u : 32u;",
    "        uint8_t* p = (uint8_t*)realloc(b->p, cap);",
    "        if (!p) {",
    "            b->bad = 1;",
    "            return;",
    "        }",
    "        b->p = p;",
    "        b->cap = cap;",
    "    }",
    "    b->p[b->n++] = c;",
    "}",
    "",
    "static void dftu_enc_int(DftuBuf* b, int64_t v) {",
    "    if (v == 0) {",
    "        dftu_buf_byte(b, 0x14);",
    "        return;",
    "    }",
    "    uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;",
    "    int size = 0;",
    "    for (uint64_t t = mag; t; t >>= 8) ++size;",
    "    uint64_t bits = mag;",
    "    if (v > 0) {",
    "        dftu_buf_byte(b, (uint8_t)(0x14 + size));",
    "    } else {",
    "        dftu_buf_byte(b, (uint8_t)(0x14 - size));",
    "        bits = (size == 8 ? ~(uint64_t)0 : (((uint64_t)1 << (8 * size)) - 1)) - mag;",
    "    }",
    "    for (int i = size - 1; i >= 0; --i) dftu_buf_byte(b, (uint8_t)(bits >> (8 * i)));",
    "}",
    "",
    "static void dftu_enc_f64(DftuBuf* b, double v) {",
    "    uint64_t bits;",
    "    memcpy(&bits, &v, sizeof(bits));",
    "    bits = (bits >> 63) ? ~bits : (bits ^ ((uint64_t)1 << 63));",
    "    dftu_buf_byte(b, 0x21);",
    "    for (int i = 7; i >= 0; --i) dftu_buf_byte(b, (uint8_t)(bits >> (8 * i)));",
    "}",
    "",
    "static void dftu_enc_str(DftuStoreEnv* s, const dftu_plugin_host* host, DftuBuf* b,",
    "                         dftu_str id, const char* field) {",
    "    uint32_t len = 0;",
    "    const char* text = id == DFTU_STR_NONE ? NULL : host->resolve(host->h, id, &len);",
    "    if (!text) {",
    "        char msg[160];",
    '        snprintf(msg, sizeof(msg), "store %s: a store key is a missing string", field);',
    "        s->st = dftu_fail(s->ctx, DFTU_E_INVALID, msg);",
    "        return;",
    "    }",
    "    dftu_buf_byte(b, 0x02);",
    "    for (uint32_t i = 0; i < len; ++i) {",
    "        dftu_buf_byte(b, (uint8_t)text[i]);",
    "        if (text[i] == 0) dftu_buf_byte(b, 0xff);",
    "    }",
    "    dftu_buf_byte(b, 0x00);",
    "}",
    "",
    "static int dftu_dec_int(dftu_store_bytes v, int64_t* out) {",
    "    if (v.size < 1) return 0;",
    "    uint8_t t = v.data[0];",
    "    if (t == 0x14) {",
    "        *out = 0;",
    "        return v.size == 1;",
    "    }",
    "    int size = t > 0x14 ? (int)t - 0x14 : 0x14 - (int)t;",
    "    if (size < 1 || size > 8 || v.size != (uint64_t)size + 1) return 0;",
    "    uint64_t m = 0;",
    "    for (int i = 0; i < size; ++i) m = (m << 8) | v.data[1 + i];",
    "    if (t > 0x14) {",
    "        if (m > (uint64_t)INT64_MAX) return 0;",
    "        *out = (int64_t)m;",
    "        return 1;",
    "    }",
    "    uint64_t mag = (size == 8 ? ~(uint64_t)0 : (((uint64_t)1 << (8 * size)) - 1)) - m;",
    "    if (mag > ((uint64_t)1 << 63)) return 0;",
    "    *out = (int64_t)((uint64_t)0 - mag);",
    "    return 1;",
    "}",
    "",
    "static int dftu_dec_f64(dftu_store_bytes v, double* out) {",
    "    if (v.size != 9 || v.data[0] != 0x21) return 0;",
    "    uint64_t bits = 0;",
    "    for (int i = 1; i < 9; ++i) bits = (bits << 8) | v.data[i];",
    "    bits = (bits >> 63) ? (bits ^ ((uint64_t)1 << 63)) : ~bits;",
    "    memcpy(out, &bits, sizeof(bits));",
    "    return 1;",
    "}",
    "",
]


def _store_key_lines(field: StoreField, buf: str) -> list[str]:
    lines = [f"    dftu_buf_byte({buf}, 0x05);"] if field.is_tuple else []
    for idx, kind in enumerate(field.key_kinds):
        if kind == "str":
            lines.append(f"    dftu_enc_str(s, host, {buf}, k{idx}, {_c_str_literal(field.name)});")
        else:
            lines.append(f"    dftu_enc_int({buf}, k{idx});")
    if field.is_tuple:
        lines.append(f"    dftu_buf_byte({buf}, 0x00);")
    return lines


def _emit_store_defs(
    stores: dict[str, StoreField],
    gets: builtins.set[str],
    puts: builtins.set[str],
) -> list[str]:
    """File-scope C for the plugin's :class:`Store` fields: the codec, one typed
    ``get`` and ``put`` per field the body uses, and the per-batch open/close."""
    count = len(stores)
    names = ", ".join(_c_str_literal(f.name) for f in stores.values())
    put_flags = ", ".join("1" if f.name in puts else "0" for f in stores.values())
    out = [f"#define DFTU_ST_COUNT {count}", ""]
    out += _STORE_HELPERS
    out += [
        f"static const char* const _ST_NAMES[] = {{{names}}};",
        f"static const int _ST_PUT[] = {{{put_flags}}};",
        f"static const int _ST_HAS_GETS = {1 if gets else 0};",
        "",
    ]
    for f in stores.values():
        params = "".join(
            f", {'dftu_str' if k == 'str' else 'int64_t'} k{i}" for i, k in enumerate(f.key_kinds)
        )
        ctype = "double" if f.value_kind == "f64" else "int64_t"
        dec = "dftu_dec_f64" if f.value_kind == "f64" else "dftu_dec_int"
        enc = "dftu_enc_f64" if f.value_kind == "f64" else "dftu_enc_int"
        kind_name = "a float" if f.value_kind == "f64" else "an int"
        if f.name in gets:
            out += [
                f"static {ctype} _st_get_{f.name}(DftuStoreEnv* s, const dftu_plugin_host* host{params},",
                f"                         {ctype} dflt) {{",
                "    if (s->st < 0) return dflt;",
                "    s->kb.n = 0;",
                *_store_key_lines(f, "&s->kb"),
                "    if (s->st < 0) return dflt;",
                "    if (s->kb.bad) {",
                '        s->st = dftu_fail(s->ctx, DFTU_E_NO_MEMORY, "out of memory");',
                "        return dflt;",
                "    }",
                "    dftu_store_scope sc = {DFTU_STORE_SCOPE_ROOT, NULL, 0};",
                "    dftu_store_value_t* v = NULL;",
                f"    dftu_status r = s->e->get(s->ctx, s->snap, s->ns[{f.index}], &sc, s->kb.p, s->kb.n, &v);",
                "    if (r < 0) {",
                "        s->st = r;",
                "        return dflt;",
                "    }",
                "    if (!v) return dflt;",
                f"    {ctype} out = dflt;",
                f"    int ok = {dec}(s->e->value_bytes(v), &out);",
                "    s->e->value_free(v);",
                "    if (!ok) {",
                "        s->st = dftu_fail(s->ctx, DFTU_E_INVALID,",
                f'                          "store {f.name}: a stored value is not {kind_name}");',
                "        return dflt;",
                "    }",
                "    return out;",
                "}",
                "",
            ]
        if f.name in puts:
            out += [
                f"static void _st_put_{f.name}(DftuStoreEnv* s, const dftu_plugin_host* host{params},",
                f"                         {ctype} value) {{",
                "    if (s->st < 0) return;",
                "    s->kb.n = 0;",
                "    s->vb.n = 0;",
                *_store_key_lines(f, "&s->kb"),
                f"    {enc}(&s->vb, value);",
                "    if (s->st < 0) return;",
                "    if (s->kb.bad || s->vb.bad) {",
                '        s->st = dftu_fail(s->ctx, DFTU_E_NO_MEMORY, "out of memory");',
                "        return;",
                "    }",
                f"    dftu_status r = s->e->put(s->ctx, s->batch[{f.index}], s->kb.p, s->kb.n, s->vb.p, s->vb.n);",
                "    if (r < 0) s->st = r;",
                "}",
                "",
            ]
    out += [
        "static dftu_status _st_open(DftuStoreEnv* s, dftu_ctx* ctx, const dftu_plugin_host* host) {",
        "    memset(s, 0, sizeof(*s));",
        "    s->ctx = ctx;",
        "    s->h = host->h;",
        "    s->e = (const dftu_svc_store*)host->get_service(host->h, DFTU_SVC_STORE);",
        '    if (!s->e) return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "the host has no store service");',
        "    dftu_store_scope sc = {DFTU_STORE_SCOPE_ROOT, NULL, 0};",
        "    for (int i = 0; i < DFTU_ST_COUNT; ++i) {",
        "        dftu_status r = s->e->ns(ctx, host->h, _ST_NAMES[i], 1, DFTU_STORE_FOLD_NONE, 1, 1, &s->ns[i]);",
        "        if (r < 0) return r;",
        "        if (_ST_PUT[i]) {",
        "            r = s->e->batch(ctx, host->h, s->ns[i], &sc, 0, NULL, 0, &s->batch[i]);",
        "            if (r < 0) return r;",
        "        }",
        "    }",
        "    if (_ST_HAS_GETS) {",
        "        dftu_status r = s->e->snapshot(ctx, host->h, &s->snap);",
        "        if (r < 0) return r;",
        "    }",
        "    return DFTU_OK;",
        "}",
        "",
        "static void _st_close(DftuStoreEnv* s, int commit) {",
        "    if (s->e) {",
        "        for (int i = 0; i < DFTU_ST_COUNT; ++i) {",
        "            if (s->batch[i]) {",
        "                if (commit) {",
        "                    dftu_status r = s->e->commit(s->ctx, s->h, s->batch[i]);",
        "                    if (r < 0 && s->st >= 0) {",
        "                        char msg[160];",
        '                        snprintf(msg, sizeof(msg), "store %s: the commit failed (status %d)", _ST_NAMES[i], (int)r);',
        "                        s->st = dftu_fail(s->ctx, r, msg);",
        "                    }",
        "                } else {",
        "                    s->e->batch_free(s->batch[i]);",
        "                }",
        "            }",
        "            if (s->ns[i]) s->e->ns_free(s->ns[i]);",
        "        }",
        "        if (s->snap) s->e->snapshot_free(s->snap);",
        "    }",
        "    free(s->kb.p);",
        "    free(s->vb.p);",
        "}",
        "",
    ]
    return out


def emit_plugin(
    maps: dict[str, AnyMap],
    body: list[str],
    fields_used: builtins.set[str],
    plan_duql: str | None,
    str_literals: list[str],
    arg_keys: list[str],
    arg_helpers: builtins.set[str],
    rows_per_event: dict[str, int],
    op_defs: list[str] | None = None,
    ports: dict[str, PortHandle] | None = None,
    config_fields: dict[str, CfgField] | None = None,
    raw_event: bool = False,
    map_ids: dict[str, str] | None = None,
    stores: dict[str, StoreField] | None = None,
    store_gets: builtins.set[str] | None = None,
    store_puts: builtins.set[str] | None = None,
    mask_guard: str | None = None,
    batch: BatchHook | None = None,
    can_raise: bool = False,
    flow_uses: builtins.set[str] | None = None,
    res_decls: list[str] | None = None,
    res_frees: list[str] | None = None,
    flow_defs: list[str] | None = None,
    finalize_lines: list[str] | None = None,
    chans: dict[str, _PortDecl] | None = None,
    chan_uses: builtins.set[str] | None = None,
    plugin_name: str = "dftracer.jit",
) -> str:
    op_defs = op_defs or []
    stores = stores or {}
    ports = ports or {}
    chans = chans or {}
    chan_uses = chan_uses or builtins.set()
    flow_uses = flow_uses if flow_uses is not None else builtins.set()
    res_decls = res_decls or []
    res_frees = res_frees or []
    has_flows = finalize_lines is not None and flow_defs is not None
    needs_stats = (
        "stats" in flow_uses
        or has_flows
        or builtins.any(u in flow_uses for u in ("json", "yaml", "task"))
    )
    map_ids = map_ids if map_ids is not None else {attr: attr for attr in maps}
    pub_ports = [(n, p) for n, p in ports.items() if p.role == "publish"]
    sub_ports = [(n, p) for n, p in ports.items() if p.role == "consume"]
    needs_str = builtins.any(
        kt.dft == "DFTU_T_STR" for decl in maps.values() for kt in decl.key_types
    ) or builtins.any(m.elem == "str" for decl in maps.values() for m in decl.values)
    needs_bitmap = builtins.any(decl.is_product for decl in maps.values())
    plan_duql_field = "plan_duql" if plan_duql is not None else "NULL"
    out: list[str] = [
        "#include <dftracer/utils/plugins/abi.h>",
        "#include <dftracer/utils/plugins/prims.h>",
        "#include <dftracer/utils/dataframe/c_api.h>",
        "",
        "#include <cstring>",
        *(["#include <atomic>"] if needs_stats else []),
        "#include <stdint.h>",
        "#include <stdio.h>",
        "#include <stdlib.h>",
        "",
    ]
    if fields_used or arg_keys or raw_event:
        out += COLBUF_HELPERS
    if raw_event:
        out += RAW_EVENT_SHIM_TYPE
    for method in ("arg_i64", "arg_f64", "arg_str"):
        if method in arg_helpers:
            out += ARG_HELPER_DEFS[method]
    if needs_str:
        out += _STR_COL_HELPER
    if needs_bitmap:
        out += _BITMAP_HELPER
    if can_raise or has_flows:
        out += ERROR_HELPER
    if "cap" in flow_uses or has_flows:
        out += _flow.C_CAPTURE
    if needs_stats:
        out += _flow.C_STATS
    if builtins.any(u in flow_uses for u in ("json", "yaml", "task")) or has_flows:
        out += _flow.C_VALUE_HELPERS
    if _channels.channels_of(chans):
        out += _channels.C_PORT_HELPERS
    if stores:
        out += _emit_store_defs(stores, store_gets or builtins.set(), store_puts or builtins.set())
    for fn_src in op_defs:
        out += [fn_src, ""]
    cfgs = config_fields if config_fields is not None else {}
    str_cfgs = [f for f in cfgs.values() if f.kind == "str"]
    # Config and interned literals belong to one loaded instance (DftuInst) and
    # one worker slice (DftuSlice), never to file-scope statics: two instances of
    # one library keep their own values, and slices need no lock.
    if cfgs:
        out.append("struct DftuCfg {")
        out += [f"    {m}" for f in cfgs.values() for m in f.members()]
        out += [
            "};",
            "",
            "struct DftuInst {",
            "    dftu_plugin plugin;",
            "    DftuCfg cfg;",
            "};",
            "",
        ]
    has_slice = bool(str_literals or cfgs)
    if has_slice:
        out += [
            "struct DftuSlice {",
            *(["    const DftuCfg* cfg;"] if cfgs else []),
            "    int resolved;",
            f"    dftu_str lit_ids[{builtins.max(len(str_literals), 1)}];",
            f"    dftu_str cfg_ids[{builtins.max(len(str_cfgs), 1)}];",
            "};",
            "",
        ]
    # A plugin that reads config declares its keys, so the host rejects a
    # config key this plugin does not read instead of ignoring it. A plugin
    # with no config fields declares nothing and stays unvalidated.
    if cfgs:
        out.append("static const dftu_config_key g_config_keys[] = {")
        for f in cfgs.values():
            out.append(
                f"    {{{_c_str_literal(f.name)}, {f.value_kind()}, {int(f.required)}, NULL}},"
            )
        out += [
            "    {NULL, DFTU_CONFIG_ANY, 0, NULL},",
            "};",
            "",
            "static const dftu_config_key* config_keys(void* self) {",
            "    (void)self;",
            "    return g_config_keys;",
            "}",
            "",
        ]
    # Config reads for the factory: one block per field, defaults from the dataclass.
    cfg_reads = "\n".join(
        ["    (void)config;"]
        + (
            [
                "    DftuInst* inst = (DftuInst*)calloc(1, sizeof(DftuInst));",
                '    if (!inst) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
                "    DftuCfg* _c = &inst->cfg;",
            ]
            if cfgs
            else []
        )
        + [ln for f in cfgs.values() for ln in f.factory_reads()]
        + (
            ["    dftu_plugin& g_plugin = inst->plugin;", "    g_plugin.self = inst;"]
            if cfgs
            else ["    g_plugin.self = NULL;"]
        )
    )
    if plan_duql is not None:
        out += [
            "static const char* plan_duql(void* self) {",
            "    (void)self;",
            f"    return {_c_str_literal(plan_duql)};",
            "}",
            "",
        ]
    # The batch columns this plugin reads are exactly the ones on_batch
    # resolves, which the compiler already knows. Declaring them projects the
    # batch, so a trace with many arg keys does not build a Series per key for
    # a plugin that never looks at them. A raw body is not AST-analyzable, so
    # it declares nothing and keeps the whole batch.
    read_cols = [] if raw_event else sorted(fields_used) + [f"args.{a}" for a in sorted(arg_keys)]
    if batch is not None:
        read_cols = batch.columns
    if read_cols:
        out.append("static const char* const g_reads[] = {")
        for col in read_cols:
            out.append(f"    {_c_str_literal(col)},")
        out += [
            "    NULL,",
            "};",
            "",
            "static const char* const* reads(void* self) {",
            "    (void)self;",
            "    return g_reads;",
            "}",
            "",
        ]

    name_defs, name_assigns = emit_name_lists(
        sorted(
            [_wire_id(p) for _, p in pub_ports]
            + [map_ids[attr] for attr in maps]
            + _channels.provided_names(chans)
        ),
        sorted([_wire_id(p) for _, p in sub_ports] + _channels.consumed_names(chans)),
    )
    out += name_defs
    if has_slice:
        out += [
            "static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {",
            "    (void)self;",
            "    DftuSlice* s = (DftuSlice*)calloc(1, sizeof(DftuSlice));",
            '    if (!s) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
        ]
        if cfgs:
            out.append("    s->cfg = &((DftuInst*)self)->cfg;")
        out += ["    *out = s;", "    return DFTU_OK;", "}", ""]
    else:
        out += [
            "static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {",
            "    (void)self;",
            "    *out = calloc(1, 1);",
            '    return *out ? DFTU_OK : dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");',
            "}",
            "",
        ]
    out += [
        "static dftu_status on_batch(dftu_ctx* ctx, void* slice, const dftu_dataframe* df,",
        "                            const dftu_plugin_host* host) {",
        "    (void)slice;",
        "    (void)ctx;",
        "    dftu_status _st = DFTU_OK;",
        "    const dftu_svc_agg* _agg =",
        "        (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);",
    ]
    if maps:
        out.append(
            "    if (!_agg || !_agg->agg_new || !_agg->agg_accumulate)"
            ' return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "the host has no aggregation service");'
        )
    else:
        out.append("    (void)_agg;")
    if stores:
        out += [
            "    DftuStoreEnv _se;",
            "    {",
            "        dftu_status _so = _st_open(&_se, ctx, host);",
            "        if (_so < 0) _st = _so;",
            "    }",
        ]
    out.append("    int64_t n = dftu_dataframe_num_rows(df);")
    if has_slice:
        out.append("    DftuSlice* _sl = (DftuSlice*)slice;")
        if cfgs:
            out.append("    const DftuCfg* _c = _sl->cfg;")
        out.append("    if (!_sl->resolved) {")
        for idx, lit in enumerate(str_literals):
            blen = len(lit.encode("utf-8"))
            out.append(
                f"        _sl->lit_ids[{idx}] = host->intern(host->h, {_c_str_literal(lit)}, {blen});"
            )
        for idx, f in enumerate(str_cfgs):
            out.append(
                f"        _sl->cfg_ids[{idx}] = host->intern(host->h, _c->v_{f.name}, _c->l_{f.name});"
            )
        out += ["        _sl->resolved = 1;", "    }"]
        for idx, _ in enumerate(str_literals):
            out.append(f"    const dftu_str lit_{idx} = _sl->lit_ids[{idx}];")
            out.append(f"    (void)lit_{idx};")
        for idx, f in enumerate(str_cfgs):
            out.append(f"    const dftu_str _cfgs_{f.name} = _sl->cfg_ids[{idx}];")
            out.append(f"    (void)_cfgs_{f.name};")
        for f in cfgs.values():
            out += f.locals_()
    for field in sorted(fields_used):
        out.append(
            f"    dftu_jit_col _col_{field} = dftu_jit_resolve(df, {_c_str_literal(field)});"
        )
    for idx, argname in enumerate(arg_keys):
        col_name = _c_str_literal(f"args.{argname}")
        out.append(f"    dftu_jit_col _argcol{idx} = dftu_jit_resolve(df, {col_name});")
    out.append("    int _alloc_ok = 1;")
    if can_raise:
        out += ["    int _unw = 0;", "    (void)_unw;"]
    if "cap" in flow_uses:
        out += ["    DftuCap _cap;", "    dftu_cap_init(&_cap, ctx);"]
    for svc, ctype, ident, word in (
        ("fileio", "dftu_svc_fileio", "DFTU_SVC_FILEIO", "file"),
        ("task", "dftu_svc_task", "DFTU_SVC_TASK", "task"),
        ("json", "dftu_svc_format", "DFTU_SVC_JSON", "JSON"),
        ("yaml", "dftu_svc_format", "DFTU_SVC_YAML", "YAML"),
        ("ports", "dftu_svc_ports", "DFTU_SVC_PORTS", "ports"),
    ):
        if svc in flow_uses:
            var = {
                "fileio": "_fio",
                "task": "_tsk",
                "json": "_fmtj",
                "yaml": "_fmty",
                "ports": "_prt",
            }[svc]
            out += [
                f"    const {ctype}* {var} = (const {ctype}*)host->get_service(host->h, {ident});",
                f'    if (!{var} && _st >= 0) _st = dftu_fail(ctx, DFTU_E_UNSUPPORTED, "the host has no {word} service");',
            ]
    out += ["    " + ln for ln in res_decls]
    for attr, decl in maps.items():
        out += _emit_buffers(attr, decl, builtins.max(1, rows_per_event.get(attr, 1)))
    out.append("    if (_alloc_ok && _st >= 0) {")
    inner: list[str] = []
    if ports:
        inner += _emit_port_pre(pub_ports, sub_ports)
    inner += _channels.event_open_lines(chans, chan_uses)
    if batch is not None:
        inner.append("    if (n > 0) {")
        inner += ["        " + ln for ln in batch.lines]
        inner.append("    }")
    else:
        if mask_guard is not None:
            inner += [
                "    uint32_t* _sel = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)(n > 0 ? n : 1));",
                "    if (!_sel) _alloc_ok = 0;",
                "    int64_t _ns = 0;",
                "    for (int64_t i = 0; _sel && i < n; ++i) {",
                "        _sel[_ns] = (uint32_t)i;",
                f"        _ns += ({mask_guard}) ? 1 : 0;",
                "    }",
                "    for (int64_t _j = 0; _j < _ns; ++_j) {",
                "        const int64_t i = (int64_t)_sel[_j];",
                "        (void)i;",
            ]
        else:
            inner.append("    for (int64_t i = 0; i < n; ++i) {")
        if raw_event:
            inner += ["        " + ln for ln in RAW_EVENT_SHIM_POPULATE]
        for line in body:
            inner.append("        " + line)
        if stores:
            inner.append("        if (_se.st < 0) break;")
        inner.append("    }")
        if can_raise:
            inner.append("_ev_exit: ;")
            inner += ["    " + ln for ln in res_frees]
        if mask_guard is not None:
            inner.append("    free(_sel);")
    tail: list[str] = []
    if ports:
        tail += _emit_port_flush(pub_ports)
    for attr, decl in maps.items():
        tail += _emit_flush(attr, decl, map_ids[attr])
    if can_raise and tail:
        inner += ["    if (_st >= 0) {"] + ["    " + ln for ln in tail] + ["    }"]
    else:
        inner += tail
    out += ["    " + ln for ln in inner]
    out.append("    }")
    if stores:
        out += [
            "    _st_close(&_se, _st >= 0 && _se.st >= 0);",
            "    if (_st >= 0 && _se.st < 0) _st = _se.st;",
        ]
    out.append('    if (!_alloc_ok) _st = dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");')
    for attr, decl in maps.items():
        out += _emit_frees(attr, decl)
    for field in sorted(fields_used):
        out.append(f"    dftu_series_free(_col_{field}.handle);")
    for idx in range(len(arg_keys)):
        out.append(f"    dftu_series_free(_argcol{idx}.handle);")
    out.extend(
        [
            "    return _st;",
            "}",
            "",
            "static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {",
            "    (void)ctx;",
            "    (void)into;",
            "    (void)other;",
            "    return DFTU_OK;",
            "}",
            "",
        ]
        + (
            (flow_defs or []) + (finalize_lines or [])
            if has_flows
            else _channels.close_without_finalize(chans)
        )
        + [
            "static void destroy_slice(void* slice) { free(slice); }",
            "",
        ]
        + (
            ["static void destroy(void* self) {", "    DftuInst* inst = (DftuInst*)self;"]
            + [f"    free(inst->cfg.v_{f.name});" for f in str_cfgs]
            + ["    free(inst);", "}", ""]
            if cfgs
            else [
                "static void destroy(void* self) { (void)self; }",
                "",
                "static dftu_plugin g_plugin;",
                "",
            ]
        )
        + [
            f"DFTU_PLUGIN_INFO({_c_str_literal(plugin_name)}, NULL)",
            "",
            "DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx, dftu_plugin_host* h,",
            "                                               const dftu_value* config,",
            "                                               dftu_plugin** out) {",
            "    (void)ctx;",
            "    (void)h;",
            *_channels.factory_lines(chans),
            cfg_reads,
            "    g_plugin.struct_size = sizeof(g_plugin);",
            f"    g_plugin.plan_duql = {plan_duql_field};",
            "    g_plugin.make_slice = make_slice;",
            "    g_plugin.on_batch = on_batch;",
            "    g_plugin.merge = merge;",
            "    g_plugin.on_finalize = on_finalize;",
            "    g_plugin.destroy_slice = destroy_slice;",
            "    g_plugin.destroy = destroy;",
        ]
        + (["    g_plugin.config_keys = config_keys;"] if cfgs else [])
        + (["    g_plugin.reads = reads;"] if read_cols else [])
        + name_assigns
    )
    out.extend(
        [
            "    *out = &g_plugin;",
            "    return DFTU_OK;",
            "}",
            "",
        ]
    )
    return "\n".join(out)


# Row budget per event per accumulator for a raw each_event body; the emitted
# buffers hold this many rows per scanned event and a raw body must not append
# more (there is no AST to count its contributions from).
RAW_ROWS_PER_EVENT = 4


def raw_body(fn: Callable[..., object]) -> list[str]:
    argc = getattr(fn, "__code__").co_argcount
    result = fn(*([None] * argc))
    if not isinstance(result, str):
        source = Source(fn)
        raise source.error(
            "@jit.each_event(raw=True) method must return a C++ body string",
            ast.parse(source.text).body[0],
        )
    return textwrap.dedent(result).splitlines()


def referenced_ops(fn: Callable[..., object]) -> dict[str, Op]:
    """The ``@jit.op`` transforms the each_event body can reach by name, from its
    globals and closure - so calling one inlines that op into the plugin."""
    try:
        cv = inspect.getclosurevars(fn)
    except TypeError:
        return {}
    scope = {**cv.globals, **cv.nonlocals}
    return {name: val for name, val in scope.items() if isinstance(val, Op)}
