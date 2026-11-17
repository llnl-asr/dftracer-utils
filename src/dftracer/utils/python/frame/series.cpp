// SPDX-License-Identifier: MIT
// A native vec column exposed to Python: the DSL and pipeline operate on our
// own SIMD columnar format (dataframe::Series, Highway kernels) and only cross
// to Arrow at the edges - construction from a pyarrow array and export via the
// Arrow PyCapsule interface (__arrow_c_array__).
//
// This is the entry translation unit: it owns the SeriesType type object, the
// PyMethodDef table, init_series, and the shared make_series
// file-static. The method bodies live in series/{arith,math,window,stats,
// reduce,string,arrow}.cpp and are declared in series_detail.h.

#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/type_helpers.h>
#include <dftracer/utils/python/frame/series.h>
#include <dftracer/utils/python/frame/series_detail.h>

#include <cstdint>
#include <new>
#include <utility>

namespace dftracer::utils::python::frame::series_detail {

namespace {

PyTypeObject SeriesType;
// The type the factories allocate: SeriesType until _bind_types gives the
// Python subclass.
PyTypeObject* g_series_type = &SeriesType;

void Series_dealloc(SeriesObject* self) {
    if (self->weaklist)
        PyObject_ClearWeakRefs(reinterpret_cast<PyObject*>(self));
    self->col.~Series();
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

}  // namespace

Series named_like(Series out, const Series& source) {
    if (out.valid() && source.valid())
        out.handle()->name = source.handle()->name;
    return out;
}

Series named_like(Series out, const Series& a, const Series& b) {
    if (!out.valid()) return out;
    out.handle()->name =
        a.name() == b.name() ? a.handle()->name : dstd::SharedString{};
    return out;
}

PyObject* make_series(Series&& col) {
    if (!col.valid()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "dataframe kernel produced a null column");
        return nullptr;
    }
    auto* self = reinterpret_cast<SeriesObject*>(
        g_series_type->tp_alloc(g_series_type, 0));
    if (!self) return nullptr;
    new (&self->col) Series(std::move(col));
    return reinterpret_cast<PyObject*>(self);
}

namespace {

Py_ssize_t Series_length_sq(PyObject* self) {
    Series* a = as_native<Series>(self);
    if (!a) return -1;
    return static_cast<Py_ssize_t>(a->length());
}

// Expose a FLAT, non-null, fixed-width numeric column as a 1-D read-only buffer
// so numpy reads it (np.asarray) with no pyarrow. Bit-packed Bool, strings,
// nulls, and non-FLAT encodings raise BufferError; the Python wrapper then
// falls back to the Arrow path.
int Series_getbuffer(PyObject* self, Py_buffer* view, int flags) {
    view->obj = nullptr;
    Series* a = as_native<Series>(self);
    if (!a) return -1;
    if ((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE) {
        PyErr_SetString(PyExc_BufferError, "Series buffer is read-only");
        return -1;
    }
    dftu_series* h = a->handle();
    if (dftu_series_encoding(h) != 0 || dftu_series_null_count(h) != 0) {
        PyErr_SetString(PyExc_BufferError,
                        "Series is not a flat, non-null column");
        return -1;
    }
    // numpy gives its fixed-width int64 types the C long code where long is 64
    // bits. With "q" and "Q" it makes longlong and ulonglong, which pandas
    // cannot factorize, so a merge on such a key fails.
    constexpr const char* INT64_FORMAT = sizeof(long) == 8 ? "l" : "q";
    constexpr const char* UINT64_FORMAT = sizeof(long) == 8 ? "L" : "Q";
    const char* fmt = nullptr;
    Py_ssize_t itemsize = 0;
    switch (static_cast<TypeId>(dftu_series_type(h))) {
        case TypeId::Int8:
            fmt = "b";
            itemsize = 1;
            break;
        case TypeId::Int16:
            fmt = "h";
            itemsize = 2;
            break;
        case TypeId::Int32:
            fmt = "i";
            itemsize = 4;
            break;
        case TypeId::Int64:
            fmt = INT64_FORMAT;
            itemsize = 8;
            break;
        case TypeId::Uint8:
            fmt = "B";
            itemsize = 1;
            break;
        case TypeId::Uint16:
            fmt = "H";
            itemsize = 2;
            break;
        case TypeId::Uint32:
            fmt = "I";
            itemsize = 4;
            break;
        case TypeId::Uint64:
            fmt = UINT64_FORMAT;
            itemsize = 8;
            break;
        case TypeId::Float32:
            fmt = "f";
            itemsize = 4;
            break;
        case TypeId::Float64:
            fmt = "d";
            itemsize = 8;
            break;
        default:
            PyErr_SetString(PyExc_BufferError,
                            "Series type has no plain numeric buffer");
            return -1;
    }
    const void* data = dftu_series_data(h);
    std::int64_t n = dftu_series_length(h);
    if (!data && n > 0) {
        PyErr_SetString(PyExc_BufferError, "Series has no contiguous buffer");
        return -1;
    }
    // The values buffer is exposed as-is (zero-copy); shape/stride live in the
    // object (constant for an immutable column), so there is nothing to free.
    auto* obj = reinterpret_cast<SeriesObject*>(self);
    obj->buf_shape = static_cast<Py_ssize_t>(n);
    obj->buf_stride = itemsize;
    Py_INCREF(self);
    view->obj = self;
    view->buf = const_cast<void*>(data);
    view->len = static_cast<Py_ssize_t>(n) * itemsize;
    view->readonly = 1;
    view->itemsize = itemsize;
    view->format = (flags & PyBUF_FORMAT) ? const_cast<char*>(fmt) : nullptr;
    view->ndim = 1;
    view->shape = &obj->buf_shape;
    view->strides = &obj->buf_stride;
    view->suboffsets = nullptr;
    view->internal = nullptr;
    return 0;
}

// ---- getters -------------------------------------------------------------

PyObject* Series_get_type(PyObject* self, void*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return PyLong_FromLong(static_cast<long>(a->type()));
}
PyObject* Series_get_length(PyObject* self, void*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return PyLong_FromLongLong(a->length());
}
PyObject* Series_get_null_count(PyObject* self, void*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return PyLong_FromLongLong(a->null_count());
}
PyObject* Series_get_encoding(PyObject* self, void*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return PyLong_FromLong(static_cast<long>(a->encoding()));
}
PyObject* Series_get_time_unit(PyObject* self, void*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return PyLong_FromLong(
        static_cast<long>(dftu_series_time_unit(a->handle())));
}

PyObject* Series_get_timezone(PyObject* self, void*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return PyUnicode_FromString(dftu_series_timezone(a->handle()));
}

PyObject* Series_get_is_json(PyObject* self, void*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return PyBool_FromLong(dftu_series_is_json(a->handle()));
}

PyObject* Series_get_name(PyObject* self, void*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    const dstd::StringView name = a->name();
    if (name.empty()) Py_RETURN_NONE;
    return PyUnicode_FromStringAndSize(name.data(),
                                       static_cast<Py_ssize_t>(name.size()));
}

// A name argument as text: an empty view for None. False with a TypeError
// naming `what` for anything but a str or None.
bool name_from_py(PyObject* arg, const char* what, dstd::StringView& out) {
    if (arg == Py_None) {
        out = dstd::StringView{};
        return true;
    }
    if (!PyUnicode_Check(arg)) {
        PyErr_Format(PyExc_TypeError, "%s must be a str or None, not %.100s",
                     what, Py_TYPE(arg)->tp_name);
        return false;
    }
    Py_ssize_t n = 0;
    const char* p = PyUnicode_AsUTF8AndSize(arg, &n);
    if (!p) return false;
    out = dstd::StringView(p, static_cast<std::size_t>(n));
    return true;
}

PyObject* Series_rename(PyObject* self, PyObject* arg) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    dstd::StringView name;
    if (!name_from_py(arg, "rename: the name", name)) return nullptr;
    return make_series(a->rename(name));
}

// The name is the one thing a Series changes in place, as pandas and polars
// do; the buffers stay shared.
int Series_set_name(PyObject* self, PyObject* value, void*) {
    if (!value) {
        PyErr_SetString(PyExc_TypeError, "cannot delete the Series name");
        return -1;
    }
    Series* a = as_native<Series>(self);
    if (!a) return -1;
    dstd::StringView name;
    if (!name_from_py(value, "a Series name", name)) return -1;
    a->handle()->name =
        name.empty() ? dstd::SharedString{} : dstd::SharedString{name};
    return 0;
}

// Series[T] on the native base and every subclass: a generic alias where
// Python has one, else the class itself.
PyObject* Series_class_getitem(PyObject* cls, PyObject* args) {
#if PY_VERSION_HEX >= 0x03090000
    return Py_GenericAlias(cls, args);
#else
    Py_INCREF(cls);
    return cls;
#endif
}

PyMethodDef Series_methods[] = {
    {"add", DFTU_PYCFUNCTION((core::kw_one<"other", Series_add>)),
     METH_VARARGS | METH_KEYWORDS,
     "Element-wise addition; mixed dtypes promote to a common type (numpy "
     "rules, e.g. uint64 + float64 -> float64)."},
    {"sub", DFTU_PYCFUNCTION((core::kw_one<"other", Series_sub>)),
     METH_VARARGS | METH_KEYWORDS,
     "Element-wise subtraction; mixed dtypes promote to a common type (numpy "
     "rules). Note: uint64 - uint64 wraps on underflow (like numpy); cast to "
     "a signed or float dtype first for interval/signed math."},
    {"mul", DFTU_PYCFUNCTION((core::kw_one<"other", Series_mul>)),
     METH_VARARGS | METH_KEYWORDS, "Elementwise a * b."},
    {"div", Series_div, METH_O, "Elementwise a / b."},
    {"add_scalar", DFTU_PYCFUNCTION((core::kw_one<"v", Series_add_scalar>)),
     METH_VARARGS | METH_KEYWORDS, "Broadcast a + scalar."},
    {"sub_scalar", DFTU_PYCFUNCTION((core::kw_one<"v", Series_sub_scalar>)),
     METH_VARARGS | METH_KEYWORDS,
     "Broadcast a - scalar; a float scalar promotes an integer column to "
     "Float64 (numpy's weak-scalar rule). Note: uint64 - v wraps on "
     "underflow (like numpy); cast to a signed or float dtype first for "
     "interval/signed math."},
    {"mul_scalar", DFTU_PYCFUNCTION((core::kw_one<"v", Series_mul_scalar>)),
     METH_VARARGS | METH_KEYWORDS, "Broadcast a * scalar."},
    {"div_scalar", DFTU_PYCFUNCTION((core::kw_one<"v", Series_div_scalar>)),
     METH_VARARGS | METH_KEYWORDS, "Broadcast a / scalar."},
    {"floordiv",
     DFTU_PYCFUNCTION((core::kw_pair<Series_floordiv, "other", "reverse">)),
     METH_VARARGS | METH_KEYWORDS,
     "floordiv(other, reverse=False) -> a // other (a Series or a number; "
     "reverse computes other // a), Python's floor rule; a zero divisor is "
     "null."},
    {"mod", DFTU_PYCFUNCTION((core::kw_pair<Series_mod, "other", "reverse">)),
     METH_VARARGS | METH_KEYWORDS,
     "mod(other, reverse=False) -> a % other with the divisor's sign; a zero "
     "divisor is null."},
    {"pow", DFTU_PYCFUNCTION((core::kw_pair<Series_pow, "other", "reverse">)),
     METH_VARARGS | METH_KEYWORDS,
     "pow(other, reverse=False) -> a ** other; Int64 for two integers (a "
     "negative exponent is null), else Float64."},
    {"ffill", series_unary<dftu_series_ffill>, METH_NOARGS,
     "ffill() -> nulls filled from the nearest present value before them "
     "(pad)."},
    {"bfill", series_unary<dftu_series_bfill>, METH_NOARGS,
     "bfill() -> nulls filled from the nearest present value after them "
     "(backfill)."},
    {"cast", Series_cast, METH_O,
     "Cast to a TypeId code; TypeError names both types for a pair the engine "
     "refuses."},
    {"prim", DFTU_PYCFUNCTION((core::kw_one<"op", Series_prim>)),
     METH_VARARGS | METH_KEYWORDS,
     "Apply a unary numeric primitive by Prim code (i64 in, i64 out)."},
    {"compare", Series_compare, METH_VARARGS,
     "compare(op, scalar) -> Bool column (op is a DFTU_CMP_* code)."},
    {"logical",
     DFTU_PYCFUNCTION((core::kw_pair<Series_logical, "op", "other">)),
     METH_VARARGS | METH_KEYWORDS,
     "logical(op, other) -> Bool column (op is a DFTU_LOGICAL_* code)."},
    {"logical_not", Series_logical_not, METH_NOARGS,
     "Boolean NOT of a Bool column."},
    {"__arrow_c_stream__", Series_arrow_c_stream, METH_VARARGS,
     "Stream the column: one Arrow array per chunk."},
    {"__arrow_c_array__", Series_arrow_c_array, METH_VARARGS,
     "Arrow PyCapsule export: (schema_capsule, array_capsule)."},
    {"quantile", DFTU_PYCFUNCTION((core::kw_one<"q", Series_quantile>)),
     METH_VARARGS | METH_KEYWORDS,
     "quantile(q) -> float (linear interpolation, SIMD sort)."},
    {"median", Series_median, METH_NOARGS, "Median (quantile 0.5)."},
    {"variance", DFTU_PYCFUNCTION(Series_variance),
     METH_VARARGS | METH_KEYWORDS, "variance(sample=True) -> float."},
    {"stddev", DFTU_PYCFUNCTION(Series_stddev), METH_VARARGS | METH_KEYWORDS,
     "stddev(sample=True) -> float."},
    {"skewness", Series_skewness, METH_NOARGS, "Population skewness."},
    {"kurtosis", Series_kurtosis, METH_NOARGS, "Excess kurtosis."},
    {"nunique", Series_nunique, METH_NOARGS, "Number of distinct values."},
    {"unique", series_unary<dftu_series_unique>, METH_NOARGS,
     "Distinct values (ascending) -> Series."},
    {"value_counts", Series_value_counts, METH_NOARGS,
     "value_counts() -> DataFrame of distinct values and counts, "
     "most-frequent first."},
    {"abs", series_unary<dftu_series_abs>, METH_NOARGS,
     "Elementwise absolute value (SIMD)."},
    {"clip", Series_clip, METH_VARARGS,
     "clip(lo, hi) -> Series clamped to [lo, hi] (SIMD)."},
    {"round", series_unary<dftu_series_round>, METH_NOARGS,
     "Round floats to nearest (SIMD)."},
    {"fillna", DFTU_PYCFUNCTION((core::kw_one<"value", Series_fillna>)),
     METH_VARARGS | METH_KEYWORDS,
     "fillna(value) -> Series with nulls replaced."},
    {"cumsum", series_unary<dftu_series_cumsum>, METH_NOARGS,
     "Cumulative sum -> Series."},
    {"cummax", series_unary<dftu_series_cummax>, METH_NOARGS,
     "Running maximum -> Series."},
    {"cummin", series_unary<dftu_series_cummin>, METH_NOARGS,
     "Running minimum -> Series."},
    {"cum_prod", series_unary<dftu_series_cum_prod>, METH_NOARGS,
     "Running product -> Series."},
    {"cum_count", series_unary<dftu_series_cum_count>, METH_NOARGS,
     "Running count of non-null rows -> Int64 Series."},
    {"ceil", series_unary<dftu_series_ceil>, METH_NOARGS,
     "Round floats toward +inf (SIMD)."},
    {"floor", series_unary<dftu_series_floor>, METH_NOARGS,
     "Round floats toward -inf (SIMD)."},
    {"trunc", series_unary<dftu_series_trunc>, METH_NOARGS,
     "Round floats toward zero (SIMD)."},
    {"sign", series_unary<dftu_series_sign>, METH_NOARGS,
     "Sign as -1/0/1 (SIMD)."},
    {"negate", series_unary<dftu_series_negate>, METH_NOARGS,
     "Unary minus (SIMD)."},
    {"diff", series_unary<dftu_series_diff>, METH_NOARGS,
     "First difference x[i]-x[i-1]; row 0 null."},
    {"pct_change", series_unary<dftu_series_pct_change>, METH_NOARGS,
     "Percent change -> Float64 Series; row 0 null."},
    {"sqrt", series_unary<dftu_series_sqrt>, METH_NOARGS,
     "Square root -> Float64 Series (SIMD)."},
    {"exp", series_unary<dftu_series_exp>, METH_NOARGS,
     "Exponential -> Float64 Series."},
    {"log", series_unary<dftu_series_log>, METH_NOARGS,
     "Natural log -> Float64 Series."},
    {"null_mask", series_unary<dftu_series_null_mask>, METH_NOARGS,
     "Bool mask: row is null."},
    {"valid_mask", series_unary<dftu_series_valid_mask>, METH_NOARGS,
     "Bool mask: row is present."},
    {"is_nan", series_unary<dftu_series_is_nan>, METH_NOARGS,
     "Bool mask: value is NaN (SIMD)."},
    {"is_finite", series_unary<dftu_series_is_finite>, METH_NOARGS,
     "Bool mask: value is finite (SIMD)."},
    {"is_infinite", series_unary<dftu_series_is_infinite>, METH_NOARGS,
     "Bool mask: value is +/-Inf (SIMD)."},
    {"is_unique", series_unary<dftu_series_is_unique>, METH_NOARGS,
     "Bool mask: value occurs exactly once."},
    {"is_duplicated", series_unary<dftu_series_is_duplicated>, METH_NOARGS,
     "Bool mask: value is repeated."},
    {"is_sorted", DFTU_PYCFUNCTION(Series_is_sorted),
     METH_VARARGS | METH_KEYWORDS, "is_sorted(descending=False) -> bool."},
    {"drop_nulls", series_unary<dftu_series_drop_nulls>, METH_NOARGS,
     "Drop null rows -> Series."},
    {"is_in", Series_is_in, METH_O,
     "is_in(values) -> Bool mask where the value is in the values Series."},
    {"sort", DFTU_PYCFUNCTION(Series_sort), METH_VARARGS | METH_KEYWORDS,
     "sort(descending=False) -> sorted Series."},
    {"head", DFTU_PYCFUNCTION(Series_head), METH_VARARGS | METH_KEYWORDS,
     "head(n=5) -> first n rows Series."},
    {"tail", DFTU_PYCFUNCTION(Series_tail), METH_VARARGS | METH_KEYWORDS,
     "tail(n=5) -> last n rows Series."},
    {"reverse", series_unary<dftu_series_reverse>, METH_NOARGS,
     "Rows in reverse order -> Series."},
    {"shift", Series_shift, METH_O,
     "shift(n) -> Series shifted by n (vacated rows null)."},
    {"top_k", DFTU_PYCFUNCTION((core::kw_one<"k", Series_top_k>)),
     METH_VARARGS | METH_KEYWORDS, "top_k(k) -> the k largest rows Series."},
    {"bottom_k", DFTU_PYCFUNCTION((core::kw_one<"k", Series_bottom_k>)),
     METH_VARARGS | METH_KEYWORDS,
     "bottom_k(k) -> the k smallest rows Series."},
    {"sample", DFTU_PYCFUNCTION(Series_sample), METH_VARARGS | METH_KEYWORDS,
     "sample(n, seed=0) -> deterministic sample of n rows Series."},
    {"rank", DFTU_PYCFUNCTION(Series_rank), METH_VARARGS | METH_KEYWORDS,
     "rank(method='average', descending=False) -> Float64 Series "
     "(method: average|min|dense|ordinal)."},
    {"rolling", DFTU_PYCFUNCTION(Series_rolling), METH_VARARGS | METH_KEYWORDS,
     "rolling(window, op='sum') -> Float64 Series (op: sum|mean|min|max)."},
    {"rolling_var",
     DFTU_PYCFUNCTION((core::kw_one<"window", Series_rolling_var>)),
     METH_VARARGS | METH_KEYWORDS,
     "rolling_var(window) -> Float64 Series of per-window sample variance."},
    {"rolling_std",
     DFTU_PYCFUNCTION((core::kw_one<"window", Series_rolling_std>)),
     METH_VARARGS | METH_KEYWORDS,
     "rolling_std(window) -> Float64 Series of per-window sample std."},
    {"rolling_median",
     DFTU_PYCFUNCTION((core::kw_one<"window", Series_rolling_median>)),
     METH_VARARGS | METH_KEYWORDS,
     "rolling_median(window) -> Float64 Series of per-window median."},
    {"rolling_quantile",
     DFTU_PYCFUNCTION((core::kw_pair<Series_rolling_quantile, "window", "q">)),
     METH_VARARGS | METH_KEYWORDS,
     "rolling_quantile(window, q) -> Float64 Series of per-window quantile q."},
    {"ewm_mean", DFTU_PYCFUNCTION((core::kw_one<"alpha", Series_ewm_mean>)),
     METH_VARARGS | METH_KEYWORDS,
     "ewm_mean(alpha) -> Float64 Series, exponentially weighted mean."},
    {"ewm_std", DFTU_PYCFUNCTION((core::kw_one<"alpha", Series_ewm_std>)),
     METH_VARARGS | METH_KEYWORDS,
     "ewm_std(alpha) -> Float64 Series, exponentially weighted sample std."},
    {"cut", Series_cut, METH_VARARGS,
     "cut(breaks, flags=0) -> Int32 Series binning by the ascending breaks "
     "Series; flags 1 closes each interval on the right, 2 gives null outside "
     "the interior intervals and numbers them from 0."},
    {"qcut", DFTU_PYCFUNCTION((core::kw_one<"q", Series_qcut>)),
     METH_VARARGS | METH_KEYWORDS,
     "qcut(q) -> Int32 Series binning by the column's q-quantile edges."},
    {"search_sorted",
     DFTU_PYCFUNCTION((core::kw_one<"values", Series_search_sorted>)),
     METH_VARARGS | METH_KEYWORDS,
     "search_sorted(values) -> Int64 Series of lower-bound insertion indices."},
    {"interpolate", series_unary<dftu_series_interpolate>, METH_NOARGS,
     "interpolate() -> Float64 Series with null interiors linearly filled."},
    {"is_between",
     DFTU_PYCFUNCTION((core::kw_pair<Series_is_between, "lo", "hi">)),
     METH_VARARGS | METH_KEYWORDS,
     "is_between(lo, hi) -> Bool mask where lo <= value <= hi (SIMD)."},
    {"dot", DFTU_PYCFUNCTION((core::kw_one<"other", Series_dot>)),
     METH_VARARGS | METH_KEYWORDS,
     "dot(other) -> scalar sum of x[i]*y[i] over non-null pairs (SIMD)."},
    {"sum", series_reduce_scalar<&Series::sum>, METH_NOARGS,
     "Sum of the valid values; a bool column counts True as 1, so (x > "
     "2).sum() counts the matches. Raises TypeError for a column type with "
     "no sum (string, most temporal types)."},
    {"min", series_reduce_scalar<&Series::min>, METH_NOARGS,
     "Smallest valid value. A bool column gives 0 or 1 and a string column "
     "the bytewise smallest string (empty when no value is valid); raises "
     "TypeError for a type with no order here."},
    {"max", series_reduce_scalar<&Series::max>, METH_NOARGS,
     "Largest valid value, over the same column types as min."},
    {"mean", series_reduce_f64<&Series::mean>, METH_NOARGS,
     "Mean of the valid values; a bool column gives the fraction that is "
     "True. Raises TypeError for a column type with no sum."},
    {"count", series_reduce_i64<&Series::count>, METH_NOARGS,
     "Non-null row count -> int."},
    {"product", series_reduce_scalar<&Series::product>, METH_NOARGS,
     "Product of non-null values -> scalar."},
    {"mode", series_reduce_scalar<&Series::mode>, METH_NOARGS,
     "Most frequent non-null value -> scalar."},
    {"all", Series_all, METH_NOARGS,
     "Whether every non-null Bool value is true."},
    {"any", Series_any, METH_NOARGS,
     "Whether any non-null Bool value is true."},
    {"arg_min", series_reduce_i64<&Series::arg_min>, METH_NOARGS,
     "Index of the minimum non-null value (-1 if none)."},
    {"arg_max", series_reduce_i64<&Series::arg_max>, METH_NOARGS,
     "Index of the maximum non-null value (-1 if none)."},
    {"take", Series_take, METH_O,
     "take(indices) -> Series gathered at the given row indices."},
    {"where", Series_where, METH_VARARGS,
     "where(mask, other) -> Series: this value where the Bool mask is true, "
     "else other's (a null mask row takes other)."},
    {"filter", DFTU_PYCFUNCTION((core::kw_one<"mask", Series_filter>)),
     METH_VARARGS | METH_KEYWORDS,
     "filter(mask) -> Series keeping rows where the Bool mask is true."},
    {"argsort", DFTU_PYCFUNCTION(Series_argsort), METH_VARARGS | METH_KEYWORDS,
     "argsort(descending=False) -> Int64 Series of the sorted row order."},
    {"dictionary_encode", Series_dictionary_encode, METH_NOARGS,
     "Dictionary-encode a String/Binary column -> Series."},
    {"str_into", Series_str_into, METH_VARARGS,
     "str_into(out, na=None): a String column's rows into a numpy object "
     "array of its length, null rows holding `na`."},
    {"np_parts", Series_np_parts, METH_VARARGS,
     "np_parts(nullable=False, na=None) -> (values, mask, dtype): the column "
     "in one native pass for numpy.frombuffer (bytearrays; a list of str for "
     "a String column, null rows holding `na`); mask is 1 = null or None."},
    {"to_pylist", Series_to_pylist, METH_NOARGS,
     "to_pylist() -> list of Python values, None for null; TypeError for a "
     "column type with no native conversion."},
    {"physical", Series_physical, METH_NOARGS,
     "physical() -> Series: the integers of a date, time, timestamp, "
     "duration or float16 column (buffers shared)."},
    {"to_bytes", Series_to_bytes, METH_NOARGS,
     "to_bytes() -> bytes: the column in the native frame format."},
    {"item", Series_item, METH_O,
     "item(i) -> the value at row i as a Python object (None for null)."},
    {"materialize", Series_materialize, METH_NOARGS,
     "Materialize any encoding to a new FLAT Series (gather)."},
    {"__class_getitem__", Series_class_getitem, METH_O | METH_CLASS,
     "Series[T]: a generic alias, for annotations."},
    {"rename", Series_rename, METH_O,
     "rename(name) -> a new Series sharing this column's buffers, named name "
     "(a str) or unnamed (None). Only the scalar form of pandas' rename is "
     "supported."},
    {"share", Series_share, METH_NOARGS,
     "A new owned Series sharing this column's buffers zero-copy."},
    {"slice",
     DFTU_PYCFUNCTION((core::kw_pair<Series_slice, "offset", "length">)),
     METH_VARARGS | METH_KEYWORDS,
     "slice(offset, length) -> view Series of a row range, sharing the "
     "value buffers and keeping the layout; raises for list, map and struct."},
    {"str_eq", DFTU_PYCFUNCTION((core::kw_one<"rhs", Series_str_eq>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_eq(rhs) -> Bool Series where the string equals rhs."},
    {"str_contains",
     DFTU_PYCFUNCTION((core::kw_one<"needle", Series_str_contains>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_contains(needle) -> Bool Series where the string contains needle."},
    {"str_starts_with",
     DFTU_PYCFUNCTION((core::kw_one<"prefix", Series_str_starts_with>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_starts_with(prefix) -> Bool Series where the string starts with "
     "prefix."},
    {"str_ends_with",
     DFTU_PYCFUNCTION((core::kw_one<"suffix", Series_str_ends_with>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_ends_with(suffix) -> Bool Series where the string ends with suffix."},
    {"str_like", DFTU_PYCFUNCTION((core::kw_one<"pattern", Series_str_like>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_like(pattern) -> Bool Series matching the SQL LIKE / glob pattern "
     "(% any run, _ one char, backslash escapes a literal %/_/backslash)."},
    {"str_matches",
     DFTU_PYCFUNCTION((core::kw_one<"pattern", Series_str_matches>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_matches(pattern) -> Bool Series where the whole string matches the "
     "duql regex."},
    {"str_search",
     DFTU_PYCFUNCTION((core::kw_one<"pattern", Series_str_search>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_search(pattern) -> Bool Series where the duql regex matches "
     "anywhere in the string."},
    {"str_len_bytes", Series_str_len_bytes, METH_NOARGS,
     "str_len_bytes() -> Int64 Series of per-row byte length."},
    {"str_len_chars", Series_str_len_chars, METH_NOARGS,
     "str_len_chars() -> Int64 Series of per-row UTF-8 codepoint count."},
    {"str_find", DFTU_PYCFUNCTION((core::kw_one<"needle", Series_str_find>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_find(needle) -> Int64 Series of the first byte index, or -1."},
    {"fnv1a", Series_fnv1a, METH_NOARGS,
     "fnv1a() -> UInt64 Series of the FNV-1a 64 hash of each row's bytes."},
    {"hex64_parse", Series_hex64_parse, METH_NOARGS,
     "hex64_parse() -> UInt64 Series parsing each row as hex64; unparsable "
     "rows are null."},
    {"hex64_format", Series_hex64_format, METH_NOARGS,
     "hex64_format() -> String Series formatting each UInt64/Int64 row as "
     "dftracer's 16-lowercase-hex-digit form; the inverse of hex64_parse."},
    {"to_lowercase", Series_to_lowercase, METH_NOARGS,
     "to_lowercase() -> String Series, ASCII case fold (non-ASCII unchanged)."},
    {"to_uppercase", Series_to_uppercase, METH_NOARGS,
     "to_uppercase() -> String Series, ASCII case fold (non-ASCII unchanged)."},
    {"str_strip", Series_str_strip, METH_NOARGS,
     "str_strip() -> String Series trimmed of ASCII whitespace both ends."},
    {"str_lstrip", Series_str_lstrip, METH_NOARGS,
     "str_lstrip() -> String Series trimmed of leading ASCII whitespace."},
    {"str_rstrip", Series_str_rstrip, METH_NOARGS,
     "str_rstrip() -> String Series trimmed of trailing ASCII whitespace."},
    {"str_replace",
     DFTU_PYCFUNCTION((core::kw_pair<Series_str_replace, "pat", "repl">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_replace(pat, repl) -> String Series, first literal occurrence."},
    {"str_replace_all",
     DFTU_PYCFUNCTION((core::kw_pair<Series_str_replace_all, "pat", "repl">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_replace_all(pat, repl) -> String Series, all literal occurrences."},
    {"str_slice",
     DFTU_PYCFUNCTION((core::kw_pair<Series_str_slice, "start", "length">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_slice(start, length=-1) -> String Series substring by byte offset."},
    {"str_pad_start", DFTU_PYCFUNCTION(Series_str_pad_start),
     METH_VARARGS | METH_KEYWORDS,
     "str_pad_start(width, fill=' ') -> left-padded String Series."},
    {"str_pad_end", DFTU_PYCFUNCTION(Series_str_pad_end),
     METH_VARARGS | METH_KEYWORDS,
     "str_pad_end(width, fill=' ') -> right-padded String Series."},
    {"str_zfill", DFTU_PYCFUNCTION((core::kw_one<"width", Series_str_zfill>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_zfill(width) -> String Series left-padded with '0' after any sign."},
    {"str_split", DFTU_PYCFUNCTION((core::kw_one<"sep", Series_str_split>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_split(sep) -> List<String> Series split on the literal sep."},
    {"str_extract",
     DFTU_PYCFUNCTION((core::kw_pair<Series_str_extract, "pattern", "group">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_extract(pattern, group=1) -> String Series of the regex capture "
     "group per row, null where there is no match."},
    {"str_regex_replace",
     DFTU_PYCFUNCTION(
         (core::kw_pair<Series_str_regex_replace, "pattern", "to">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_regex_replace(pattern, to) -> String Series with every regex match "
     "replaced by to ($n, ${n}, ${name} insert a group, $$ a dollar sign). "
     "Raises ValueError for an invalid pattern or replacement."},
    {"list_len", Series_list_len, METH_NOARGS,
     "list_len() -> Int64 Series of each row's list length."},
    {"list_get", DFTU_PYCFUNCTION((core::kw_one<"index", Series_list_get>)),
     METH_VARARGS | METH_KEYWORDS,
     "list_get(index) -> the element at index of each row's list (negative "
     "from the end), null where the list is too short."},
    {"list_join", DFTU_PYCFUNCTION((core::kw_one<"sep", Series_list_join>)),
     METH_VARARGS | METH_KEYWORDS,
     "list_join(sep) -> each row's list of strings joined with sep."},
    {"str_case", DFTU_PYCFUNCTION((core::kw_one<"code", Series_str_case>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_case(code) -> ASCII case form: 0 capitalize, 1 title, 2 swapcase."},
    {"str_is", DFTU_PYCFUNCTION((core::kw_one<"code", Series_str_is>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_is(code) -> Bool: 0 alnum, 1 alpha, 2 digit, 3 decimal, 4 numeric, "
     "5 space, 6 lower, 7 upper, 8 title (ASCII; an empty string is false)."},
    {"str_count", DFTU_PYCFUNCTION((core::kw_one<"pat", Series_str_count>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_count(pat) -> Int64 non-overlapping occurrences of the literal."},
    {"str_rfind", DFTU_PYCFUNCTION((core::kw_one<"needle", Series_str_rfind>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_rfind(needle) -> Int64 byte index of the last occurrence, or -1."},
    {"str_remove_prefix",
     DFTU_PYCFUNCTION((core::kw_one<"prefix", Series_str_remove_prefix>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_remove_prefix(prefix) -> each row without a leading prefix."},
    {"str_remove_suffix",
     DFTU_PYCFUNCTION((core::kw_one<"suffix", Series_str_remove_suffix>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_remove_suffix(suffix) -> each row without a trailing suffix."},
    {"str_repeat", DFTU_PYCFUNCTION((core::kw_one<"n", Series_str_repeat>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_repeat(n) -> each row repeated n times."},
    {"str_center", DFTU_PYCFUNCTION(Series_str_center),
     METH_VARARGS | METH_KEYWORDS,
     "str_center(width, fill=' ') -> each row centered in width bytes."},
    {"str_cat", DFTU_PYCFUNCTION((core::kw_one<"other", Series_str_cat>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_cat(other) -> row-wise concatenation with another String Series."},
    {"str_findall",
     DFTU_PYCFUNCTION((core::kw_one<"pattern", Series_str_findall>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_findall(pattern) -> List<String> of every regex match per row."},
    {"str_partition", DFTU_PYCFUNCTION(Series_str_partition),
     METH_VARARGS | METH_KEYWORDS,
     "str_partition(sep, from_right=False) -> List<String> of (head, sep, "
     "tail) per row at the first (last) occurrence."},
    {"str_normalize",
     DFTU_PYCFUNCTION((core::kw_one<"form", Series_str_normalize>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_normalize(form) -> each row in the Unicode form (0 NFC, 1 NFKC, "
     "2 NFD, 3 NFKD); null for a row that is not valid UTF-8."},
    {"str_translate",
     DFTU_PYCFUNCTION((core::kw_pair<Series_str_translate, "from_", "to">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_translate(from, to) -> each codepoint of every row replaced by the "
     "String Series pair: from holds one codepoint per row, to the "
     "replacement (null deletes)."},
    {"str_wrap",
     DFTU_PYCFUNCTION((core::kw_pair<Series_str_wrap, "width", "flags">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_wrap(width, flags) -> each row as textwrap.wrap lines joined by a "
     "newline; flags is an OR of 1 expand_tabs, 2 replace_whitespace, "
     "4 drop_whitespace, 8 break_long_words, 16 break_on_hyphens."},
    {"dt_part",
     DFTU_PYCFUNCTION((core::kw_pair<Series_dt_part, "code", "unit">)),
     METH_VARARGS | METH_KEYWORDS,
     "dt_part(code, unit=2) -> Int64 calendar part (0 year .. 16 epoch days) "
     "of a Timestamp / Date / Duration column, or an Int64 column read in "
     "unit (0 s, 1 ms, 2 us, 3 ns)."},
    {"dt_format",
     DFTU_PYCFUNCTION((core::kw_pair<Series_dt_format, "fmt", "unit">)),
     METH_VARARGS | METH_KEYWORDS,
     "dt_format(fmt, unit=2) -> String strftime of a Timestamp / Date column, "
     "or an Int64 column read in unit (0 s, 1 ms, 2 us, 3 ns); a Timestamp "
     "uses its own unit. Directives: %Y %y %m %d %H %I %M %S %f %j %a %A %b "
     "%B %p %F %T %s %z %Z %%. Raises ValueError for another directive."},
    {"dt_round",
     DFTU_PYCFUNCTION((core::kw_pair<Series_dt_round, "every", "mode">)),
     METH_VARARGS | METH_KEYWORDS,
     "dt_round(every, mode) -> each instant rounded to a multiple of every "
     "(the column's unit) by mode 0 floor, 1 ceil, 2 round (half to even)."},
    {"with_timezone",
     DFTU_PYCFUNCTION((core::kw_one<"tz", Series_with_timezone>)),
     METH_VARARGS | METH_KEYWORDS,
     "with_timezone(tz) -> this Timestamp column under zone tz ('' for "
     "naive), the same instants and buffers."},
    {"is_null", Series_is_null, METH_O, "is_null(i) -> whether row i is null."},
    {"num_children", Series_num_children, METH_NOARGS,
     "Child count: 1 for a List, the field count for a Struct, else 0."},
    {"child", DFTU_PYCFUNCTION((core::kw_one<"i", Series_child>)),
     METH_VARARGS | METH_KEYWORDS,
     "child(i) -> child Series (List: 0=values; Struct: field i)."},
    {nullptr, nullptr, 0, nullptr}};

PySequenceMethods Series_as_sequence = {};
PyBufferProcs Series_as_buffer = {};

PyGetSetDef Series_getset[] = {
    {"type", Series_get_type, nullptr, "TypeId code.", nullptr},
    {"length", Series_get_length, nullptr, "Row count.", nullptr},
    {"null_count", Series_get_null_count, nullptr, "Null row count.", nullptr},
    {"encoding", Series_get_encoding, nullptr, "Encoding code.", nullptr},
    {"time_unit", Series_get_time_unit, nullptr,
     "dftu_time_unit code of a Timestamp / Time / Duration column (0 s, 1 "
     "ms, 2 us, 3 ns).",
     nullptr},
    {"timezone", Series_get_timezone, nullptr,
     "Zone name of a Timestamp column; '' for a naive one or another type.",
     nullptr},
    {"name", Series_get_name, Series_set_name,
     "The name of the Series, None when it has none. Ops keep it as pandas "
     "does: an elementwise or transform op gives the result this name, a "
     "binary op between two Series keeps it only when both names are equal. "
     "A frame column holds the name of its column. Assigning renames this "
     "Series in place (a str or None); the values stay shared.",
     nullptr},
    {"nbytes", Series_get_nbytes, nullptr,
     "Bytes held by the column's buffers, children included.", nullptr},
    {"is_json", Series_get_is_json, nullptr,
     "Whether this String column holds canonical JSON text per value.",
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}};

}  // namespace

}  // namespace dftracer::utils::python::frame::series_detail

namespace dftracer::utils::python::frame {

TypeBinding series_binding() {
    return {"Series", &series_detail::SeriesType,
            &series_detail::g_series_type};
}

PyObject* wrap_vec_column(dftracer::utils::dataframe::Series&& col) {
    return series_detail::make_series(std::move(col));
}

const dftracer::utils::dataframe::Series* unwrap_vec_column(PyObject* o) {
    if (!PyObject_TypeCheck(o, &series_detail::SeriesType)) return nullptr;
    return &reinterpret_cast<series_detail::SeriesObject*>(o)->col;
}

dftracer::utils::dataframe::Series column_from_arrow(PyObject* arrow_array) {
    return series_detail::import_arrow_column(arrow_array);
}

int init_series(PyObject* m) {
    using namespace series_detail;
    SeriesType = {};
    SeriesType.ob_base = {PyObject_HEAD_INIT(nullptr) 0};
    SeriesType.tp_name = "dftracer_utils_ext.Series";
    SeriesType.tp_basicsize = sizeof(SeriesObject);
    SeriesType.tp_weaklistoffset = WEAKLIST_OFFSET;
    SeriesType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    SeriesType.tp_doc = "Native columnar Series (SIMD ops + Arrow capsule).";
    SeriesType.tp_dealloc = reinterpret_cast<destructor>(Series_dealloc);
    Series_as_sequence.sq_length = Series_length_sq;
    SeriesType.tp_as_sequence = &Series_as_sequence;
    Series_as_buffer.bf_getbuffer = Series_getbuffer;
    SeriesType.tp_as_buffer = &Series_as_buffer;
    SeriesType.tp_methods = Series_methods;
    SeriesType.tp_getset = Series_getset;
    // Created by the kernels and the _series_from_* functions; the Python
    // subclass builds one from Python data in its own __new__.
    SeriesType.tp_new = nullptr;
    if (register_type(m, &SeriesType, "Series") < 0) return -1;

    static PyMethodDef from_arrow_def = {
        "_series_from_arrow", vec_from_arrow, METH_O,
        "_series_from_arrow(arrow_array) -> Series: import a pyarrow array "
        "into a native columnar Series."};
    PyObject* fn = PyCFunction_NewEx(&from_arrow_def, nullptr, nullptr);
    if (!fn) return -1;
    if (PyModule_AddObject(m, "_series_from_arrow", fn) < 0) {
        Py_DECREF(fn);
        return -1;
    }

    static PyMethodDef from_list_def = {
        "_series_from_list", vec_from_list, METH_VARARGS,
        "_series_from_list(values[, type_id]) -> Series: a column from a list "
        "of bool, int, float, str, bytes, datetime, date, time, timedelta or "
        "Decimal (None is null), typed `type_id` when given; TypeError "
        "otherwise."};
    PyObject* list_fn = PyCFunction_NewEx(&from_list_def, nullptr, nullptr);
    if (!list_fn) return -1;
    if (PyModule_AddObject(m, "_series_from_list", list_fn) < 0) {
        Py_DECREF(list_fn);
        return -1;
    }

    static PyMethodDef nulls_def = {
        "_series_nulls", series_nulls, METH_VARARGS,
        "_series_nulls(type_id, n) -> Series: a column of `n` nulls of that "
        "type."};
    PyObject* nulls_fn = PyCFunction_NewEx(&nulls_def, nullptr, nullptr);
    if (!nulls_fn) return -1;
    if (PyModule_AddObject(m, "_series_nulls", nulls_fn) < 0) {
        Py_DECREF(nulls_fn);
        return -1;
    }

    static PyMethodDef retype_def = {
        "_series_retype", series_retype, METH_VARARGS,
        "_series_retype(series, type_id[, unit, timezone, precision, scale, "
        "width]) -> Series: the same buffers read as another type of the "
        "same width."};
    PyObject* retype_fn = PyCFunction_NewEx(&retype_def, nullptr, nullptr);
    if (!retype_fn) return -1;
    if (PyModule_AddObject(m, "_series_retype", retype_fn) < 0) {
        Py_DECREF(retype_fn);
        return -1;
    }
    static PyMethodDef from_bytes_def = {
        "_series_from_bytes", series_from_bytes, METH_O,
        "_series_from_bytes(bytes) -> Series written by Series.to_bytes."};
    PyObject* bytes_fn = PyCFunction_NewEx(&from_bytes_def, nullptr, nullptr);
    if (!bytes_fn) return -1;
    if (PyModule_AddObject(m, "_series_from_bytes", bytes_fn) < 0) {
        Py_DECREF(bytes_fn);
        return -1;
    }

    static PyMethodDef from_numpy_def = {
        "_series_from_numpy", vec_from_numpy, METH_VARARGS,
        "_series_from_numpy(array[, mask]) -> Series: import a 1-D "
        "C-contiguous numeric, float16 or bool numpy array (buffer protocol; "
        "numbers borrow the memory, no pyarrow); `mask` is a bool array, true "
        "where null."};
    PyObject* np_fn = PyCFunction_NewEx(&from_numpy_def, nullptr, nullptr);
    if (!np_fn) return -1;
    if (PyModule_AddObject(m, "_series_from_numpy", np_fn) < 0) {
        Py_DECREF(np_fn);
        return -1;
    }
    return 0;
}

}  // namespace dftracer::utils::python::frame

PyTypeObject* dftracer::utils::python::core::NativeTraits<
    dftracer::utils::dataframe::Series>::type() {
    return &dftracer::utils::python::frame::series_detail::SeriesType;
}
