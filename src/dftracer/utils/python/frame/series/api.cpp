// SPDX-License-Identifier: MIT
#include <dftracer/utils/python/core/api_methods.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/core/scalar_helpers.h>
#include <dftracer/utils/python/frame/series_detail.h>

#include <array>
#include <cstdint>

namespace dftracer::utils::python::core::api {

namespace {

using dataframe::Series;
using frame::series_detail::Held;
using frame::series_detail::kernel;
using frame::series_detail::make_series;
using frame::series_detail::named_like;
using frame::series_detail::scalar_to_py;
using frame::series_detail::without_gil;

const char* args_name(Args a) {
    switch (a) {
        case Args::None:
            return "NONE";
        case Args::Scalar:
            return "SCALAR";
        case Args::Number2:
            return "NUMBER2";
        case Args::Str:
            return "STR";
        case Args::Str2:
            return "STR2";
        case Args::Int:
            return "INT";
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
        case Result::Str:
            return "STR";
        case Result::Any:
            return "ANY";
        case Result::I64:
            return "I64";
        case Result::F64:
            return "F64";
        case Result::Scalar:
            return "SCALAR";
        case Result::Element:
            return "ELEMENT";
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
        case Args::Str2:
            return 2;
        case Args::Str:
        case Args::Int:
            return 1;
    }
    return 0;
}

dftu_op_tok arg_token(Args a) {
    switch (a) {
        case Args::Str:
        case Args::Str2:
            return DFTU_TOK_STR;
        case Args::Int:
            return DFTU_TOK_I64;
        default:
            return DFTU_TOK_SCALAR;
    }
}

// The function of a row's registry op, found on first use and checked once
// against the tokens of the row: a series in, `arg_count` scalars, a series
// out. Null, with a Python error set, when the op does not match.
std::array<dftu_op_generic_fn, ROW_COUNT> g_bound{};

[[gnu::noinline]] dftu_op_generic_fn bind_fn(std::size_t id) {
    const Row& row = ROWS[id];
    const dftu_op_desc* d = dftu_op_find(row.op);
    dftu_op_tok want_result = DFTU_TOK_SERIES;
    switch (row.result) {
        case Result::I64:
            want_result = DFTU_TOK_I64;
            break;
        case Result::F64:
            want_result = DFTU_TOK_F64;
            break;
        case Result::Scalar:
        case Result::Element:
            want_result = DFTU_TOK_SCALAR;
            break;
        default:
            break;
    }
    bool ok = d && DFTU_OP_SIG_RET(d->sig) == want_result &&
              DFTU_OP_SIG_ARG(d->sig, 0) == DFTU_TOK_SERIES;
    const std::size_t n = arg_count(row.args);
    for (std::size_t i = 0; ok && i < 3; ++i) {
        const dftu_op_tok want = i < n ? arg_token(row.args) : DFTU_TOK_NONE;
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

namespace {

// The Python object for the column a row's op returned: the error the row names
// when the op refused the column, and under the name of the source.
PyObject* finish(const Row& row, Series&& out, const Series& source) {
    if (!out.valid() && row.err[0] != '\0') {
        PyErr_SetString(core::g_dft_value_error, row.err);
        return nullptr;
    }
    return make_series(named_like(std::move(out), source));
}

// A str argument as the C ABI takes it; the bytes belong to the Python object.
struct StrArg {
    const char* data = nullptr;
    std::int32_t size = 0;

    bool set(PyObject* o) {
        Py_ssize_t n = 0;
        data = PyUnicode_AsUTF8AndSize(o, &n);
        size = static_cast<std::int32_t>(n);
        return data != nullptr;
    }
};

}  // namespace

PyObject* run_value_noargs(std::size_t id, PyObject* self) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    dftu_op_generic_fn op = bound_fn(id);
    if (!op) return nullptr;
    switch (ROWS[id].result) {
        case Result::I64: {
            auto fn =
                reinterpret_cast<std::int64_t (*)(const dftu_series*)>(op);
            return PyLong_FromLongLong(
                without_gil(a->length(), [&] { return fn(a->handle()); }));
        }
        case Result::F64: {
            auto fn = reinterpret_cast<double (*)(const dftu_series*)>(op);
            return PyFloat_FromDouble(
                without_gil(a->length(), [&] { return fn(a->handle()); }));
        }
        default: {
            auto fn = reinterpret_cast<dftu_scalar (*)(const dftu_series*)>(op);
            const dftu_scalar v =
                without_gil(a->length(), [&] { return fn(a->handle()); });
            // An element of a Bool column is a bool, as it is a bool in
            // `to_list`.
            if (ROWS[id].result == Result::Element &&
                v.kind == DFTU_SCALAR_TAG_I64 &&
                a->type() == dataframe::TypeId::Bool)
                return PyBool_FromLong(v.value.i != 0);
            return scalar_to_py(v);
        }
    }
}

PyObject* run_op_noargs(std::size_t id, PyObject* self) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    dftu_op_generic_fn op = bound_fn(id);
    if (!op) return nullptr;
    auto fn = reinterpret_cast<dftu_series* (*)(const dftu_series*)>(op);
    return finish(ROWS[id], kernel(*a, [&] { return Series{fn(a->handle())}; }),
                  *a);
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
            else if (!parse_kw_for<"O">(row.name, args, kw, names, &value))
                return nullptr;
            dftu_scalar s{};
            if (!py_to_scalar(value, &s)) return nullptr;
            auto fn = reinterpret_cast<dftu_series* (*)(const dftu_series*,
                                                        dftu_scalar)>(op);
            return finish(
                row, kernel(*a, [&] { return Series{fn(a->handle(), s)}; }),
                *a);
        }
        case Args::Number2: {
            PyObject* lo = nullptr;
            PyObject* hi = nullptr;
            const char* const names[2] = {row.kw[0], row.kw[1]};
            if (!kw && PyTuple_GET_SIZE(args) == 2) {
                lo = PyTuple_GET_ITEM(args, 0);
                hi = PyTuple_GET_ITEM(args, 1);
            } else if (!parse_kw_for<"OO">(row.name, args, kw, names, &lo,
                                           &hi)) {
                return nullptr;
            }
            dftu_scalar slo{}, shi{};
            if (!py_to_scalar(lo, &slo) || !py_to_scalar(hi, &shi))
                return nullptr;
            auto fn =
                reinterpret_cast<dftu_series* (*)(const dftu_series*,
                                                  dftu_scalar, dftu_scalar)>(
                    op);
            return finish(
                row,
                kernel(*a, [&] { return Series{fn(a->handle(), slo, shi)}; }),
                *a);
        }
        case Args::Str: {
            PyObject* value = nullptr;
            const char* const names[1] = {row.kw[0]};
            if (!kw && PyTuple_GET_SIZE(args) == 1)
                value = PyTuple_GET_ITEM(args, 0);
            else if (!parse_kw_for<"O">(row.name, args, kw, names, &value))
                return nullptr;
            StrArg s;
            if (!s.set(value)) return nullptr;
            auto fn =
                reinterpret_cast<dftu_series* (*)(const dftu_series*,
                                                  const char*, std::int32_t)>(
                    op);
            return finish(
                row,
                kernel(*a,
                       [&] { return Series{fn(a->handle(), s.data, s.size)}; }),
                *a);
        }
        case Args::Str2: {
            PyObject* first = nullptr;
            PyObject* second = nullptr;
            const char* const names[2] = {row.kw[0], row.kw[1]};
            if (!kw && PyTuple_GET_SIZE(args) == 2) {
                first = PyTuple_GET_ITEM(args, 0);
                second = PyTuple_GET_ITEM(args, 1);
            } else if (!parse_kw_for<"OO">(row.name, args, kw, names, &first,
                                           &second)) {
                return nullptr;
            }
            StrArg s1, s2;
            if (!s1.set(first) || !s2.set(second)) return nullptr;
            auto fn = reinterpret_cast<
                dftu_series* (*)(const dftu_series*, const char*, std::int32_t,
                                 const char*, std::int32_t)>(op);
            return finish(
                row,
                kernel(*a,
                       [&] {
                           return Series{fn(a->handle(), s1.data, s1.size,
                                            s2.data, s2.size)};
                       }),
                *a);
        }
        case Args::Int: {
            PyObject* value = nullptr;
            const char* const names[1] = {row.kw[0]};
            if (!kw && PyTuple_GET_SIZE(args) == 1)
                value = PyTuple_GET_ITEM(args, 0);
            else if (!parse_kw_for<"O">(row.name, args, kw, names, &value))
                return nullptr;
            const long long n = PyLong_AsLongLong(value);
            if (n == -1 && PyErr_Occurred()) return nullptr;
            auto fn = reinterpret_cast<dftu_series* (*)(const dftu_series*,
                                                        std::int64_t)>(op);
            return finish(
                row,
                kernel(*a,
                       [&] {
                           return Series{
                               fn(a->handle(), static_cast<std::int64_t>(n))};
                       }),
                *a);
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
            Py_BuildValue("(ssssssOOss)", r.cls, r.name, r.op,
                          args_name(r.args), result_name(r.result), "KEEP",
                          r.on_expr ? Py_True : Py_False, kws, r.err, r.doc);
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
