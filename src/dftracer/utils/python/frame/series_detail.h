// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FRAME_SERIES_DETAIL_H
#define DFTRACER_UTILS_PYTHON_FRAME_SERIES_DETAIL_H

#include <Python.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/python/core/call.h>

// Private seam shared by the Series extension translation units (series.cpp
// plus series/{arith,math,window,stats,reduce,string,arrow}.cpp). series.cpp
// owns the SeriesType type object and defines make_series; every
// family TU calls them and defines the method bodies referenced by the
// PyMethodDef table back in series.cpp.
namespace dftracer::utils::python::frame::series_detail {

using core::as_native;
using core::as_native_or_null;
using core::guard;
using core::guard_as;
using core::parse_kw;
using core::release_gil;
using core::without_gil;
using dataframe::Series;
using dataframe::TypeId;

struct SeriesObject {
    PyObject_HEAD PyObject* weaklist;
    Series col;
    // Buffer-protocol shape[0] and strides[0]; a column is immutable, so these
    // are constant and every export can point at them (no per-export alloc).
    Py_ssize_t buf_shape;
    Py_ssize_t buf_stride;
    // Live buffer exports: `_replace` refuses while a reader points into the
    // column's buffers.
    Py_ssize_t exports;
};

// What s.str, s.dt and s.list hold: the Series they read. Defined in
// series/accessor.cpp.
struct AccessorObject {
    PyObject_HEAD PyObject* weaklist;
    PyObject* series;
};

int init_accessors(PyObject* m);

// The window types s.rolling(n) and s.ewm(alpha) build, and the module
// function _ewm_alpha. Defined in series/window.cpp.
int init_windows(PyObject* m);

}  // namespace dftracer::utils::python::frame::series_detail

template <>
struct dftracer::utils::python::core::NativeTraits<
    dftracer::utils::dataframe::Series> {
    static PyTypeObject* type();
    static constexpr const char* EXPECTED = "expected a Series";
    static dftracer::utils::dataframe::Series* get(PyObject* o) {
        return &reinterpret_cast<dftracer::utils::python::frame::series_detail::
                                     SeriesObject*>(o)
                    ->col;
    }
};

namespace dftracer::utils::python::frame::series_detail {

// The column behind a Series object, for a method that runs a kernel with the
// GIL released. `s.name = x` swaps the name of the object's own handle, which
// a released kernel copies into its result. A column long enough to release
// the GIL is therefore read through a share taken under the GIL, which has its
// own name; a shorter one is read in place, as no other thread runs meanwhile.
// With `share_always` the share is taken for every length, for a method whose
// release depends on more than this column (the sum of two lengths).
class Held {
   public:
    enum Mode { Required, Optional };

    explicit Held(PyObject* o, Mode mode = Required, bool share_always = false)
        : p_(mode == Required ? as_native<Series>(o)
                              : as_native_or_null<Series>(o)) {
        if (p_ && (share_always || p_->length() >= core::GIL_RELEASE_ROWS)) {
            copy_ = p_->share();
            p_ = &copy_;
        }
    }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;

    Series* get() const { return p_; }

