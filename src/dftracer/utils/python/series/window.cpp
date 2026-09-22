#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/python/series_detail.h>

#include <cstdint>
#include <string>

namespace dftracer::utils::python::series_detail {

PyObject* Series_rolling(PyObject* self, PyObject* args, PyObject* kwds) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    Py_ssize_t window = 0;
    const char* op = "sum";
    static const char* kwlist[] = {"window", "op", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "n|s",
                                     const_cast<char**>(kwlist), &window, &op))
        return nullptr;
    const std::string o(op);
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
    return make_series(Series{
        dftu_series_rolling(a->handle(), static_cast<std::int64_t>(window),
                            static_cast<dftu_rolling_op>(code))});
}
PyObject* Series_rolling_var(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long window = PyLong_AsLongLong(arg);
    if (window == -1 && PyErr_Occurred()) return nullptr;
    return make_series(Series{dftu_series_rolling_var(
        a->handle(), static_cast<std::int64_t>(window))});
}
PyObject* Series_rolling_std(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long window = PyLong_AsLongLong(arg);
    if (window == -1 && PyErr_Occurred()) return nullptr;
    return make_series(Series{dftu_series_rolling_std(
        a->handle(), static_cast<std::int64_t>(window))});
}
PyObject* Series_rolling_median(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long window = PyLong_AsLongLong(arg);
    if (window == -1 && PyErr_Occurred()) return nullptr;
    return make_series(Series{dftu_series_rolling_median(
        a->handle(), static_cast<std::int64_t>(window))});
}
PyObject* Series_rolling_quantile(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long window = 0;
    double q = 0.0;
    if (!PyArg_ParseTuple(args, "Ld", &window, &q)) return nullptr;
    return make_series(Series{dftu_series_rolling_quantile(
        a->handle(), static_cast<std::int64_t>(window), q)});
}
PyObject* Series_ewm_mean(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    double alpha = PyFloat_AsDouble(arg);
    if (alpha == -1.0 && PyErr_Occurred()) return nullptr;
    return make_series(Series{dftu_series_ewm_mean(a->handle(), alpha)});
}
PyObject* Series_ewm_std(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    double alpha = PyFloat_AsDouble(arg);
    if (alpha == -1.0 && PyErr_Occurred()) return nullptr;
    return make_series(Series{dftu_series_ewm_std(a->handle(), alpha)});
}
PyObject* Series_cut(PyObject* self, PyObject* other) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    Series* breaks = as_series(other);
    if (!breaks) return nullptr;
    return make_series(Series{dftu_series_cut(a->handle(), breaks->handle())});
}
PyObject* Series_qcut(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long q = PyLong_AsLong(arg);
    if (q == -1 && PyErr_Occurred()) return nullptr;
    return make_series(
        Series{dftu_series_qcut(a->handle(), static_cast<std::int32_t>(q))});
}

}  // namespace dftracer::utils::python::series_detail

#endif  // DFTRACER_UTILS_ENABLE_ARROW
