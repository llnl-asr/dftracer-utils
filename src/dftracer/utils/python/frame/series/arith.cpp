// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/kernels/kernels.h>
#include <dftracer/utils/dataframe/kernels/prims.h>
#include <dftracer/utils/dataframe/kernels/string_ops.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/core/scalar_helpers.h>
#include <dftracer/utils/python/frame/series_detail.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::python::frame::series_detail {

namespace {

bool is_text(TypeId t) {
    return t == TypeId::String || t == TypeId::LargeString;
}

// Arithmetic between two dtypes that promotion cannot reconcile (e.g. a
// non-numeric column) is a dtype mismatch, not the generic "null column"
// failure other kernels use - name both dtypes so the caller knows what to
// cast.
PyObject* make_arith_series(Series&& col, TypeId a_type, TypeId b_type) {
    if (!col.valid()) {
        PyErr_Format(PyExc_TypeError, "unsupported operand dtypes: %s and %s",
                     dataframe::type_name(a_type),
                     dataframe::type_name(b_type));
        return nullptr;
    }
    return make_series(std::move(col));
}

// A String column plus a str, either side, as pandas concatenates them. The
// str is repeated into a column, a copy that str_cat needs; it takes the
// column's row labels so the two line up.
PyObject* text_add_str(PyObject* self, PyObject* text, bool text_first) {
    Held held(self);
    Series* a = held.get();
    if (!a) return nullptr;
    Py_ssize_t size = 0;
    const char* p = PyUnicode_AsUTF8AndSize(text, &size);
    if (!p) return nullptr;
    const dstd::StringView piece(p, static_cast<std::size_t>(size));
    Series out =
        kernel(*a, [&] { return dataframe::str_affix(*a, piece, text_first); });
    return make_arith_series(named_like(std::move(out), *a), a->type(),
                             TypeId::String);
}

// The operands of a Series-Series op aligned on their row labels as pandas
// does (see dataframe::align_labels): `a` and `b` are repointed at the aligned
// pair, kept in `store`. Two positional operands stay as they are. False with
// a ValueError set when the labels cannot align.
bool align_operands(Series*& a, Series*& b, dataframe::AlignedSeries& store) {
    if (!a->has_index() && !b->has_index()) return true;
    auto aligned = dataframe::align_labels(*a, *b);
    if (!aligned) {
        PyErr_SetString(core::g_dft_value_error, aligned.error().c_str());
        return false;
    }
    store = std::move(*aligned);
    a = &store.left;
    b = &store.right;
    return true;
}

// A Python number as either an i64 or an f64 scalar; the kernel converts it to
// the column's element type, promoting to Float64 when the scalar is a float
// and the column is not (numpy's weak-scalar rule).
enum class ScalarOp { Add, Sub, Mul, Div };

template <class T>
PyObject* scalar_apply(Series* a, ScalarOp op, T v, TypeId out_type) {
    const TypeId a_type = a->type();
    if (op == ScalarOp::Mul)
        return make_arith_series(
            named_like(kernel(*a, [&] { return dataframe::mul_scalar(*a, v); }),
                       *a),
            a_type, out_type);
    if (op == ScalarOp::Div)
        return make_arith_series(
            named_like(kernel(*a, [&] { return dataframe::div_scalar(*a, v); }),
                       *a),
            a_type, out_type);
    return make_arith_series(
        named_like(kernel(*a,
                          [&] {
                              return dataframe::add_scalar(
                                  *a, op == ScalarOp::Sub ? -v : v);
                          }),
                   *a),
        a_type, out_type);
}

PyObject* scalar_op(PyObject* self, PyObject* value, ScalarOp op) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    if (PyFloat_Check(value)) {
        double v = PyFloat_AsDouble(value);
        if (v == -1.0 && PyErr_Occurred()) return nullptr;
        return scalar_apply(a, op, v, TypeId::Float64);
    }
    long long v = PyLong_AsLongLong(value);
    if (v == -1 && PyErr_Occurred()) return nullptr;
    return scalar_apply(a, op, static_cast<std::int64_t>(v), TypeId::Int64);
}

Series add_or_concat(const Series& a, const Series& b) {
    if (is_text(a.type()) && is_text(b.type())) return a.str_cat(b);
    return dataframe::add(a, b);
}

template <class Fn>
PyObject* binary_op(PyObject* self, PyObject* other, Fn fn) {
    Held held_a(self);
    Series* a = held_a.get();
    Held held_b(other);
    Series* b = held_b.get();
    if (!a || !b) return nullptr;
    dataframe::AlignedSeries aligned;
    if (!align_operands(a, b, aligned)) return nullptr;
    return make_arith_series(
        named_like(kernel(*a, [&] { return fn(*a, *b); }), *a, *b), a->type(),
        b->type());
}

}  // namespace

