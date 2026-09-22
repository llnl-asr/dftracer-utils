#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/dataframe/kernels/kernels.h>
#include <dftracer/utils/dataframe/kernels/prims.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/python/py_scalar_helpers.h>
#include <dftracer/utils/python/series_detail.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::python::series_detail {

namespace {

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

// A Python number as either an i64 or an f64 scalar; the kernel converts it to
// the column's element type, promoting to Float64 when the scalar is a float
// and the column is not (numpy's weak-scalar rule).
enum class ScalarOp { Add, Sub, Mul, Div };

PyObject* scalar_op(PyObject* self, PyObject* value, ScalarOp op) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    TypeId a_type = a->type();
    if (PyFloat_Check(value)) {
        double v = PyFloat_AsDouble(value);
        if (v == -1.0 && PyErr_Occurred()) return nullptr;
        if (op == ScalarOp::Mul)
            return make_arith_series(dataframe::mul_scalar(*a, v), a_type,
                                     TypeId::Float64);
        if (op == ScalarOp::Div)
            return make_arith_series(dataframe::div_scalar(*a, v), a_type,
                                     TypeId::Float64);
        return make_arith_series(
            dataframe::add_scalar(*a, op == ScalarOp::Sub ? -v : v), a_type,
            TypeId::Float64);
    }
    long long v = PyLong_AsLongLong(value);
    if (v == -1 && PyErr_Occurred()) return nullptr;
    auto iv = static_cast<std::int64_t>(v);
    if (op == ScalarOp::Mul)
        return make_arith_series(dataframe::mul_scalar(*a, iv), a_type,
                                 TypeId::Int64);
    if (op == ScalarOp::Div)
        return make_arith_series(dataframe::div_scalar(*a, iv), a_type,
                                 TypeId::Int64);
    return make_arith_series(
        dataframe::add_scalar(*a, op == ScalarOp::Sub ? -iv : iv), a_type,
        TypeId::Int64);
}

}  // namespace

PyObject* Series_add(PyObject* self, PyObject* other) {
    Series* a = as_series(self);
    Series* b = as_series(other);
    if (!a || !b) return nullptr;
    return make_arith_series(dataframe::add(*a, *b), a->type(), b->type());
}
PyObject* Series_sub(PyObject* self, PyObject* other) {
    Series* a = as_series(self);
    Series* b = as_series(other);
    if (!a || !b) return nullptr;
    return make_arith_series(dataframe::sub(*a, *b), a->type(), b->type());
}
PyObject* Series_mul(PyObject* self, PyObject* other) {
    Series* a = as_series(self);
    Series* b = as_series(other);
    if (!a || !b) return nullptr;
    return make_arith_series(dataframe::mul(*a, *b), a->type(), b->type());
}
PyObject* Series_div(PyObject* self, PyObject* other) {
    Series* a = as_series(self);
    Series* b = as_series(other);
    if (!a || !b) return nullptr;
    return make_arith_series(dataframe::div(*a, *b), a->type(), b->type());
}

// floordiv / mod / pow: `other` is a Series, or a number with the optional
// `reverse` flag (`number <op> self`).
enum class ExtraOp { FloorDiv, Mod, Pow };

PyObject* extra_op(PyObject* self, PyObject* args, ExtraOp op) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    PyObject* other = nullptr;
    int reverse = 0;
    if (!PyArg_ParseTuple(args, "O|p", &other, &reverse)) return nullptr;
    if (Series* b = as_series_or_null(other)) {
        Series out = op == ExtraOp::FloorDiv ? dataframe::floordiv(*a, *b)
                     : op == ExtraOp::Mod    ? dataframe::mod(*a, *b)
                                             : dataframe::pow(*a, *b);
        return make_arith_series(std::move(out), a->type(), b->type());
    }
    const bool rev = reverse != 0;
    if (PyFloat_Check(other)) {
        const double v = PyFloat_AsDouble(other);
        if (v == -1.0 && PyErr_Occurred()) return nullptr;
        Series out = op == ExtraOp::FloorDiv
                         ? dataframe::floordiv_scalar(*a, v, rev)
                     : op == ExtraOp::Mod ? dataframe::mod_scalar(*a, v, rev)
                                          : dataframe::pow_scalar(*a, v, rev);
        return make_arith_series(std::move(out), a->type(), TypeId::Float64);
    }
    const long long v = PyLong_AsLongLong(other);
    if (v == -1 && PyErr_Occurred()) return nullptr;
    const auto iv = static_cast<std::int64_t>(v);
    Series out = op == ExtraOp::FloorDiv
                     ? dataframe::floordiv_scalar(*a, iv, rev)
                 : op == ExtraOp::Mod ? dataframe::mod_scalar(*a, iv, rev)
                                      : dataframe::pow_scalar(*a, iv, rev);
    return make_arith_series(std::move(out), a->type(), TypeId::Int64);
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
    Series* a = as_series(self);
    if (!a) return nullptr;
    long tid = PyLong_AsLong(target);
    if (tid == -1 && PyErr_Occurred()) return nullptr;
    return make_series(dataframe::cast(*a, static_cast<TypeId>(tid)));
}

PyObject* Series_prim(PyObject* self, PyObject* code) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long op = PyLong_AsLong(code);
    if (op == -1 && PyErr_Occurred()) return nullptr;
    return make_series(dataframe::prim(*a, static_cast<dataframe::Prim>(op)));
}

// Compare a column against a scalar (op is a DFTU_CMP_* code) -> Bool
// column.
PyObject* Series_compare(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    int op = 0;
    PyObject* value = nullptr;
    if (!PyArg_ParseTuple(args, "iO", &op, &value)) return nullptr;
    dftu_scalar s{};
    if (!py_to_scalar(value, &s)) return nullptr;
    return make_series(Series{
        dftu_series_compare(a->handle(), static_cast<dftu_cmp_op>(op), s)});
}

// Boolean AND/OR of two Bool columns (op is a DFTU_LOGICAL_* code).
PyObject* Series_logical(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    int op = 0;
    PyObject* other = nullptr;
    if (!PyArg_ParseTuple(args, "iO", &op, &other)) return nullptr;
    Series* b = as_series(other);
    if (!b) return nullptr;
    return make_series(Series{dftu_series_logical(
        a->handle(), b->handle(), static_cast<dftu_logical_op>(op))});
}

PyObject* Series_logical_not(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(Series{dftu_series_logical_not(a->handle())});
}

}  // namespace dftracer::utils::python::series_detail

#endif  // DFTRACER_UTILS_ENABLE_ARROW
