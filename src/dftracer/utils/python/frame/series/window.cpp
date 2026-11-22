// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <dftracer/utils/python/frame/series_detail.h>

#include <cmath>
#include <cstdint>

namespace dftracer::utils::python::frame::series_detail {

namespace {

enum class Roll { Sum, Mean, Min, Max, Var, Std, Median, Quantile };

Series reduce_rolling(const Series& a, std::int64_t window,
                      dftu_rolling_op op) {
    return Series{dftu_series_rolling(a.handle(), window, op)};
}

Series roll(const Series& a, Roll kind, std::int64_t window, double q) {
    switch (kind) {
        case Roll::Sum:
            return reduce_rolling(a, window, DFTU_ROLLING_SUM);
        case Roll::Mean:
            return reduce_rolling(a, window, DFTU_ROLLING_MEAN);
        case Roll::Min:
            return reduce_rolling(a, window, DFTU_ROLLING_MIN);
        case Roll::Max:
            return reduce_rolling(a, window, DFTU_ROLLING_MAX);
        case Roll::Var:
            return Series{dftu_series_rolling_var(a.handle(), window)};
        case Roll::Std:
            return Series{dftu_series_rolling_std(a.handle(), window)};
        case Roll::Median:
            return Series{dftu_series_rolling_median(a.handle(), window)};
        case Roll::Quantile:
            return Series{dftu_series_rolling_quantile(a.handle(), window, q)};
    }
    return Series{};
}

PyObject* rolling_of(PyObject* series, Roll kind, std::int64_t window,
                     double q = 0.0) {
    Held held_a(series);
    Series* a = held_a.get();
    if (!a) return nullptr;
    return make_series(
        named_like(kernel(*a, [&] { return roll(*a, kind, window, q); }), *a));
}

enum class Ewma { Mean, Std };

PyObject* ewm_of(PyObject* series, Ewma kind, double alpha) {
    Held held_a(series);
    Series* a = held_a.get();
    if (!a) return nullptr;
    return make_series(named_like(
        kernel(*a,
               [&] {
                   return Series{kind == Ewma::Mean
                                     ? dftu_series_ewm_mean(a->handle(), alpha)
                                     : dftu_series_ewm_std(a->handle(), alpha)};
               }),
        *a));
}

// What s.rolling(n) and s.ewm(alpha) hold: the Series they read and the window
// or the smoothing factor.
struct WindowObject {
    PyObject_HEAD PyObject* weaklist;
    PyObject* series;
    union {
        std::int64_t window;
        double alpha;
    };
};

PyTypeObject RollingType;
PyTypeObject EwmType;

PyObject* make_window(PyTypeObject* type, PyObject* series) {
    auto* self = reinterpret_cast<WindowObject*>(type->tp_alloc(type, 0));
    if (!self) return nullptr;
    Py_INCREF(series);
    self->series = series;
    return reinterpret_cast<PyObject*>(self);
}

void Window_dealloc(WindowObject* self) {
    if (self->weaklist)
        PyObject_ClearWeakRefs(reinterpret_cast<PyObject*>(self));
    Py_XDECREF(self->series);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

PyObject* Window_get_series(PyObject* self, void*) {
    PyObject* series = reinterpret_cast<WindowObject*>(self)->series;
    Py_INCREF(series);
    return series;
}

PyGetSetDef Window_getset[] = {{"_s", Window_get_series, nullptr,
                                "The Series this window reads.", nullptr},
                               {nullptr, nullptr, nullptr, nullptr, nullptr}};

template <Roll Kind>
PyObject* Rolling_reduce(PyObject* self, PyObject*) {
    auto* w = reinterpret_cast<WindowObject*>(self);
    return rolling_of(w->series, Kind, w->window);
}

PyObject* Rolling_quantile(PyObject* self, PyObject* args, PyObject* kwds) {
    double q = 0.0;
    if (!parse_kw<"d:quantile">(args, kwds, {"q"}, &q)) return nullptr;
    auto* w = reinterpret_cast<WindowObject*>(self);
    return rolling_of(w->series, Roll::Quantile, w->window, q);
}

template <Ewma Kind>
PyObject* Ewm_reduce(PyObject* self, PyObject*) {
    auto* w = reinterpret_cast<WindowObject*>(self);
    return ewm_of(w->series, Kind, w->alpha);
}

PyMethodDef Rolling_methods[] = {
    {"sum", Rolling_reduce<Roll::Sum>, METH_NOARGS,
     "sum() -> Float64 Series of per-window sums."},
    {"mean", Rolling_reduce<Roll::Mean>, METH_NOARGS,
     "mean() -> Float64 Series of per-window means."},
    {"min", Rolling_reduce<Roll::Min>, METH_NOARGS,
     "min() -> Float64 Series of per-window minima."},
    {"max", Rolling_reduce<Roll::Max>, METH_NOARGS,
     "max() -> Float64 Series of per-window maxima."},
    {"var", Rolling_reduce<Roll::Var>, METH_NOARGS,
     "var() -> Float64 Series of per-window sample variance."},
    {"std", Rolling_reduce<Roll::Std>, METH_NOARGS,
     "std() -> Float64 Series of per-window sample std."},
    {"median", Rolling_reduce<Roll::Median>, METH_NOARGS,
     "median() -> Float64 Series of per-window median."},
    {"quantile", DFTU_PYCFUNCTION(Rolling_quantile),
     METH_VARARGS | METH_KEYWORDS,
     "quantile(q) -> Float64 Series of per-window quantile q."},
    {nullptr, nullptr, 0, nullptr}};

PyMethodDef Ewm_methods[] = {
    {"mean", Ewm_reduce<Ewma::Mean>, METH_NOARGS,
     "mean() -> Float64 Series, exponentially weighted mean."},
    {"std", Ewm_reduce<Ewma::Std>, METH_NOARGS,
     "std() -> Float64 Series, exponentially weighted sample std."},
    {nullptr, nullptr, 0, nullptr}};

void make_window_type(PyTypeObject& t, const char* name, const char* doc,
                      PyMethodDef* methods) {
    t = {};
    t.ob_base = {PyObject_HEAD_INIT(nullptr) 0};
    t.tp_name = name;
    t.tp_basicsize = sizeof(WindowObject);
    t.tp_weaklistoffset = WEAKLIST_OFFSET;
    t.tp_flags = Py_TPFLAGS_DEFAULT;
    t.tp_doc = doc;
    t.tp_dealloc = reinterpret_cast<destructor>(Window_dealloc);
    t.tp_methods = methods;
    t.tp_getset = Window_getset;
}

// The smoothing factor of the one of alpha, span, com and halflife that is
// given (a None is not given).
bool ewm_alpha(PyObject* const (&given)[4], double* out) {
    static constexpr const char* NAMES[4] = {"alpha", "span", "com",
                                             "halflife"};
    int which = -1;
    int count = 0;
    for (int i = 0; i < 4; ++i) {
        if (given[i] && given[i] != Py_None) {
            which = i;
            ++count;
        }
    }
    if (count != 1) {
        PyErr_SetString(PyExc_ValueError,
                        "ewm: pass exactly one of alpha, span, com or "
                        "halflife");
        return false;
    }
    const double v = PyFloat_AsDouble(given[which]);
    if (v == -1.0 && PyErr_Occurred()) return false;
    switch (which) {
        case 0:
            if (!(v > 0 && v <= 1)) break;
            *out = v;
            return true;
        case 1:
            if (v < 1) break;
            *out = 2.0 / (v + 1.0);
            return true;
        case 2:
            if (v < 0) break;
            *out = 1.0 / (1.0 + v);
            return true;
        default:
            if (v <= 0) break;
            *out = 1.0 - std::pow(0.5, 1.0 / v);
            return true;
    }
    static constexpr const char* LIMITS[4] = {"in (0, 1]", ">= 1", ">= 0",
                                              "> 0"};
    PyErr_Format(PyExc_ValueError, "ewm: %s must be %s", NAMES[which],
                 LIMITS[which]);
    return false;
}

PyObject* ewm_alpha_fn(PyObject*, PyObject* args, PyObject* kwds) {
    PyObject* given[4] = {nullptr, nullptr, nullptr, nullptr};
    if (!parse_kw<"|O$OOO:_ewm_alpha">(
            args, kwds, {"alpha", "span", "com", "halflife"}, &given[0],
            &given[1], &given[2], &given[3]))
        return nullptr;
    double alpha = 0.0;
    if (!ewm_alpha(given, &alpha)) return nullptr;
    return PyFloat_FromDouble(alpha);
}

PyMethodDef window_functions[] = {
    {"_ewm_alpha", DFTU_PYCFUNCTION(ewm_alpha_fn), METH_VARARGS | METH_KEYWORDS,
     "_ewm_alpha(alpha=None, *, span=None, com=None, halflife=None) -> the "
     "smoothing factor of the one parameter given."},
    {nullptr, nullptr, 0, nullptr}};

}  // namespace

int init_windows(PyObject* m) {
    make_window_type(RollingType, "dftracer.utils.dftracer_utils_ext.Rolling",
                     "A trailing window of a Series: Series.rolling(n).",
                     Rolling_methods);
    make_window_type(EwmType, "dftracer.utils.dftracer_utils_ext.Ewm",
                     "An exponentially weighted window of a Series: "
                     "Series.ewm(alpha).",
                     Ewm_methods);
    if (register_type(m, &RollingType, "Rolling") < 0) return -1;
    if (register_type(m, &EwmType, "Ewm") < 0) return -1;
    return PyModule_AddFunctions(m, window_functions);
}

PyObject* Series_rolling(PyObject* self, PyObject* args, PyObject* kwds) {
    Py_ssize_t window = 0;
    const char* op = nullptr;
    if (!parse_kw<"n|z:rolling">(args, kwds, {"window", "op"}, &window, &op))
        return nullptr;
    if (!op) {
        if (!as_native<Series>(self)) return nullptr;
        PyObject* out = make_window(&RollingType, self);
        if (out) reinterpret_cast<WindowObject*>(out)->window = window;
        return out;
    }
    const dstd::StringView o(op);
    Roll kind = Roll::Sum;
    if (o == "sum")
        kind = Roll::Sum;
    else if (o == "mean")
        kind = Roll::Mean;
    else if (o == "min")
        kind = Roll::Min;
    else if (o == "max")
        kind = Roll::Max;
    else {
        PyErr_SetString(PyExc_ValueError,
                        "rolling op must be sum|mean|min|max");
        return nullptr;
    }
    return rolling_of(self, kind, window);
}

namespace {

PyObject* rolling_flat(PyObject* self, PyObject* arg, Roll kind) {
    const long long window = PyLong_AsLongLong(arg);
    if (window == -1 && PyErr_Occurred()) return nullptr;
    return rolling_of(self, kind, window);
}

PyObject* ewm_flat(PyObject* self, PyObject* arg, Ewma kind) {
    const double alpha = PyFloat_AsDouble(arg);
    if (alpha == -1.0 && PyErr_Occurred()) return nullptr;
    return ewm_of(self, kind, alpha);
}

}  // namespace

PyObject* Series_rolling_sum(PyObject* self, PyObject* arg) {
    return rolling_flat(self, arg, Roll::Sum);
}
PyObject* Series_rolling_mean(PyObject* self, PyObject* arg) {
    return rolling_flat(self, arg, Roll::Mean);
}
PyObject* Series_rolling_min(PyObject* self, PyObject* arg) {
    return rolling_flat(self, arg, Roll::Min);
}
PyObject* Series_rolling_max(PyObject* self, PyObject* arg) {
    return rolling_flat(self, arg, Roll::Max);
}
PyObject* Series_rolling_quantile(PyObject* self, PyObject* args) {
    long long window = 0;
    double q = 0.0;
    if (!PyArg_ParseTuple(args, "Ld", &window, &q)) return nullptr;
    return rolling_of(self, Roll::Quantile, window, q);
}
PyObject* Series_ewm_mean(PyObject* self, PyObject* arg) {
    return ewm_flat(self, arg, Ewma::Mean);
}
PyObject* Series_ewm_std(PyObject* self, PyObject* arg) {
    return ewm_flat(self, arg, Ewma::Std);
}

PyObject* Series_ewm(PyObject* self, PyObject* args, PyObject* kwds) {
    PyObject* given[4] = {nullptr, nullptr, nullptr, nullptr};
    if (!parse_kw<"|O$OOO:ewm">(args, kwds,
                                {"alpha", "span", "com", "halflife"}, &given[0],
                                &given[1], &given[2], &given[3]))
        return nullptr;
    double alpha = 0.0;
    if (!ewm_alpha(given, &alpha)) return nullptr;
    if (!as_native<Series>(self)) return nullptr;
    PyObject* out = make_window(&EwmType, self);
    if (out) reinterpret_cast<WindowObject*>(out)->alpha = alpha;
    return out;
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
