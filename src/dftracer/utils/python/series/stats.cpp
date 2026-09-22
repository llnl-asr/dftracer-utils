#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/py_frame_op_helpers.h>
#include <dftracer/utils/python/series_detail.h>

#include <cstdint>
#include <exception>
#include <vector>

namespace dftracer::utils::python::series_detail {

PyObject* Series_quantile(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    double q = PyFloat_AsDouble(arg);
    if (q == -1.0 && PyErr_Occurred()) return nullptr;
    return PyFloat_FromDouble(dftu_series_quantile(a->handle(), q));
}
PyObject* Series_median(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyFloat_FromDouble(dftu_series_quantile(a->handle(), 0.5));
}
PyObject* Series_variance(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    int sample = 1;
    if (!PyArg_ParseTuple(args, "|p", &sample)) return nullptr;
    return PyFloat_FromDouble(dftu_series_variance(a->handle(), sample));
}
PyObject* Series_stddev(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    int sample = 1;
    if (!PyArg_ParseTuple(args, "|p", &sample)) return nullptr;
    return PyFloat_FromDouble(dftu_series_stddev(a->handle(), sample));
}
PyObject* Series_skewness(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyFloat_FromDouble(dftu_series_skewness(a->handle()));
}
PyObject* Series_kurtosis(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyFloat_FromDouble(dftu_series_kurtosis(a->handle()));
}
PyObject* Series_nunique(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyLong_FromLongLong(dftu_series_nunique(a->handle()));
}
PyObject* Series_value_counts(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    try {
        return dftracer::utils::python::wrap_dataframe(
            dataframe::value_counts(*a));
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        return nullptr;
    }
}
PyObject* Series_is_sorted(PyObject* self, PyObject* args, PyObject* kwds) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    int descending = 0;
    static const char* kwlist[] = {"descending", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p",
                                     const_cast<char**>(kwlist), &descending))
        return nullptr;
    return PyBool_FromLong(dftu_series_is_sorted(a->handle(), descending));
}
PyObject* Series_is_in(PyObject* self, PyObject* other) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    Series* values = as_series(other);
    if (!values) return nullptr;
    return make_series(
        Series{dftu_series_is_in(a->handle(), values->handle())});
}
PyObject* Series_sort(PyObject* self, PyObject* args, PyObject* kwds) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    int descending = 0;
    static const char* kwlist[] = {"descending", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p",
                                     const_cast<char**>(kwlist), &descending))
        return nullptr;
    return make_series(Series{dftu_series_sort(a->handle(), descending)});
}
PyObject* Series_head(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long n = PyLong_AsLongLong(arg);
    if (n == -1 && PyErr_Occurred()) return nullptr;
    return make_series(
        Series{dftu_series_head(a->handle(), static_cast<std::int64_t>(n))});
}
PyObject* Series_tail(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long n = PyLong_AsLongLong(arg);
    if (n == -1 && PyErr_Occurred()) return nullptr;
    return make_series(
        Series{dftu_series_tail(a->handle(), static_cast<std::int64_t>(n))});
}
PyObject* Series_shift(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long n = PyLong_AsLongLong(arg);
    if (n == -1 && PyErr_Occurred()) return nullptr;
    return make_series(
        Series{dftu_series_shift(a->handle(), static_cast<std::int64_t>(n))});
}
PyObject* Series_top_k(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long k = PyLong_AsLongLong(arg);
    if (k == -1 && PyErr_Occurred()) return nullptr;
    return make_series(
        Series{dftu_series_top_k(a->handle(), static_cast<std::int64_t>(k))});
}
PyObject* Series_bottom_k(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long k = PyLong_AsLongLong(arg);
    if (k == -1 && PyErr_Occurred()) return nullptr;
    return make_series(Series{
        dftu_series_bottom_k(a->handle(), static_cast<std::int64_t>(k))});
}
PyObject* Series_sample(PyObject* self, PyObject* args, PyObject* kwds) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long n = 0;
    unsigned long long seed = 0;
    static const char* kwlist[] = {"n", "seed", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "L|K",
                                     const_cast<char**>(kwlist), &n, &seed))
        return nullptr;
    return make_series(
        Series{dftu_series_sample(a->handle(), static_cast<std::int64_t>(n),
                                  static_cast<std::uint64_t>(seed))});
}
PyObject* Series_rank(PyObject* self, PyObject* args, PyObject* kwds) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    const char* method = "average";
    int descending = 0;
    static const char* kwlist[] = {"method", "descending", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|sp",
                                     const_cast<char**>(kwlist), &method,
                                     &descending))
        return nullptr;
    dftu_rank_method code = DFTU_RANK_AVERAGE;
    if (!dftracer::utils::python::rank_method_from_str(method, &code))
        return nullptr;
    return make_series(Series{dftu_series_rank(a->handle(), code, descending)});
}
PyObject* Series_search_sorted(PyObject* self, PyObject* other) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    Series* values = as_series(other);
    if (!values) return nullptr;
    return make_series(
        Series{dftu_series_search_sorted(a->handle(), values->handle())});
}