#define DFTRACER_UTILS_SERIES_BINARY(name, fn)                 \
    PyObject* Series_##name(PyObject* self, PyObject* other) { \
        return binary_op(self, other, fn);                     \
    }
DFTRACER_UTILS_SERIES_BINARY(add, add_or_concat)
DFTRACER_UTILS_SERIES_BINARY(sub, dataframe::sub)
DFTRACER_UTILS_SERIES_BINARY(mul, dataframe::mul)
DFTRACER_UTILS_SERIES_BINARY(div, dataframe::div)
#undef DFTRACER_UTILS_SERIES_BINARY

// floordiv / mod / pow: `other` is a Series, or a number with the optional
// `reverse` flag (`number <op> self`).
enum class ExtraOp { FloorDiv, Mod, Pow };

static PyObject* extra_apply(PyObject* self, PyObject* other, ExtraOp op,
                             bool reverse) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    Held held_b(other, Held::Optional);
    if (Series* b = held_b.get()) {
        dataframe::AlignedSeries aligned;
        if (!align_operands(a, b, aligned)) return nullptr;
        Series out = kernel(*a, [&] {
            return op == ExtraOp::FloorDiv ? dataframe::floordiv(*a, *b)
                   : op == ExtraOp::Mod    ? dataframe::mod(*a, *b)
                                           : dataframe::pow(*a, *b);
        });
        return make_arith_series(named_like(std::move(out), *a, *b), a->type(),
                                 b->type());
    }
    const bool rev = reverse;
    if (PyFloat_Check(other)) {
        const double v = PyFloat_AsDouble(other);
        if (v == -1.0 && PyErr_Occurred()) return nullptr;
        Series out = kernel(*a, [&] {
            return op == ExtraOp::FloorDiv
                       ? dataframe::floordiv_scalar(*a, v, rev)
                   : op == ExtraOp::Mod ? dataframe::mod_scalar(*a, v, rev)
                                        : dataframe::pow_scalar(*a, v, rev);
        });
        return make_arith_series(named_like(std::move(out), *a), a->type(),
                                 TypeId::Float64);
    }
    const long long v = PyLong_AsLongLong(other);
    if (v == -1 && PyErr_Occurred()) return nullptr;
    const auto iv = static_cast<std::int64_t>(v);
    Series out = kernel(*a, [&] {
        return op == ExtraOp::FloorDiv ? dataframe::floordiv_scalar(*a, iv, rev)
               : op == ExtraOp::Mod    ? dataframe::mod_scalar(*a, iv, rev)
                                       : dataframe::pow_scalar(*a, iv, rev);
    });
    return make_arith_series(named_like(std::move(out), *a), a->type(),
                             TypeId::Int64);
}

static PyObject* extra_op(PyObject* self, PyObject* args, ExtraOp op) {
    PyObject* other = nullptr;
    int reverse = 0;
    if (!PyArg_ParseTuple(args, "O|p", &other, &reverse)) return nullptr;
    return extra_apply(self, other, op, reverse != 0);
}

PyObject* Series_add_scalar(PyObject* self, PyObject* v) {
    return scalar_op(self, v, ScalarOp::Add);
}
PyObject* Series_sub_scalar(PyObject* self, PyObject* v) {
    return scalar_op(self, v, ScalarOp::Sub);
}
PyObject* Series_mul_scalar(PyObject* self, PyObject* v) {
    return scalar_op(self, v, ScalarOp::Mul);
}
PyObject* Series_div_scalar(PyObject* self, PyObject* v) {
    return scalar_op(self, v, ScalarOp::Div);
}
PyObject* Series_floordiv(PyObject* self, PyObject* args) {
    return extra_op(self, args, ExtraOp::FloorDiv);
}
PyObject* Series_mod(PyObject* self, PyObject* args) {
    return extra_op(self, args, ExtraOp::Mod);
}
PyObject* Series_pow(PyObject* self, PyObject* args) {
    return extra_op(self, args, ExtraOp::Pow);
}

PyObject* Series_cast(PyObject* self, PyObject* target) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long tid = PyLong_AsLong(target);
    if (tid == -1 && PyErr_Occurred()) return nullptr;
    const TypeId to = static_cast<TypeId>(tid);
    Series out =
        named_like(kernel(*a, [&] { return dataframe::cast(*a, to); }), *a);
    if (!out.valid()) {
        // The engine refused the pair: say which, not "null column".
        PyErr_Format(PyExc_TypeError, "astype: cannot cast %s to %s",
                     dataframe::type_name(a->type()), dataframe::type_name(to));
        return nullptr;
    }
    return make_series(std::move(out));
}