   private:
    Series* p_;
    Series copy_;
};

// Runs a kernel over `a` with the GIL released when `a` is long. The body
// touches no Python object.
template <class Fn>
Series kernel(const Series& a, Fn&& fn) {
    return without_gil(a.length(), fn);
}

// Defined in series.cpp (owns the SeriesType type object).
PyObject* make_series(Series&& col);

// @p out under the name of @p source, as the Series methods give it; @p out
// unchanged when it is invalid.
Series named_like(Series out, const Series& source);
// @p out under the name @p a and @p b share, else unnamed (the pandas rule).
Series named_like(Series out, const Series& a, const Series& b);

// Defined in series/reduce.cpp.
PyObject* scalar_to_py(dftu_scalar v);

// No-argument reducers that call a Series member and wrap the result. Method is
// the member (returning dftu_scalar / double / int64), bound as a template
// argument so each stays one PyMethodDef line.
//
// A reducer that refuses its column (sum of a string, ...) returns an error
// from the C++ member; it reaches Python as a TypeError that carries the
// refusal's message.
// A column with no valid value reduces to None. The engine says so by a NULL
// scalar (min, max) or a NaN (mean, median, quantile, variance, stddev);
// reduced_float is the one place the NaN becomes None, as a NaN over present
// values stays a float.
inline PyObject* reduced_float(const Series& a, double v) {
    if (v != v && a.count() == 0) Py_RETURN_NONE;
    return PyFloat_FromDouble(v);
}

template <auto Method>
PyObject* series_reduce_scalar(PyObject* self, PyObject*) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    auto out = without_gil(a->length(), [&] { return (a->*Method)(); });
    if constexpr (std::is_same_v<decltype(out),
                                 dftracer::utils::dstd::Result<
                                     dftracer::utils::dataframe::Scalar>>) {
        if (!out) {
            PyErr_SetString(PyExc_TypeError, out.error().c_str());
            return nullptr;
        }
        return scalar_to_py(*out);
    } else {
        return scalar_to_py(out);
    }
}
template <auto Method>
PyObject* series_reduce_f64(PyObject* self, PyObject*) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    auto out = without_gil(a->length(), [&] { return (a->*Method)(); });
    if (!out) {
        PyErr_SetString(PyExc_TypeError, out.error().c_str());
        return nullptr;
    }
    return reduced_float(*a, *out);
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
PyObject* Series_nb_add(PyObject* a, PyObject* b);
PyObject* Series_nb_sub(PyObject* a, PyObject* b);
PyObject* Series_nb_mul(PyObject* a, PyObject* b);
PyObject* Series_nb_true_divide(PyObject* a, PyObject* b);
PyObject* Series_nb_floor_divide(PyObject* a, PyObject* b);
PyObject* Series_nb_remainder(PyObject* a, PyObject* b);
PyObject* Series_nb_power(PyObject* a, PyObject* b, PyObject* modulus);
PyObject* Series_richcompare(PyObject* self, PyObject* other, int op);
PyObject* Series_cast(PyObject* self, PyObject* target);
PyObject* Series_prim(PyObject* self, PyObject* code);
PyObject* Series_compare(PyObject* self, PyObject* args);
PyObject* Series_logical(PyObject* self, PyObject* args);

// series/math.cpp

