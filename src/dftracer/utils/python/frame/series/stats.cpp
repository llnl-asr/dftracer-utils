// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/core/seq_helpers.h>
#include <dftracer/utils/python/frame/dataframe.h>
#include <dftracer/utils/python/frame/frame_op_helpers.h>
#include <dftracer/utils/python/frame/series_detail.h>

#include <cstdint>
#include <exception>

namespace dftracer::utils::python::frame::series_detail {

PyObject* Series_quantile(PyObject* self, PyObject* arg) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    double q = PyFloat_AsDouble(arg);
    if (q == -1.0 && PyErr_Occurred()) return nullptr;
    return PyFloat_FromDouble(without_gil(
        a->length(), [&] { return dftu_series_quantile(a->handle(), q); }));
}
PyObject* Series_median(PyObject* self, PyObject*) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    return PyFloat_FromDouble(without_gil(
        a->length(), [&] { return dftu_series_quantile(a->handle(), 0.5); }));
}
PyObject* Series_variance(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    int sample = 1;
    if (!parse_kw<"|p">(args, kwds, {"sample"}, &sample)) return nullptr;
    return PyFloat_FromDouble(without_gil(a->length(), [&] {
        return dftu_series_variance(a->handle(), sample);
    }));
}
PyObject* Series_stddev(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    int sample = 1;
    if (!parse_kw<"|p">(args, kwds, {"sample"}, &sample)) return nullptr;
    return PyFloat_FromDouble(without_gil(
        a->length(), [&] { return dftu_series_stddev(a->handle(), sample); }));
}
PyObject* Series_value_counts(PyObject* self, PyObject*) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    auto out =
        without_gil(a->length(), [&] { return dataframe::value_counts(*a); });
    if (!out) {
        core::set_py_error(out.error());
        return nullptr;
    }
    return dftracer::utils::python::frame::wrap_dataframe(std::move(*out));
}
PyObject* Series_is_sorted(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    int descending = 0;
    if (!parse_kw<"|p">(args, kwds, {"descending"}, &descending))
        return nullptr;
    return PyBool_FromLong(without_gil(a->length(), [&] {
        return dftu_series_is_sorted(a->handle(), descending);
    }));
}
PyObject* Series_is_in(PyObject* self, PyObject* other) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    Held held_values(other);
    Series* values = held_values.get();
    if (!values) return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_is_in(
                                  a->handle(), values->handle())};
                          }),
                   *a));
}
PyObject* Series_sort(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    int descending = 0;
    int nulls_last = 0;
    if (!parse_kw<"|pp">(args, kwds, {"descending", "nulls_last"}, &descending,
                         &nulls_last))
        return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_sort_nulls(
                                  a->handle(), descending, nulls_last)};
                          }),
                   *a));
}
PyObject* Series_head(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long long n = 5;
    if (!parse_kw<"|L">(args, kwds, {"n"}, &n)) return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_head(
                                  a->handle(), static_cast<std::int64_t>(n))};
                          }),
                   *a));
}
PyObject* Series_tail(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long long n = 5;
    if (!parse_kw<"|L">(args, kwds, {"n"}, &n)) return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_tail(
                                  a->handle(), static_cast<std::int64_t>(n))};
                          }),
                   *a));
}
PyObject* Series_shift(PyObject* self, PyObject* arg) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long long n = PyLong_AsLongLong(arg);
    if (n == -1 && PyErr_Occurred()) return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_shift(
                                  a->handle(), static_cast<std::int64_t>(n))};
                          }),
                   *a));
}
PyObject* Series_top_k(PyObject* self, PyObject* arg) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long long k = PyLong_AsLongLong(arg);
    if (k == -1 && PyErr_Occurred()) return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_top_k(
                                  a->handle(), static_cast<std::int64_t>(k))};
                          }),
                   *a));
}
PyObject* Series_bottom_k(PyObject* self, PyObject* arg) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long long k = PyLong_AsLongLong(arg);
    if (k == -1 && PyErr_Occurred()) return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_bottom_k(
                                  a->handle(), static_cast<std::int64_t>(k))};
                          }),
                   *a));
}
PyObject* Series_sample(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    long long n = 0;
    unsigned long long seed = 0;
    if (!parse_kw<"L|K">(args, kwds, {"n", "seed"}, &n, &seed)) return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_sample(
                                  a->handle(), static_cast<std::int64_t>(n),
                                  static_cast<std::uint64_t>(seed))};
                          }),
                   *a));
}
PyObject* Series_rank(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    const char* method = "average";
    int descending = 0;
    int pct = 0;
    if (!parse_kw<"|spp">(args, kwds, {"method", "descending", "pct"}, &method,
                          &descending, &pct))
        return nullptr;
    dftu_rank_method code = DFTU_RANK_AVERAGE;
    if (!dftracer::utils::python::frame::rank_method_from_str(method, &code))
        return nullptr;
    const int flags = (descending ? DFTU_RANK_FLAG_DESCENDING : 0) |
                      (pct ? DFTU_RANK_FLAG_PCT : 0);
    return make_series(named_like(
        kernel(
            *a,
            [&] { return Series{dftu_series_rank(a->handle(), code, flags)}; }),
        *a));
}
PyObject* Series_search_sorted(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    PyObject* other = nullptr;
    const char* side = "left";
    if (!parse_kw<"O|s">(args, kwds, {"element", "side"}, &other, &side))
        return nullptr;
    const dftracer::utils::dstd::StringView which = side;
    if (which != "left" && which != "right" && which != "any") {
        PyErr_SetString(PyExc_ValueError,
                        "search_sorted: side must be 'left', 'right' or 'any'");
        return nullptr;
    }
    const int right = which == "right" ? 1 : 0;
    // A scalar gives its insertion index, a Series or a sequence of numbers a
    // Series of them.
    const bool scalar = PyFloat_Check(other) || PyLong_Check(other);
    Series needles;
    Held held_values(other, Held::Optional);
    if (Series* given = held_values.get()) {
        needles = given->share();
    } else if (scalar || PySequence_Check(other)) {
        PyObject* seq = scalar ? PyTuple_Pack(1, other)
                               : PySequence_Fast(other,
                                                 "values must be a "
                                                 "sequence of numbers");
        if (!seq) return nullptr;
        const Py_ssize_t count = PySequence_Fast_GET_SIZE(seq);
        dftracer::utils::dstd::Vector<double> numbers(
            static_cast<std::size_t>(count));
        for (Py_ssize_t i = 0; i < count; ++i) {
            const double x = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, i));
            if (x == -1.0 && PyErr_Occurred()) {
                Py_DECREF(seq);
                return nullptr;
            }
            numbers[static_cast<std::size_t>(i)] = x;
        }
        Py_DECREF(seq);
        needles = Series::flat_f64(numbers.data(), count);
    } else {
        PyErr_SetString(PyExc_TypeError,
                        "values must be a number, a sequence of numbers or a "
                        "Series");
        return nullptr;
    }
    PyObject* found = make_series(kernel(*a, [&] {
        return Series{dftu_series_search_sorted_side(a->handle(),
                                                     needles.handle(), right)};
    }));
    if (!found || !scalar) return found;
    PyObject* first = PySequence_GetItem(found, 0);
    Py_DECREF(found);
    return first;
}

