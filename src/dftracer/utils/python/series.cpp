// A native vec column exposed to Python: the DSL and pipeline operate on our
// own SIMD columnar format (dataframe::Series, Highway kernels) and only cross
// to Arrow at the edges - construction from a pyarrow array and export via the
// Arrow PyCapsule interface (__arrow_c_array__).
//
// This is the entry translation unit: it owns the SeriesType type object, the
// PyMethodDef table, init_series, and the shared make_series/as_series
// file-statics. The method bodies live in series/{arith,math,window,stats,
// reduce,string,arrow}.cpp and are declared in series_detail.h.

#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/series.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/series_detail.h>

#include <cstdint>
#include <new>
#include <utility>

namespace dftracer::utils::python::series_detail {

namespace {

PyTypeObject SeriesType;

void Series_dealloc(SeriesObject* self) {
    self->col.~Series();
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

}  // namespace

PyObject* make_series(Series&& col) {
    if (!col.valid()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "dataframe kernel produced a null column");
        return nullptr;
    }
    auto* self =
        reinterpret_cast<SeriesObject*>(SeriesType.tp_alloc(&SeriesType, 0));
    if (!self) return nullptr;
    new (&self->col) Series(std::move(col));
    return reinterpret_cast<PyObject*>(self);
}

Series* as_series(PyObject* o) {
    if (!PyObject_TypeCheck(o, &SeriesType)) {
        PyErr_SetString(PyExc_TypeError, "expected a Series");
        return nullptr;
    }
    return &reinterpret_cast<SeriesObject*>(o)->col;
}

Series* as_series_or_null(PyObject* o) {
    if (!PyObject_TypeCheck(o, &SeriesType)) return nullptr;
    return &reinterpret_cast<SeriesObject*>(o)->col;
}

namespace {

Py_ssize_t Series_length_sq(PyObject* self) {
    Series* a = as_series(self);
    if (!a) return -1;
    return static_cast<Py_ssize_t>(a->length());
}

// Expose a FLAT, non-null, fixed-width numeric column as a 1-D read-only buffer
// so numpy reads it (np.asarray) with no pyarrow. Bit-packed Bool, strings,
// nulls, and non-FLAT encodings raise BufferError; the Python wrapper then
// falls back to the Arrow path.
int Series_getbuffer(PyObject* self, Py_buffer* view, int flags) {
    view->obj = nullptr;
    Series* a = as_series(self);
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
            fmt = "q";
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
            fmt = "Q";
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
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyLong_FromLong(static_cast<long>(a->type()));
}
PyObject* Series_get_length(PyObject* self, void*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyLong_FromLongLong(a->length());
}
PyObject* Series_get_null_count(PyObject* self, void*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyLong_FromLongLong(a->null_count());
}
PyObject* Series_get_encoding(PyObject* self, void*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyLong_FromLong(static_cast<long>(a->encoding()));
}
PyObject* Series_get_time_unit(PyObject* self, void*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyLong_FromLong(
        static_cast<long>(dftu_series_time_unit(a->handle())));
}

PyObject* Series_get_timezone(PyObject* self, void*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyUnicode_FromString(dftu_series_timezone(a->handle()));
}

PyMethodDef Series_methods[] = {
    {"add", Series_add, METH_O, "Elementwise a + b."},
    {"sub", Series_sub, METH_O, "Elementwise a - b."},
    {"mul", Series_mul, METH_O, "Elementwise a * b."},
    {"div", Series_div, METH_O, "Elementwise a / b."},
    {"add_scalar", Series_add_scalar, METH_O, "Broadcast a + scalar."},
    {"sub_scalar", Series_sub_scalar, METH_O, "Broadcast a - scalar."},
    {"mul_scalar", Series_mul_scalar, METH_O, "Broadcast a * scalar."},
    {"div_scalar", Series_div_scalar, METH_O, "Broadcast a / scalar."},
    {"floordiv", Series_floordiv, METH_VARARGS,
     "floordiv(other, reverse=False) -> a // other (a Series or a number; "
     "reverse computes other // a), Python's floor rule; a zero divisor is "
     "null."},
    {"mod", Series_mod, METH_VARARGS,
     "mod(other, reverse=False) -> a % other with the divisor's sign; a zero "
     "divisor is null."},
    {"pow", Series_pow, METH_VARARGS,
     "pow(other, reverse=False) -> a ** other; Int64 for two integers (a "
     "negative exponent is null), else Float64."},
    {"ffill", series_unary<dftu_series_ffill>, METH_NOARGS,
     "ffill() -> nulls filled from the nearest present value before them."},
    {"bfill", series_unary<dftu_series_bfill>, METH_NOARGS,
     "bfill() -> nulls filled from the nearest present value after them."},
    {"cast", Series_cast, METH_O, "Cast to a numeric TypeId code."},
    {"prim", Series_prim, METH_O,
     "Apply a unary numeric primitive by Prim code (i64 in, i64 out)."},
    {"compare", Series_compare, METH_VARARGS,
     "compare(op, scalar) -> Bool column (op is a DFTU_CMP_* code)."},
    {"logical", Series_logical, METH_VARARGS,
     "logical(op, other) -> Bool column (op is a DFTU_LOGICAL_* code)."},
    {"logical_not", Series_logical_not, METH_NOARGS,
     "Boolean NOT of a Bool column."},
    {"__arrow_c_array__", Series_arrow_c_array, METH_VARARGS,
     "Arrow PyCapsule export: (schema_capsule, array_capsule)."},
    {"quantile", Series_quantile, METH_O,
     "quantile(q) -> float (linear interpolation, SIMD sort)."},
    {"median", Series_median, METH_NOARGS, "Median (quantile 0.5)."},
    {"variance", Series_variance, METH_VARARGS,
     "variance(sample=True) -> float."},
    {"stddev", Series_stddev, METH_VARARGS, "stddev(sample=True) -> float."},
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
    {"fillna", Series_fillna, METH_O,
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
    {"head", Series_head, METH_O, "head(n) -> first n rows Series."},
    {"tail", Series_tail, METH_O, "tail(n) -> last n rows Series."},
    {"reverse", series_unary<dftu_series_reverse>, METH_NOARGS,
     "Rows in reverse order -> Series."},
    {"shift", Series_shift, METH_O,
     "shift(n) -> Series shifted by n (vacated rows null)."},
    {"top_k", Series_top_k, METH_O, "top_k(k) -> the k largest rows Series."},
    {"bottom_k", Series_bottom_k, METH_O,
     "bottom_k(k) -> the k smallest rows Series."},
    {"sample", DFTU_PYCFUNCTION(Series_sample), METH_VARARGS | METH_KEYWORDS,
     "sample(n, seed=0) -> deterministic sample of n rows Series."},
    {"rank", DFTU_PYCFUNCTION(Series_rank), METH_VARARGS | METH_KEYWORDS,
     "rank(method='average', descending=False) -> Float64 Series "
     "(method: average|min|dense|ordinal)."},
    {"rolling", DFTU_PYCFUNCTION(Series_rolling), METH_VARARGS | METH_KEYWORDS,
     "rolling(window, op='sum') -> Float64 Series (op: sum|mean|min|max)."},
    {"rolling_var", Series_rolling_var, METH_O,
     "rolling_var(window) -> Float64 Series of per-window sample variance."},
    {"rolling_std", Series_rolling_std, METH_O,
     "rolling_std(window) -> Float64 Series of per-window sample std."},
    {"rolling_median", Series_rolling_median, METH_O,
     "rolling_median(window) -> Float64 Series of per-window median."},
    {"rolling_quantile", Series_rolling_quantile, METH_VARARGS,
     "rolling_quantile(window, q) -> Float64 Series of per-window quantile q."},
    {"ewm_mean", Series_ewm_mean, METH_O,
     "ewm_mean(alpha) -> Float64 Series, exponentially weighted mean."},
    {"ewm_std", Series_ewm_std, METH_O,
     "ewm_std(alpha) -> Float64 Series, exponentially weighted sample std."},
    {"cut", Series_cut, METH_O,
     "cut(breaks) -> Int32 Series binning by the ascending breaks Series."},
    {"qcut", Series_qcut, METH_O,
     "qcut(q) -> Int32 Series binning by the column's q-quantile edges."},
    {"search_sorted", Series_search_sorted, METH_O,
     "search_sorted(values) -> Int64 Series of lower-bound insertion indices."},
    {"interpolate", series_unary<dftu_series_interpolate>, METH_NOARGS,
     "interpolate() -> Float64 Series with null interiors linearly filled."},
    {"is_between", Series_is_between, METH_VARARGS,
     "is_between(lo, hi) -> Bool mask where lo <= value <= hi (SIMD)."},
    {"dot", Series_dot, METH_O,
     "dot(other) -> scalar sum of x[i]*y[i] over non-null pairs (SIMD)."},
    {"sum", series_reduce_scalar<&Series::sum>, METH_NOARGS,
     "Sum of non-null values -> scalar."},
    {"min", series_reduce_scalar<&Series::min>, METH_NOARGS,
     "Minimum non-null value -> scalar."},
    {"max", series_reduce_scalar<&Series::max>, METH_NOARGS,
     "Maximum non-null value -> scalar."},
    {"mean", series_reduce_f64<&Series::mean>, METH_NOARGS,
     "Mean of non-null values -> float."},
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
    {"filter", Series_filter, METH_O,
     "filter(mask) -> Series keeping rows where the Bool mask is true."},
    {"argsort", DFTU_PYCFUNCTION(Series_argsort), METH_VARARGS | METH_KEYWORDS,
     "argsort(descending=False) -> Int64 Series of the sorted row order."},
    {"dictionary_encode", Series_dictionary_encode, METH_NOARGS,
     "Dictionary-encode a String/Binary column -> Series."},
    {"materialize", Series_materialize, METH_NOARGS,
     "Materialize any encoding to a new FLAT Series (gather)."},
    {"share", Series_share, METH_NOARGS,
     "A new owned Series sharing this column's buffers zero-copy."},
    {"slice", Series_slice, METH_VARARGS,
     "slice(offset, length) -> zero-copy view Series of a FLAT row range."},
    {"str_eq", Series_str_eq, METH_O,
     "str_eq(rhs) -> Bool Series where the string equals rhs."},
    {"str_contains", Series_str_contains, METH_O,
     "str_contains(needle) -> Bool Series where the string contains needle."},
    {"str_starts_with", Series_str_starts_with, METH_O,
     "str_starts_with(prefix) -> Bool Series where the string starts with "
     "prefix."},
    {"str_ends_with", Series_str_ends_with, METH_O,
     "str_ends_with(suffix) -> Bool Series where the string ends with suffix."},
    {"str_like", Series_str_like, METH_O,
     "str_like(pattern) -> Bool Series matching the SQL LIKE / glob pattern "
     "(% any run, _ one char, backslash escapes a literal %/_/backslash)."},
    {"str_matches", Series_str_matches, METH_O,
     "str_matches(pattern) -> Bool Series where the whole string matches the "
     "ECMAScript regex."},
    {"str_search", Series_str_search, METH_O,
     "str_search(pattern) -> Bool Series where the ECMAScript regex matches "
     "anywhere in the string."},
    {"str_len_bytes", Series_str_len_bytes, METH_NOARGS,
     "str_len_bytes() -> Int64 Series of per-row byte length."},
    {"str_len_chars", Series_str_len_chars, METH_NOARGS,
     "str_len_chars() -> Int64 Series of per-row UTF-8 codepoint count."},
    {"str_find", Series_str_find, METH_O,
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
    {"str_replace", Series_str_replace, METH_VARARGS,
     "str_replace(pat, repl) -> String Series, first literal occurrence."},
    {"str_replace_all", Series_str_replace_all, METH_VARARGS,
     "str_replace_all(pat, repl) -> String Series, all literal occurrences."},
    {"str_slice", Series_str_slice, METH_VARARGS,
     "str_slice(start, length=-1) -> String Series substring by byte offset."},
    {"str_pad_start", DFTU_PYCFUNCTION(Series_str_pad_start),
     METH_VARARGS | METH_KEYWORDS,
     "str_pad_start(width, fill=' ') -> left-padded String Series."},
    {"str_pad_end", DFTU_PYCFUNCTION(Series_str_pad_end),
     METH_VARARGS | METH_KEYWORDS,
     "str_pad_end(width, fill=' ') -> right-padded String Series."},
    {"str_zfill", Series_str_zfill, METH_O,
     "str_zfill(width) -> String Series left-padded with '0' after any sign."},
    {"str_split", Series_str_split, METH_O,
     "str_split(sep) -> List<String> Series split on the literal sep."},
    {"str_extract", Series_str_extract, METH_VARARGS,
     "str_extract(pattern, group=1) -> String Series of the regex capture "
     "group per row, null where there is no match."},
    {"list_len", Series_list_len, METH_NOARGS,
     "list_len() -> Int64 Series of each row's list length."},
    {"list_get", Series_list_get, METH_O,
     "list_get(index) -> the element at index of each row's list (negative "
     "from the end), null where the list is too short."},
    {"list_join", Series_list_join, METH_O,
     "list_join(sep) -> each row's list of strings joined with sep."},
    {"str_case", Series_str_case, METH_O,
     "str_case(code) -> ASCII case form: 0 capitalize, 1 title, 2 swapcase."},
    {"str_is", Series_str_is, METH_O,
     "str_is(code) -> Bool: 0 alnum, 1 alpha, 2 digit, 3 decimal, 4 numeric, "
     "5 space, 6 lower, 7 upper, 8 title (ASCII; an empty string is false)."},
    {"str_count", Series_str_count, METH_O,
     "str_count(pat) -> Int64 non-overlapping occurrences of the literal."},
    {"str_rfind", Series_str_rfind, METH_O,
     "str_rfind(needle) -> Int64 byte index of the last occurrence, or -1."},
    {"str_remove_prefix", Series_str_remove_prefix, METH_O,
     "str_remove_prefix(prefix) -> each row without a leading prefix."},
    {"str_remove_suffix", Series_str_remove_suffix, METH_O,
     "str_remove_suffix(suffix) -> each row without a trailing suffix."},
    {"str_repeat", Series_str_repeat, METH_O,
     "str_repeat(n) -> each row repeated n times."},
    {"str_center", DFTU_PYCFUNCTION(Series_str_center),
     METH_VARARGS | METH_KEYWORDS,
     "str_center(width, fill=' ') -> each row centered in width bytes."},
    {"str_cat", Series_str_cat, METH_O,
     "str_cat(other) -> row-wise concatenation with another String Series."},
    {"str_findall", Series_str_findall, METH_O,
     "str_findall(pattern) -> List<String> of every regex match per row."},
    {"str_partition", DFTU_PYCFUNCTION(Series_str_partition),
     METH_VARARGS | METH_KEYWORDS,
     "str_partition(sep, from_right=False) -> List<String> of (head, sep, "
     "tail) per row at the first (last) occurrence."},
    {"dt_part", Series_dt_part, METH_VARARGS,
     "dt_part(code, unit=2) -> Int64 calendar part (0 year .. 16 epoch days) "
     "of a Timestamp / Date / Duration column, or an Int64 column read in "
     "unit (0 s, 1 ms, 2 us, 3 ns)."},
    {"dt_round", Series_dt_round, METH_VARARGS,
     "dt_round(every, mode) -> each instant rounded to a multiple of every "
     "(the column's unit) by mode 0 floor, 1 ceil, 2 round (half to even)."},
    {"with_timezone", Series_with_timezone, METH_O,
     "with_timezone(tz) -> the same instants under zone tz ('' for naive); "
     "Timestamp only."},
    {"is_null", Series_is_null, METH_O, "is_null(i) -> whether row i is null."},
    {"num_children", Series_num_children, METH_NOARGS,
     "Child count: 1 for a List, the field count for a Struct, else 0."},
    {"child", Series_child, METH_O,
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
    {nullptr, nullptr, nullptr, nullptr, nullptr}};

}  // namespace

}  // namespace dftracer::utils::python::series_detail

namespace dftracer::utils::python {

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
    SeriesType.tp_name = "dftracer_utils_ext._Series";
    SeriesType.tp_basicsize = sizeof(SeriesObject);
    SeriesType.tp_flags = Py_TPFLAGS_DEFAULT;
    SeriesType.tp_doc =
        "Native columnar Series handle (SIMD ops + Arrow capsule).";
    SeriesType.tp_dealloc = reinterpret_cast<destructor>(Series_dealloc);
    Series_as_sequence.sq_length = Series_length_sq;
    SeriesType.tp_as_sequence = &Series_as_sequence;
    Series_as_buffer.bf_getbuffer = Series_getbuffer;
    SeriesType.tp_as_buffer = &Series_as_buffer;
    SeriesType.tp_methods = Series_methods;
    SeriesType.tp_getset = Series_getset;
    SeriesType.tp_new =
        nullptr;  // created only via _series_from_arrow / kernels
    if (register_type(m, &SeriesType, "_Series") < 0) return -1;

    static PyMethodDef from_arrow_def = {
        "_series_from_arrow", vec_from_arrow, METH_O,
        "_series_from_arrow(arrow_array) -> _Series: import a pyarrow array "
        "into a native columnar Series."};
    PyObject* fn = PyCFunction_NewEx(&from_arrow_def, nullptr, nullptr);
    if (!fn) return -1;
    if (PyModule_AddObject(m, "_series_from_arrow", fn) < 0) {
        Py_DECREF(fn);
        return -1;
    }

    static PyMethodDef from_numpy_def = {
        "_series_from_numpy", vec_from_numpy, METH_O,
        "_series_from_numpy(array) -> _Series: import a 1-D C-contiguous "
        "numeric numpy array (buffer protocol, zero-copy borrow, no pyarrow)."};
    PyObject* np_fn = PyCFunction_NewEx(&from_numpy_def, nullptr, nullptr);
    if (!np_fn) return -1;
    if (PyModule_AddObject(m, "_series_from_numpy", np_fn) < 0) {
        Py_DECREF(np_fn);
        return -1;
    }
    return 0;
}

}  // namespace dftracer::utils::python

#else   // !DFTRACER_UTILS_ENABLE_ARROW

namespace dftracer::utils::python {
int init_series(PyObject*) { return 0; }
const dftracer::utils::dataframe::Series* unwrap_vec_column(PyObject*) {
    return nullptr;
}
}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_ENABLE_ARROW
