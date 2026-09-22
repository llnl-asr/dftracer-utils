#ifndef DFTRACER_UTILS_PYTHON_SERIES_DETAIL_H
#define DFTRACER_UTILS_PYTHON_SERIES_DETAIL_H

#include <Python.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>

// Private seam shared by the Series extension translation units (series.cpp
// plus series/{arith,math,window,stats,reduce,string,arrow}.cpp). series.cpp
// owns the SeriesType type object and defines make_series/as_series; every
// family TU calls them and defines the method bodies referenced by the
// PyMethodDef table back in series.cpp.
namespace dftracer::utils::python::series_detail {

namespace dataframe = dftracer::utils::dataframe;

using dataframe::Series;
using dataframe::TypeId;

struct SeriesObject {
    PyObject_HEAD Series col;
    // Buffer-protocol shape[0] and strides[0]; a column is immutable, so these
    // are constant and every export can point at them (no per-export alloc).
    Py_ssize_t buf_shape;
    Py_ssize_t buf_stride;
};

// Defined in series.cpp (owns the SeriesType type object).
PyObject* make_series(Series&& col);
Series* as_series(PyObject* o);
// The Series behind `o`, or nullptr with no error set when it is not one.
Series* as_series_or_null(PyObject* o);

// Defined in series/reduce.cpp.
PyObject* scalar_to_py(dftu_scalar v);

// A no-argument column op that forwards to a dftu_series_* C ABI function and
// wraps the resulting column. Fn is the native op, bound as a template argument
// so each method is one PyMethodDef line.
template <dftu_series* (*Fn)(const dftu_series*)>
PyObject* series_unary(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return make_series(Series{Fn(a->handle())});
}

// No-argument reducers that call a Series member and wrap the result. Method is
// the member (returning dftu_scalar / double / int64), bound as a template
// argument so each stays one PyMethodDef line.
template <auto Method>
PyObject* series_reduce_scalar(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return scalar_to_py((a->*Method)());
}
template <auto Method>
PyObject* series_reduce_f64(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyFloat_FromDouble((a->*Method)());
}
template <auto Method>
PyObject* series_reduce_i64(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyLong_FromLongLong((a->*Method)());
}

// series/arith.cpp
PyObject* Series_add(PyObject* self, PyObject* other);
PyObject* Series_sub(PyObject* self, PyObject* other);
PyObject* Series_mul(PyObject* self, PyObject* other);
PyObject* Series_div(PyObject* self, PyObject* other);
PyObject* Series_add_scalar(PyObject* self, PyObject* v);
PyObject* Series_sub_scalar(PyObject* self, PyObject* v);
PyObject* Series_mul_scalar(PyObject* self, PyObject* v);
PyObject* Series_div_scalar(PyObject* self, PyObject* v);
PyObject* Series_floordiv(PyObject* self, PyObject* args);
PyObject* Series_mod(PyObject* self, PyObject* args);
PyObject* Series_pow(PyObject* self, PyObject* args);
PyObject* Series_cast(PyObject* self, PyObject* target);
PyObject* Series_prim(PyObject* self, PyObject* code);
PyObject* Series_compare(PyObject* self, PyObject* args);
PyObject* Series_logical(PyObject* self, PyObject* args);
PyObject* Series_logical_not(PyObject* self, PyObject*);

// series/math.cpp
PyObject* Series_clip(PyObject* self, PyObject* args);
PyObject* Series_fillna(PyObject* self, PyObject* value);
PyObject* Series_is_between(PyObject* self, PyObject* args);

// series/window.cpp
PyObject* Series_rolling(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_rolling_var(PyObject* self, PyObject* arg);
PyObject* Series_rolling_std(PyObject* self, PyObject* arg);
PyObject* Series_rolling_median(PyObject* self, PyObject* arg);
PyObject* Series_rolling_quantile(PyObject* self, PyObject* args);
PyObject* Series_ewm_mean(PyObject* self, PyObject* arg);
PyObject* Series_ewm_std(PyObject* self, PyObject* arg);
PyObject* Series_cut(PyObject* self, PyObject* other);
PyObject* Series_qcut(PyObject* self, PyObject* arg);

// series/stats.cpp
PyObject* Series_quantile(PyObject* self, PyObject* arg);
PyObject* Series_median(PyObject* self, PyObject*);
PyObject* Series_variance(PyObject* self, PyObject* args);
PyObject* Series_stddev(PyObject* self, PyObject* args);
PyObject* Series_skewness(PyObject* self, PyObject*);
PyObject* Series_kurtosis(PyObject* self, PyObject*);
PyObject* Series_nunique(PyObject* self, PyObject*);
PyObject* Series_value_counts(PyObject* self, PyObject*);
PyObject* Series_is_sorted(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_is_in(PyObject* self, PyObject* other);
PyObject* Series_sort(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_head(PyObject* self, PyObject* arg);
PyObject* Series_tail(PyObject* self, PyObject* arg);
PyObject* Series_shift(PyObject* self, PyObject* arg);
PyObject* Series_top_k(PyObject* self, PyObject* arg);
PyObject* Series_bottom_k(PyObject* self, PyObject* arg);
PyObject* Series_sample(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_rank(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_search_sorted(PyObject* self, PyObject* other);
PyObject* Series_take(PyObject* self, PyObject* seq);
PyObject* Series_filter(PyObject* self, PyObject* other);
PyObject* Series_where(PyObject* self, PyObject* args);
PyObject* Series_argsort(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_dictionary_encode(PyObject* self, PyObject*);
PyObject* Series_materialize(PyObject* self, PyObject*);
PyObject* Series_share(PyObject* self, PyObject*);
PyObject* Series_slice(PyObject* self, PyObject* args);
PyObject* Series_is_null(PyObject* self, PyObject* arg);
PyObject* Series_num_children(PyObject* self, PyObject*);
PyObject* Series_child(PyObject* self, PyObject* arg);

// series/reduce.cpp
PyObject* Series_all(PyObject* self, PyObject*);
PyObject* Series_any(PyObject* self, PyObject*);
PyObject* Series_dot(PyObject* self, PyObject* other);

// series/string.cpp
PyObject* Series_str_eq(PyObject* self, PyObject* arg);
PyObject* Series_str_contains(PyObject* self, PyObject* arg);
PyObject* Series_str_starts_with(PyObject* self, PyObject* arg);
PyObject* Series_str_ends_with(PyObject* self, PyObject* arg);
PyObject* Series_str_matches(PyObject* self, PyObject* arg);
PyObject* Series_str_search(PyObject* self, PyObject* arg);
PyObject* Series_str_like(PyObject* self, PyObject* arg);
PyObject* Series_str_len_bytes(PyObject* self, PyObject*);
PyObject* Series_str_len_chars(PyObject* self, PyObject*);
PyObject* Series_str_find(PyObject* self, PyObject* arg);
PyObject* Series_fnv1a(PyObject* self, PyObject*);
PyObject* Series_hex64_parse(PyObject* self, PyObject*);
PyObject* Series_hex64_format(PyObject* self, PyObject*);
PyObject* Series_to_lowercase(PyObject* self, PyObject*);
PyObject* Series_to_uppercase(PyObject* self, PyObject*);
PyObject* Series_str_strip(PyObject* self, PyObject*);
PyObject* Series_str_lstrip(PyObject* self, PyObject*);
PyObject* Series_str_rstrip(PyObject* self, PyObject*);
PyObject* Series_str_replace(PyObject* self, PyObject* args);
PyObject* Series_str_replace_all(PyObject* self, PyObject* args);
PyObject* Series_str_slice(PyObject* self, PyObject* args);
PyObject* Series_str_pad_start(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_str_pad_end(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_str_zfill(PyObject* self, PyObject* arg);
PyObject* Series_str_split(PyObject* self, PyObject* arg);
PyObject* Series_str_extract(PyObject* self, PyObject* args);
PyObject* Series_list_len(PyObject* self, PyObject*);
PyObject* Series_list_get(PyObject* self, PyObject* arg);
PyObject* Series_list_join(PyObject* self, PyObject* arg);
PyObject* Series_str_case(PyObject* self, PyObject* arg);
PyObject* Series_str_is(PyObject* self, PyObject* arg);
PyObject* Series_str_count(PyObject* self, PyObject* arg);
PyObject* Series_str_rfind(PyObject* self, PyObject* arg);
PyObject* Series_str_remove_prefix(PyObject* self, PyObject* arg);
PyObject* Series_str_remove_suffix(PyObject* self, PyObject* arg);
PyObject* Series_str_repeat(PyObject* self, PyObject* arg);
PyObject* Series_str_center(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_str_cat(PyObject* self, PyObject* other);
PyObject* Series_str_findall(PyObject* self, PyObject* arg);
PyObject* Series_str_partition(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_dt_part(PyObject* self, PyObject* args);
PyObject* Series_dt_round(PyObject* self, PyObject* args);
PyObject* Series_with_timezone(PyObject* self, PyObject* arg);

// series/arrow.cpp
PyObject* Series_arrow_c_array(PyObject* self, PyObject* args);
Series import_arrow_column(PyObject* obj);
PyObject* vec_from_arrow(PyObject* self, PyObject* obj);
PyObject* vec_from_numpy(PyObject* self, PyObject* obj);

}  // namespace dftracer::utils::python::series_detail

#endif  // DFTRACER_UTILS_PYTHON_SERIES_DETAIL_H