PyObject* Series_unique(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    int maintain_order = 0;
    if (!parse_kw<"|p">(args, kwds, {"maintain_order"}, &maintain_order))
        return nullptr;
    return make_series(
        named_like(kernel(*a,
                          [&] {
                              return Series{dftu_series_unique_nulls(
                                  a->handle(), maintain_order)};
                          }),
                   *a));
}

PyObject* Series_duplicated(PyObject* self, PyObject* args, PyObject* kwds) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    PyObject* keep_obj = nullptr;
    if (!parse_kw<"|O">(args, kwds, {"keep"}, &keep_obj)) return nullptr;
    int keep = 0;
    if (keep_obj == Py_False) {
        keep = 2;
    } else if (keep_obj && PyUnicode_Check(keep_obj)) {
        const char* text = PyUnicode_AsUTF8(keep_obj);
        if (!text) return nullptr;
        const dftracer::utils::dstd::StringView which = text;
        if (which == "last")
            keep = 1;
        else if (which != "first")
            keep = -1;
    } else if (keep_obj && keep_obj != Py_None) {
        keep = -1;
    }
    if (keep < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "duplicated: keep must be 'first', 'last' or False");
        return nullptr;
    }
    return make_series(named_like(
        kernel(
            *a,
            [&] { return Series{dftu_series_duplicated(a->handle(), keep)}; }),
        *a));
}