PyObject* Series_prim(PyObject* self, PyObject* code) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long op = PyLong_AsLong(code);
    if (op == -1 && PyErr_Occurred()) return nullptr;
    return make_series(named_like(
        kernel(*a,
               [&] {
                   return dataframe::prim(*a, static_cast<dataframe::Prim>(op));
               }),
        *a));
}

// Compare a column against a scalar (op is a DFTU_CMP_* code) -> Bool
// column.
PyObject* Series_compare(PyObject* self, PyObject* args) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    int op = 0;
    PyObject* value = nullptr;
    if (!PyArg_ParseTuple(args, "iO", &op, &value)) return nullptr;
    dftu_scalar s{};
    if (!py_to_scalar(value, &s)) return nullptr;
    return make_series(named_like(
        kernel(*a,
               [&] {
                   return Series{dftu_series_compare(
                       a->handle(), static_cast<dftu_cmp_op>(op), s)};
               }),
        *a));
}

// Boolean AND/OR of two Bool columns (op is a DFTU_LOGICAL_* code).
PyObject* Series_logical(PyObject* self, PyObject* args) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    int op = 0;
    PyObject* other = nullptr;
    if (!PyArg_ParseTuple(args, "iO", &op, &other)) return nullptr;
    Held held_b(other);
    Series* b = held_b.get();
    if (!b) return nullptr;
    dataframe::AlignedSeries aligned;
    if (!align_operands(a, b, aligned)) return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_logical(
                                  a->handle(), b->handle(),
                                  static_cast<dftu_logical_op>(op))};
                          }),
                   *a, *b));
}

namespace {

// The numbers a Series operator takes as the other operand: an int or a float,
// not a bool.
bool is_number(PyObject* o) {
    return (PyLong_Check(o) || PyFloat_Check(o)) && !PyBool_Check(o);
}

enum class Bin { Add, Sub, Mul, TrueDiv, FloorDiv, Mod, Pow };

bool is_integral(TypeId t) {
    switch (t) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            return true;
        default:
            return false;
    }
}

// An integral column widened to Float64, as true division does first; any
// other column unchanged. Returns a new reference.
PyObject* widened(PyObject* o) {
    Held held(o);
    Series* a = held.get();
    if (!a) return nullptr;
    if (!is_integral(a->type())) {
        Py_INCREF(o);
        return o;
    }
    return make_series(named_like(
        kernel(*a, [&] { return dataframe::cast(*a, TypeId::Float64); }), *a));
}

PyObject* true_divide(PyObject* self, PyObject* other) {
    if (as_native_or_null<Series>(other)) {
        PyObject* me = widened(self);
        PyObject* you = me ? widened(other) : nullptr;
        PyObject* out = you ? Series_div(me, you) : nullptr;
        Py_XDECREF(me);
        Py_XDECREF(you);
        return out;
    }
    if (!is_number(other)) Py_RETURN_NOTIMPLEMENTED;
    PyObject* me = widened(self);
    if (!me) return nullptr;
    PyObject* divisor = PyFloat_Check(other)
                            ? (Py_INCREF(other), other)
                            : PyFloat_FromDouble(PyLong_AsDouble(other));
    PyObject* out = divisor ? scalar_op(me, divisor, ScalarOp::Div) : nullptr;
    Py_DECREF(me);
    Py_XDECREF(divisor);
    return out;
}

PyObject* binary(PyObject* left, PyObject* right, Bin op) {
    const bool reflected = !as_native_or_null<Series>(left);
    PyObject* self = reflected ? right : left;
    PyObject* other = reflected ? left : right;
    if (!as_native_or_null<Series>(self)) Py_RETURN_NOTIMPLEMENTED;
    if (as_native_or_null<Series>(other)) {
        switch (op) {
            case Bin::Add:
                return Series_add(self, other);
            case Bin::Sub:
                return Series_sub(self, other);
            case Bin::Mul:
                return Series_mul(self, other);
            case Bin::TrueDiv:
                return true_divide(self, other);
            case Bin::FloorDiv:
                return extra_apply(self, other, ExtraOp::FloorDiv, false);
            case Bin::Mod:
                return extra_apply(self, other, ExtraOp::Mod, false);
            case Bin::Pow:
                return extra_apply(self, other, ExtraOp::Pow, false);
        }
    }
    if (op == Bin::Add && PyUnicode_Check(other)) {
        Held held(self);
        Series* s = held.get();
        if (s && is_text(s->type()))
            return text_add_str(self, other, reflected);
        if (!s) return nullptr;
    }
    if (!is_number(other)) Py_RETURN_NOTIMPLEMENTED;
    switch (op) {
        case Bin::Add:
            return scalar_op(self, other, ScalarOp::Add);
        case Bin::Mul:
            return scalar_op(self, other, ScalarOp::Mul);
        case Bin::Sub: {
            if (!reflected) return scalar_op(self, other, ScalarOp::Sub);
            PyObject* minus_one = PyLong_FromLong(-1);
            if (!minus_one) return nullptr;
            PyObject* negated = scalar_op(self, minus_one, ScalarOp::Mul);
            Py_DECREF(minus_one);
            if (!negated) return nullptr;
            PyObject* out = scalar_op(negated, other, ScalarOp::Add);
            Py_DECREF(negated);
            return out;
        }
        case Bin::TrueDiv:
            if (reflected) Py_RETURN_NOTIMPLEMENTED;
            return true_divide(self, other);
        case Bin::FloorDiv:
            return extra_apply(self, other, ExtraOp::FloorDiv, reflected);
        case Bin::Mod:
            return extra_apply(self, other, ExtraOp::Mod, reflected);
        case Bin::Pow:
            return extra_apply(self, other, ExtraOp::Pow, reflected);
    }
    Py_RETURN_NOTIMPLEMENTED;
}

}  // namespace

