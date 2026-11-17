// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/python/frame/series_detail.h>

#include <cstdint>

namespace dftracer::utils::python::frame::series_detail {

PyObject* Series_rolling(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    Py_ssize_t window = 0;
    const char* op = "sum";
    if (!parse_kw<"n|s">(args, kwds, {"window", "op"}, &window, &op))
        return nullptr;
    const dstd::StringView o(op);
    int code = 0;
    if (o == "sum")
        code = 0;
    else if (o == "mean")
        code = 1;
    else if (o == "min")
        code = 2;
    else if (o == "max")
        code = 3;
    else {
        PyErr_SetString(PyExc_ValueError,
                        "rolling op must be sum|mean|min|max");
        return nullptr;
    }
    return make_series(named_like(
        kernel(*a,
               [&] {
                   return Series{dftu_series_rolling(
                       a->handle(), static_cast<std::int64_t>(window),
                       static_cast<dftu_rolling_op>(code))};
               }),
        *a));
}
PyObject* Series_rolling_var(PyObject* self, PyObject* arg) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long long window = PyLong_AsLongLong(arg);
    if (window == -1 && PyErr_Occurred()) return nullptr;
    return make_series(named_like(
        kernel(*a,
               [&] {
                   return Series{dftu_series_rolling_var(
                       a->handle(), static_cast<std::int64_t>(window))};
               }),
        *a));
}
PyObject* Series_rolling_std(PyObject* self, PyObject* arg) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long long window = PyLong_AsLongLong(arg);
    if (window == -1 && PyErr_Occurred()) return nullptr;
    return make_series(named_like(
        kernel(*a,
               [&] {
                   return Series{dftu_series_rolling_std(
                       a->handle(), static_cast<std::int64_t>(window))};
               }),
        *a));
}
PyObject* Series_rolling_median(PyObject* self, PyObject* arg) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long long window = PyLong_AsLongLong(arg);
    if (window == -1 && PyErr_Occurred()) return nullptr;
    return make_series(named_like(
        kernel(*a,
               [&] {
                   return Series{dftu_series_rolling_median(
                       a->handle(), static_cast<std::int64_t>(window))};
               }),
        *a));
}
PyObject* Series_rolling_quantile(PyObject* self, PyObject* args) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long long window = 0;
    double q = 0.0;
    if (!PyArg_ParseTuple(args, "Ld", &window, &q)) return nullptr;
    return make_series(named_like(
        kernel(*a,
               [&] {
                   return Series{dftu_series_rolling_quantile(
                       a->handle(), static_cast<std::int64_t>(window), q)};
               }),
        *a));
}
PyObject* Series_ewm_mean(PyObject* self, PyObject* arg) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    double alpha = PyFloat_AsDouble(arg);
    if (alpha == -1.0 && PyErr_Occurred()) return nullptr;
    return make_series(named_like(
        kernel(
            *a,
            [&] { return Series{dftu_series_ewm_mean(a->handle(), alpha)}; }),
        *a));
}
PyObject* Series_ewm_std(PyObject* self, PyObject* arg) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    double alpha = PyFloat_AsDouble(arg);
    if (alpha == -1.0 && PyErr_Occurred()) return nullptr;
    return make_series(named_like(
        kernel(*a,
               [&] { return Series{dftu_series_ewm_std(a->handle(), alpha)}; }),
        *a));
}
PyObject* Series_cut(PyObject* self, PyObject* args) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    PyObject* other = nullptr;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "O|i", &other, &flags)) return nullptr;
    Held held_breaks(other);
    Series* breaks = held_breaks.get();
    if (!breaks) return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_cut(
                                  a->handle(), breaks->handle(), flags)};
                          }),
                   *a));
}
PyObject* Series_qcut(PyObject* self, PyObject* arg) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long q = PyLong_AsLong(arg);
    if (q == -1 && PyErr_Occurred()) return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_qcut(
                                  a->handle(), static_cast<std::int32_t>(q))};
                          }),
                   *a));
}

}  // namespace dftracer::utils::python::frame::series_detail
