// SPDX-License-Identifier: MIT
#include <dftracer/utils/python/core/api_methods.h>
#include <dftracer/utils/python/core/scalar_helpers.h>
#include <dftracer/utils/python/frame/series_detail.h>

#include <array>

namespace dftracer::utils::python::core::api {

namespace {

using dataframe::Series;
using frame::series_detail::Held;
using frame::series_detail::kernel;
using frame::series_detail::make_series;
using frame::series_detail::named_like;

const char* args_name(Args a) {
    switch (a) {
        case Args::None:
            return "NONE";
        case Args::Scalar:
            return "SCALAR";
        case Args::Number2:
            return "NUMBER2";
    }
    return "";
}

const char* result_name(Result r) {
    switch (r) {
        case Result::Same:
            return "SAME";
        case Result::Bool:
            return "BOOL";
        case Result::Int:
            return "INT";
        case Result::Float:
            return "FLOAT";
    }
    return "";
}

std::size_t arg_count(Args a) {
    switch (a) {
        case Args::None:
            return 0;
        case Args::Scalar:
            return 1;
        case Args::Number2:
            return 2;
    }
    return 0;
}

// The function of a row's registry op, found on first use and checked once
// against the tokens of the row: a series in, `arg_count` scalars, a series
// out. Null, with a Python error set, when the op does not match.
std::array<dftu_op_generic_fn, ROW_COUNT> g_bound{};

[[gnu::noinline]] dftu_op_generic_fn bind_fn(std::size_t id) {
    const Row& row = ROWS[id];
    const dftu_op_desc* d = dftu_op_find(row.op);
    bool ok = d && DFTU_OP_SIG_RET(d->sig) == DFTU_TOK_SERIES &&
              DFTU_OP_SIG_ARG(d->sig, 0) == DFTU_TOK_SERIES;
    const std::size_t n = arg_count(row.args);
    for (std::size_t i = 0; ok && i < 3; ++i) {
        const dftu_op_tok want = i < n ? DFTU_TOK_SCALAR : DFTU_TOK_NONE;
        ok = DFTU_OP_SIG_ARG(d->sig, i + 1) == want;
    }
    if (!ok) {
        PyErr_Format(PyExc_RuntimeError,
                     "api.def row %s.%s does not match the signature of op %s",
                     row.cls, row.name, row.op);
        return nullptr;
    }
    g_bound[id] = d->fn;
    return d->fn;
}

inline dftu_op_generic_fn bound_fn(std::size_t id) {
    if (dftu_op_generic_fn fn = g_bound[id]) return fn;
    return bind_fn(id);
}

}  // namespace

PyObject* run_op_noargs(std::size_t id, PyObject* self) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    dftu_op_generic_fn op = bound_fn(id);
    if (!op) return nullptr;
    auto fn = reinterpret_cast<dftu_series* (*)(const dftu_series*)>(op);
    return make_series(
        named_like(kernel(*a, [&] { return Series{fn(a->handle())}; }), *a));
}

PyObject* run_op(std::size_t id, PyObject* self, PyObject* args, PyObject* kw) {
    const Row& row = ROWS[id];
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    dftu_op_generic_fn op = bound_fn(id);
    if (!op) return nullptr;
    switch (row.args) {
        case Args::None:
            return run_op_noargs(id, self);
        case Args::Scalar: {
            PyObject* value = nullptr;
            const char* const names[1] = {row.kw[0]};
            if (!kw && PyTuple_GET_SIZE(args) == 1)
                value = PyTuple_GET_ITEM(args, 0);
            else if (!parse_kw<"O">(args, kw, names, &value))
                return nullptr;
            dftu_scalar s{};
            if (!py_to_scalar(value, &s)) return nullptr;
            auto fn = reinterpret_cast<dftu_series* (*)(const dftu_series*,
                                                        dftu_scalar)>(op);
            return make_series(named_like(
                kernel(*a, [&] { return Series{fn(a->handle(), s)}; }), *a));
        }
        case Args::Number2: {
            PyObject* lo = nullptr;
            PyObject* hi = nullptr;
            const char* const names[2] = {row.kw[0], row.kw[1]};
            if (!kw && PyTuple_GET_SIZE(args) == 2) {
                lo = PyTuple_GET_ITEM(args, 0);
                hi = PyTuple_GET_ITEM(args, 1);
            } else if (!parse_kw<"OO">(args, kw, names, &lo, &hi)) {
                return nullptr;
            }
            dftu_scalar slo{}, shi{};
            if (!py_to_scalar(lo, &slo) || !py_to_scalar(hi, &shi))
                return nullptr;
            auto fn =
                reinterpret_cast<dftu_series* (*)(const dftu_series*,
                                                  dftu_scalar, dftu_scalar)>(
                    op);
            return make_series(named_like(
                kernel(*a, [&] { return Series{fn(a->handle(), slo, shi)}; }),
                *a));
        }
    }
    return nullptr;
}

PyObject* rows_as_tuple() {
    PyObject* out = PyTuple_New(ROW_COUNT);
    if (!out) return nullptr;
    for (std::size_t i = 0; i < ROW_COUNT; ++i) {
        const Row& r = ROWS[i];
        const std::size_t n = arg_count(r.args);
        PyObject* kws = PyTuple_New(static_cast<Py_ssize_t>(n));
        if (!kws) {
            Py_DECREF(out);
            return nullptr;
        }
        for (std::size_t k = 0; k < n; ++k)
            PyTuple_SET_ITEM(kws, static_cast<Py_ssize_t>(k),
                             PyUnicode_FromString(r.kw[k]));
        PyObject* row =
            Py_BuildValue("(ssssssOOs)", r.cls, r.name, r.op, args_name(r.args),
                          result_name(r.result), "KEEP",
                          r.on_expr ? Py_True : Py_False, kws, r.doc);
        Py_DECREF(kws);
        if (!row) {
            Py_DECREF(out);
            return nullptr;
        }
        PyTuple_SET_ITEM(out, static_cast<Py_ssize_t>(i), row);
    }
    return out;
}

}  // namespace dftracer::utils::python::core::api