// series/window.cpp
PyObject* Series_rolling(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_ewm(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_rolling_sum(PyObject* self, PyObject* arg);
PyObject* Series_rolling_mean(PyObject* self, PyObject* arg);
PyObject* Series_rolling_min(PyObject* self, PyObject* arg);
PyObject* Series_rolling_max(PyObject* self, PyObject* arg);
PyObject* Series_rolling_quantile(PyObject* self, PyObject* args);
PyObject* Series_ewm_mean(PyObject* self, PyObject* arg);
PyObject* Series_ewm_std(PyObject* self, PyObject* arg);
PyObject* Series_cut(PyObject* self, PyObject* other);
PyObject* Series_qcut(PyObject* self, PyObject* arg);

// series/stats.cpp
PyObject* Series_quantile(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_median(PyObject* self, PyObject*);
PyObject* Series_variance(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_stddev(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_value_counts(PyObject* self, PyObject*);
PyObject* Series_is_sorted(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_is_in(PyObject* self, PyObject* other);
PyObject* Series_sort(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_head(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_tail(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_shift(PyObject* self, PyObject* arg);
PyObject* Series_top_k(PyObject* self, PyObject* arg);
PyObject* Series_bottom_k(PyObject* self, PyObject* arg);
PyObject* Series_sample(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_rank(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_search_sorted(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_unique(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_duplicated(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_take(PyObject* self, PyObject* seq);
PyObject* Series_filter(PyObject* self, PyObject* other);
PyObject* Series_where(PyObject* self, PyObject* args);
PyObject* Series_argsort(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_arg_sort(PyObject* self, PyObject* args, PyObject* kwds);
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
PyObject* Series_str_find(PyObject* self, PyObject* arg);
PyObject* Series_str_replace(PyObject* self, PyObject* args);
PyObject* Series_str_replace_all(PyObject* self, PyObject* args);
PyObject* Series_str_slice(PyObject* self, PyObject* args);
PyObject* Series_str_pad_start(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_str_pad_end(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* Series_str_zfill(PyObject* self, PyObject* arg);
PyObject* Series_str_split(PyObject* self, PyObject* arg);
PyObject* Series_str_extract(PyObject* self, PyObject* args);
PyObject* Series_str_regex_replace(PyObject* self, PyObject* args);
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
PyObject* Series_str_normalize(PyObject* self, PyObject* arg);
PyObject* Series_str_translate(PyObject* self, PyObject* args);
PyObject* Series_str_wrap(PyObject* self, PyObject* args);
PyObject* Series_dt_part(PyObject* self, PyObject* args);
PyObject* Series_dt_format(PyObject* self, PyObject* args);
PyObject* Series_dt_round(PyObject* self, PyObject* args);
PyObject* Series_dt_as_unit(PyObject* self, PyObject* args);
/// Sets the Python error of this thread's last time zone failure and returns
/// true; false when there was none.
bool raise_tz_failure();
PyObject* Series_with_timezone(PyObject* self, PyObject* arg);
PyObject* Series_tz_localize(PyObject* self, PyObject* args, PyObject* kwds);

// series/arrow.cpp
PyObject* Series_arrow_c_array(PyObject* self, PyObject* args);
PyObject* Series_arrow_c_stream(PyObject* self, PyObject* args);
Series import_arrow_column(PyObject* obj);
PyObject* vec_from_arrow(PyObject* self, PyObject* obj);
PyObject* vec_from_numpy(PyObject* self, PyObject* obj);

// series/convert.cpp
PyObject* Series_to_pylist(PyObject* self, PyObject* args);
PyObject* Series_np_parts(PyObject* self, PyObject* args);
PyObject* Series_str_into(PyObject* self, PyObject* args);
PyObject* Series_item(PyObject* self, PyObject* arg);
PyObject* Series_item_unlabelled(PyObject* self, PyObject* arg);
PyObject* vec_from_list(PyObject* self, PyObject* args);

// series/typed.cpp
// `s` (or its materialization) read as `type` with the given parameters,
// sharing its buffers; an invalid Series when the widths differ.
Series retype_series(const Series& s, TypeId type, dataframe::TimeUnit unit,
                     dftracer::utils::dstd::StringView zone, int precision,
                     int scale, int width);
PyObject* Series_physical(PyObject* self, PyObject*);
PyObject* Series_dictionary_parts(PyObject* self, PyObject*);
PyObject* Series_null_bytes(PyObject* self, PyObject*);
PyObject* Series_plain_text(PyObject* self, PyObject*);
PyObject* Series_nulls_like(PyObject* self, PyObject* arg);
PyObject* Series_joined_dictionary(PyObject* self, PyObject*);
/// The lookup table of @ref null_mask_bytes: 8 mask bytes per validity byte.
const std::uint64_t* null_expand_table();
/// @p n bytes into @p dst, 1 where the validity bit is 0.
void expand_null_bytes(const std::uint8_t* valid, std::int64_t n,
                       std::uint8_t* dst);
/// `n` bytes, 1 where the validity bit is 0, as a new bytearray.
PyObject* null_mask_bytes(const std::uint8_t* valid, std::int64_t n);
PyObject* series_from_dictionary(PyObject* self, PyObject* args);
PyObject* Series_get_nbytes(PyObject* self, void*);
PyObject* Series_to_bytes(PyObject* self, PyObject*);
PyObject* series_retype(PyObject* self, PyObject* args);
PyObject* series_list_of(PyObject* self, PyObject* args);
PyObject* series_from_bytes(PyObject* self, PyObject* obj);
PyObject* series_nulls(PyObject* self, PyObject* args);
PyObject* number_rows(PyObject* self, PyObject* args);
PyObject* unstack_positions(PyObject* self, PyObject* args);
PyObject* unstack_columns(PyObject* self, PyObject* args);
PyObject* tz_set_search_path(PyObject* self, PyObject* arg);

}  // namespace dftracer::utils::python::frame::series_detail

#endif  // DFTRACER_UTILS_PYTHON_FRAME_SERIES_DETAIL_H