PyObject* Series_take(PyObject* self, PyObject* seq) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    dstd::Vector<std::int64_t> idx;
    if (!core::parse_int_seq(seq, "take() expects a sequence of ints", idx))
        return nullptr;
    Series out = kernel(*a, [&] { return a->take(idx); });
    if (!out.valid()) {
        PyErr_SetString(PyExc_IndexError,
                        "take: an index is at or past the column's length");
        return nullptr;
    }
    return make_series(std::move(out));
}

PyObject* Series_filter(PyObject* self, PyObject* other) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    Held held_mask(other);
    Series* mask = held_mask.get();
    if (!mask) return nullptr;
    return make_series(kernel(*a, [&] { return a->filter(*mask); }));
}

PyObject* Series_where(PyObject* self, PyObject* args) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    PyObject* mask_obj = nullptr;
    PyObject* other_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &mask_obj, &other_obj)) return nullptr;
    Held held_mask(mask_obj);
    Series* mask = held_mask.get();
    if (!mask) return nullptr;
    Held held_other(other_obj);
    Series* other = held_other.get();
    if (!other) return nullptr;
    Series out = kernel(*a, [&] { return a->where(*mask, *other); });
    if (!out.valid()) {
        PyErr_SetString(PyExc_ValueError,
                        "where() needs a Bool mask and two columns of one type "
                        "and length");
        return nullptr;
    }
    return make_series(std::move(out));
}

static PyObject* argsort_with_default(PyObject* self, PyObject* args,
                                      PyObject* kwds, int default_nulls_last) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    int descending = 0;
    int nulls_last = default_nulls_last;
    if (!parse_kw<"|pp">(args, kwds, {"descending", "nulls_last"}, &descending,
                         &nulls_last))
        return nullptr;
    return make_series(kernel(
        *a, [&] { return a->argsort(descending != 0, nulls_last != 0); }));
}

PyObject* Series_argsort(PyObject* self, PyObject* args, PyObject* kwds) {
    return argsort_with_default(self, args, kwds, 1);
}

PyObject* Series_arg_sort(PyObject* self, PyObject* args, PyObject* kwds) {
    return argsort_with_default(self, args, kwds, 0);
}

PyObject* Series_dictionary_encode(PyObject* self, PyObject*) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    return make_series(kernel(*a, [&] { return a->dictionary_encode(); }));
}
PyObject* Series_materialize(PyObject* self, PyObject*) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    return make_series(kernel(*a, [&] { return a->materialize(); }));
}
PyObject* Series_share(PyObject* self, PyObject*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return make_series(a->share());
}
PyObject* Series_slice(PyObject* self, PyObject* args) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    long long offset = 0, length = 0;
    if (!PyArg_ParseTuple(args, "LL", &offset, &length)) return nullptr;
    return make_series(a->slice(static_cast<std::int64_t>(offset),
                                static_cast<std::int64_t>(length)));
}

PyObject* Series_is_null(PyObject* self, PyObject* arg) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    long long i = PyLong_AsLongLong(arg);
    if (i == -1 && PyErr_Occurred()) return nullptr;
    return PyBool_FromLong(a->is_null(static_cast<std::int64_t>(i)));
}
PyObject* Series_num_children(PyObject* self, PyObject*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return PyLong_FromLongLong(a->num_children());
}
PyObject* Series_child(PyObject* self, PyObject* arg) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    long long i = PyLong_AsLongLong(arg);
    if (i == -1 && PyErr_Occurred()) return nullptr;
    return make_series(a->child(static_cast<std::int64_t>(i)));
}

}  // namespace dftracer::utils::python::frame::series_detail