PyObject* Series_take(PyObject* self, PyObject* seq) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    PyObject* fast = PySequence_Fast(seq, "take() expects a sequence of ints");
    if (!fast) return nullptr;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    std::vector<std::int64_t> idx;
    idx.reserve(static_cast<std::size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        long long v = PyLong_AsLongLong(PySequence_Fast_GET_ITEM(fast, i));
        if (v == -1 && PyErr_Occurred()) {
            Py_DECREF(fast);
            return nullptr;
        }
        idx.push_back(static_cast<std::int64_t>(v));
    }
    Py_DECREF(fast);
    Series out = a->take(idx);
    if (!out.valid()) {
        PyErr_SetString(PyExc_IndexError,
                        "take: an index is at or past the column's length");
        return nullptr;
    }
    return make_series(std::move(out));
}

PyObject* Series_filter(PyObject* self, PyObject* other) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    Series* mask = as_series(other);
    if (!mask) return nullptr;
    return make_series(a->filter(*mask));
}

PyObject* Series_where(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    PyObject* mask_obj = nullptr;
    PyObject* other_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &mask_obj, &other_obj)) return nullptr;
    Series* mask = as_series(mask_obj);
    if (!mask) return nullptr;
    Series* other = as_series(other_obj);
    if (!other) return nullptr;
    Series out = a->where(*mask, *other);
    if (!out.valid()) {
        PyErr_SetString(PyExc_ValueError,
                        "where() needs a Bool mask and two columns of one type "
                        "and length");
        return nullptr;
    }
    return make_series(std::move(out));
}

PyObject* Series_argsort(PyObject* self, PyObject* args, PyObject* kwds) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    int descending = 0;
    static const char* kwlist[] = {"descending", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p",
                                     const_cast<char**>(kwlist), &descending))
        return nullptr;
    return make_series(a->argsort(descending != 0));
}

PyObject* Series_dictionary_encode(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->dictionary_encode());
}
PyObject* Series_materialize(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->materialize());
}
PyObject* Series_share(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(a->share());
}
PyObject* Series_slice(PyObject* self, PyObject* args) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long offset = 0, length = 0;
    if (!PyArg_ParseTuple(args, "LL", &offset, &length)) return nullptr;
    return make_series(a->slice(static_cast<std::int64_t>(offset),
                                static_cast<std::int64_t>(length)));
}

PyObject* Series_is_null(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long i = PyLong_AsLongLong(arg);
    if (i == -1 && PyErr_Occurred()) return nullptr;
    return PyBool_FromLong(a->is_null(static_cast<std::int64_t>(i)));
}
PyObject* Series_num_children(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyLong_FromLongLong(a->num_children());
}
PyObject* Series_child(PyObject* self, PyObject* arg) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    long long i = PyLong_AsLongLong(arg);
    if (i == -1 && PyErr_Occurred()) return nullptr;
    return make_series(a->child(static_cast<std::int64_t>(i)));
}

}  // namespace dftracer::utils::python::series_detail

#endif  // DFTRACER_UTILS_ENABLE_ARROW