PyObject* Series_nb_add(PyObject* a, PyObject* b) {
    return binary(a, b, Bin::Add);
}
PyObject* Series_nb_sub(PyObject* a, PyObject* b) {
    return binary(a, b, Bin::Sub);
}
PyObject* Series_nb_mul(PyObject* a, PyObject* b) {
    return binary(a, b, Bin::Mul);
}
PyObject* Series_nb_true_divide(PyObject* a, PyObject* b) {
    return binary(a, b, Bin::TrueDiv);
}
PyObject* Series_nb_floor_divide(PyObject* a, PyObject* b) {
    return binary(a, b, Bin::FloorDiv);
}
PyObject* Series_nb_remainder(PyObject* a, PyObject* b) {
    return binary(a, b, Bin::Mod);
}
PyObject* Series_nb_power(PyObject* a, PyObject* b, PyObject* modulus) {
    if (modulus != Py_None) Py_RETURN_NOTIMPLEMENTED;
    return binary(a, b, Bin::Pow);
}

// `self <op> other` as a Bool mask: against a Series row by row, against a
// number or a string with the scalar kernel.
PyObject* Series_richcompare(PyObject* self, PyObject* other, int op) {
    dftu_cmp_op code = DFTU_CMP_EQ;
    switch (op) {
        case Py_LT:
            code = DFTU_CMP_LT;
            break;
        case Py_LE:
            code = DFTU_CMP_LE;
            break;
        case Py_EQ:
            code = DFTU_CMP_EQ;
            break;
        case Py_NE:
            code = DFTU_CMP_NE;
            break;
        case Py_GT:
            code = DFTU_CMP_GT;
            break;
        case Py_GE:
            code = DFTU_CMP_GE;
            break;
        default:
            Py_RETURN_NOTIMPLEMENTED;
    }
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    Held held_b(other, Held::Optional);
    if (Series* b = held_b.get()) {
        const bool labelled = a->has_index() || b->has_index();
        if (labelled && !dataframe::same_labels(*a, *b)) {
            PyErr_SetString(core::g_dft_value_error,
                            "Can only compare identically-labeled Series "
                            "objects");
            return nullptr;
        }
        Series out = kernel(*a, [&] {
            return Series{
                dftu_series_compare_series(a->handle(), b->handle(), code)};
        });
        if (!out.valid()) {
            PyErr_SetString(PyExc_RuntimeError,
                            "op 'dftu.series.compare_series' failed");
            return nullptr;
        }
        if (a->has_index()) out.handle()->labels = a->handle()->labels;
        return make_series(std::move(out));
    }
    if (other == Py_None) {
        // As pandas: no value equals None, so == is all False and != all True.
        if (op != Py_EQ && op != Py_NE) Py_RETURN_NOTIMPLEMENTED;
        Series out = kernel(*a, [&] {
            Series nulls{dftu_series_null_mask(a->handle())};
            Series valid{dftu_series_logical_not(nulls.handle())};
            return Series{dftu_series_logical(
                nulls.handle(), valid.handle(),
                op == Py_EQ ? DFTU_LOGICAL_AND : DFTU_LOGICAL_OR)};
        });
        return make_series(named_like(std::move(out), *a));
    }
    dftu_scalar s{};
    if (!py_to_scalar(other, &s)) {
        // Not a number, a string or a bool: let the other operand answer.
        if (!PyErr_ExceptionMatches(PyExc_TypeError)) return nullptr;
        PyErr_Clear();
        Py_RETURN_NOTIMPLEMENTED;
    }
    return make_series(named_like(
        kernel(
            *a,
            [&] { return Series{dftu_series_compare(a->handle(), code, s)}; }),
        *a));
}

}  // namespace dftracer::utils::python::frame::series_detail
