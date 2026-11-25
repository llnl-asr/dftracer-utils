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
#include <dftracer/utils/python/core/api_methods.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/core/method.h>
#include <dftracer/utils/python/core/seq_helpers.h>
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
    if (out.valid() && source.valid()) {
        out.handle()->name = source.handle()->name;
        out.handle()->labels =
            out.length() == source.length() ? source.handle()->labels : nullptr;
    }
    return out;
}

Series named_like(Series out, const Series& a, const Series& b) {
    if (!out.valid()) return out;
    out.handle()->name =
        a.name() == b.name() ? a.handle()->name : dstd::SharedString{};
    const dftu_series* from = a.has_index() ? a.handle() : b.handle();
    out.handle()->labels =
        out.length() == from->length ? from->labels : nullptr;
    return out;
}

PyObject* make_series(Series&& col) {
    if (!col.valid()) {
        PyErr_SetString(core::g_dft_value_error,
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
// The buffer-protocol format and item size of a fixed-width numeric type;
// false, with a BufferError set, for any other type.
bool numeric_format(TypeId type, const char** fmt, Py_ssize_t* itemsize) {
    // numpy gives its fixed-width int64 types the C long code where long is 64
    // bits. With "q" and "Q" it makes longlong and ulonglong, which pandas
    // cannot factorize, so a merge on such a key fails.
    constexpr const char* INT64_FORMAT = sizeof(long) == 8 ? "l" : "q";
    constexpr const char* UINT64_FORMAT = sizeof(long) == 8 ? "L" : "Q";
    switch (type) {
        case TypeId::Int8:
            *fmt = "b";
            *itemsize = 1;
            return true;
        case TypeId::Int16:
            *fmt = "h";
            *itemsize = 2;
            return true;
        case TypeId::Int32:
            *fmt = "i";
            *itemsize = 4;
            return true;
        case TypeId::Int64:
            *fmt = INT64_FORMAT;
            *itemsize = 8;
            return true;
        case TypeId::Uint8:
            *fmt = "B";
            *itemsize = 1;
            return true;
        case TypeId::Uint16:
            *fmt = "H";
            *itemsize = 2;
            return true;
        case TypeId::Uint32:
            *fmt = "I";
            *itemsize = 4;
            return true;
        case TypeId::Uint64:
            *fmt = UINT64_FORMAT;
            *itemsize = 8;
            return true;
        case TypeId::Float32:
            *fmt = "f";
            *itemsize = 4;
            return true;
        case TypeId::Float64:
            *fmt = "d";
            *itemsize = 8;
            return true;
        default:
            PyErr_SetString(PyExc_BufferError,
                            "Series type has no plain numeric buffer");
            return false;
    }
}

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
    const char* fmt = nullptr;
    Py_ssize_t itemsize = 0;
    if (!numeric_format(static_cast<TypeId>(dftu_series_type(h)), &fmt,
                        &itemsize))
        return -1;
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
    ++obj->exports;
    return 0;
}

void Series_releasebuffer(PyObject* self, Py_buffer*) {
    --reinterpret_cast<SeriesObject*>(self)->exports;
}

// The values of a FLAT fixed-width numeric column as a buffer, nulls or not
// (the values under a null are unspecified). It holds its own share of the
// column, so a later _replace of the Series cannot change or free what a view
// reads; _values_buffer() wraps it in a memoryview.
struct ValuesObject {
    PyObject_HEAD Series* col;
    const char* format;
    Py_ssize_t itemsize;
    Py_ssize_t shape;
    Py_ssize_t stride;
};

PyTypeObject ValuesType;
PyBufferProcs Values_as_buffer = {};

void Values_dealloc(ValuesObject* self) {
    // made by dstd::make_unique, so freed by its deleter
    dftracer::utils::dstd::UniquePtr<Series>{self->col};
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

int Values_getbuffer(PyObject* self, Py_buffer* view, int flags) {
    view->obj = nullptr;
    auto* v = reinterpret_cast<ValuesObject*>(self);
    if ((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE) {
        PyErr_SetString(PyExc_BufferError, "Series buffer is read-only");
        return -1;
    }
    static const char EMPTY = 0;
    const void* data = dftu_series_data(v->col->handle());
    if (!data) data = &EMPTY;
    Py_INCREF(self);
    view->obj = self;
    view->buf = const_cast<void*>(data);
    view->len = v->shape * v->itemsize;
    view->readonly = 1;
    view->itemsize = v->itemsize;
    view->format =
        (flags & PyBUF_FORMAT) ? const_cast<char*>(v->format) : nullptr;
    view->ndim = 1;
    view->shape = &v->shape;
    view->strides = &v->stride;
    view->suboffsets = nullptr;
    view->internal = nullptr;
    return 0;
}

// _values_buffer() -> a read-only memoryview over the values of a FLAT numeric
// column, with no copy and nulls allowed. BufferError for any other column.
PyObject* Series_values_buffer(PyObject* self, PyObject*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    dftu_series* h = a->handle();
    if (dftu_series_encoding(h) != 0) {
        PyErr_SetString(PyExc_BufferError, "Series is not a flat column");
        return nullptr;
    }
    const char* fmt = nullptr;
    Py_ssize_t itemsize = 0;
    if (!numeric_format(static_cast<TypeId>(dftu_series_type(h)), &fmt,
                        &itemsize))
        return nullptr;
    const std::int64_t n = dftu_series_length(h);
    if (!dftu_series_data(h) && n > 0) {
        PyErr_SetString(PyExc_BufferError, "Series has no contiguous buffer");
        return nullptr;
    }
    auto* v = PyObject_New(ValuesObject, &ValuesType);
    if (!v) return nullptr;
    v->col = dftracer::utils::dstd::make_unique<Series>(a->share()).release();
    v->format = fmt;
    v->itemsize = itemsize;
    v->shape = static_cast<Py_ssize_t>(n);
    v->stride = itemsize;
    PyObject* view = PyMemoryView_FromObject(reinterpret_cast<PyObject*>(v));
    Py_DECREF(v);
    return view;
}

// _replace(other): this Series takes the column, the name and the labels of
// `other`, in place (setting with enlargement). A numpy array or memoryview
// over the old buffers would dangle, so it is a BufferError while one is live.
// A kernel that runs with the GIL released reads the column through `Held`, a
// share taken under the GIL, so this swap cannot free what it reads.
PyObject* Series_replace(PyObject* self, PyObject* other) {
    auto* a = reinterpret_cast<SeriesObject*>(self);
    Series* o = as_native<Series>(other);
    if (!as_native<Series>(self) || !o) return nullptr;
    if (a == reinterpret_cast<SeriesObject*>(other)) Py_RETURN_NONE;
    if (a->exports > 0) {
        PyErr_SetString(PyExc_BufferError,
                        "cannot replace a Series whose buffer is exported "
                        "(a numpy array or memoryview over it is alive)");
        return nullptr;
    }
    return guard([&]() -> PyObject* {
        a->col = o->share();
        Py_RETURN_NONE;
    });
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

PyObject* Series_get_decimal(PyObject* self, void*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return Py_BuildValue("(ii)", dftu_series_decimal_precision(a->handle()),
                         dftu_series_decimal_scale(a->handle()));
}

PyObject* Series_get_fixed_size(PyObject* self, void*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    return PyLong_FromLong(dftu_series_fixed_size(a->handle()));
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

// index_names() -> list[str]: the row label levels, in order; [] for none.
PyObject* Series_index_names(PyObject* self, PyObject*) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    const std::int32_t n = a->index_count();
    PyObject* list = PyList_New(n);
    if (!list) return nullptr;
    for (std::int32_t i = 0; i < n; ++i) {
        const dstd::StringView name = a->index_name(i);
        PyObject* text = PyUnicode_FromStringAndSize(
            name.data(), static_cast<Py_ssize_t>(name.size()));
        if (!text) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, i, text);
    }
    return list;
}

// _index_level(i) -> Series: the label column of level i, sharing its buffers.
PyObject* Series_index_level(PyObject* self, PyObject* arg) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    const long i = PyLong_AsLong(arg);
    if (i == -1 && PyErr_Occurred()) return nullptr;
    if (i < 0 || i >= a->index_count()) {
        PyErr_SetString(PyExc_IndexError, "index level out of range");
        return nullptr;
    }
    return make_series(a->index_level(static_cast<std::int32_t>(i)));
}

// _with_index(names, levels) -> Series: a copy sharing the buffers whose row
// labels are the `levels` (Series as long as this one) under `names`; empty
// lists drop them.
PyObject* Series_with_index(PyObject* self, PyObject* args) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    PyObject* names_obj = nullptr;
    PyObject* levels_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &names_obj, &levels_obj)) return nullptr;
    PyObject* names = PySequence_Fast(names_obj, "names must be a sequence");
    if (!names) return nullptr;
    PyObject* levels = PySequence_Fast(levels_obj, "levels must be a sequence");
    if (!levels) {
        Py_DECREF(names);
        return nullptr;
    }
    PyObject* result = nullptr;
    const Py_ssize_t n = PySequence_Fast_GET_SIZE(names);
    if (n != PySequence_Fast_GET_SIZE(levels)) {
        PyErr_SetString(PyExc_ValueError,
                        "with_index: names and levels differ in length");
    } else {
        dstd::Vector<dstd::StringView> views;
        dstd::Vector<Series> columns;
        bool ok = true;
        for (Py_ssize_t i = 0; ok && i < n; ++i) {
            dstd::StringView view;
            const Series* level =
                as_native_or_null<Series>(PySequence_Fast_GET_ITEM(levels, i));
            if (!level) {
                PyErr_SetString(PyExc_TypeError,
                                "with_index: levels must be Series");
                ok = false;
            } else if (PySequence_Fast_GET_ITEM(names, i) == Py_None ||
                       !name_from_py(PySequence_Fast_GET_ITEM(names, i),
                                     "with_index: a level name", view)) {
                if (!PyErr_Occurred())
                    PyErr_SetString(PyExc_TypeError,
                                    "with_index: a level name must be a str");
                ok = false;
            } else {
                views.push_back(view);
                columns.push_back(level->share());
            }
        }
        if (ok) {
            Series out = a->with_index(views, columns);
            if (!out.valid())
                PyErr_SetString(core::g_dft_value_error,
                                "with_index: each level must be as long as "
                                "the Series, not a list or a struct, and "
                                "named apart");
            else
                result = make_series(std::move(out));
        }
    }
    Py_DECREF(names);
    Py_DECREF(levels);
    return result;
}

// _locate(keys) -> (positions, missing): the rows whose labels equal the key
// columns, one per level (see Series::locate).
PyObject* Series_locate(PyObject* self, PyObject* keys_obj) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    dstd::Vector<Series> keys;
    if (!frame::series_list_from_py(keys_obj, "_locate: keys must be Series",
                                    keys))
        return nullptr;
    return frame::located_to_py(
        core::without_gil(a->length(), [&] { return a->locate(keys); }));
}

// _locate_range(start, stop) -> (offset, length): the rows between two label
// bounds, each a list of one-row Series per level, empty for an open end.
PyObject* Series_locate_range(PyObject* self, PyObject* args) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    PyObject* start_obj = nullptr;
    PyObject* stop_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &start_obj, &stop_obj)) return nullptr;
    dstd::Vector<Series> start;
    dstd::Vector<Series> stop;
    if (!frame::series_list_from_py(
            start_obj, "_locate_range: bounds must be Series", start) ||
        !frame::series_list_from_py(
            stop_obj, "_locate_range: bounds must be Series", stop))
        return nullptr;
    return frame::label_span_to_py(core::without_gil(
        a->length(), [&] { return a->locate_range(start, stop); }));
}

// The options of _reindex: method 0 none, 1 pad, 2 backfill, 3 nearest.
bool reindex_options(int method, long long limit, double tolerance,
                     PyObject* nanos, dataframe::ReindexOptions& out) {
    if (method < 0 || method > 3) {
        PyErr_SetString(PyExc_ValueError, "_reindex: method must be 0..3");
        return false;
    }
    out.method = static_cast<dataframe::FillMethod>(method);
    out.limit = limit;
    out.tolerance = tolerance;
    if (nanos != Py_None) {
        out.tolerance_nanos = PyLong_AsLongLong(nanos);
        if (out.tolerance_nanos == -1 && PyErr_Occurred()) return false;
        out.tolerance_is_nanos = true;
    }
    return true;
}

// _reindex(keys, method, limit, tolerance, fill) -> Series: the column on the
// label key Series (one per level) as dataframe::Series::reindex; `fill` is a
// one-row Series or None.
PyObject* Series_reindex(PyObject* self, PyObject* args) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    PyObject* keys_obj = nullptr;
    PyObject* fill_obj = nullptr;
    PyObject* nanos_obj = Py_None;
    int method = 0;
    long long limit = -1;
    double tolerance = 0;
    if (!PyArg_ParseTuple(args, "OiLdO|O", &keys_obj, &method, &limit,
                          &tolerance, &fill_obj, &nanos_obj))
        return nullptr;
    dstd::Vector<Series> keys;
    if (!frame::series_list_from_py(keys_obj, "_reindex: keys must be Series",
                                    keys))
        return nullptr;
    dataframe::ReindexOptions options;
    if (!reindex_options(method, limit, tolerance, nanos_obj, options))
        return nullptr;
    Series fill;
    if (fill_obj != Py_None) {
        const Series* f = as_native_or_null<Series>(fill_obj);
        if (!f) {
            PyErr_SetString(PyExc_TypeError, "_reindex: fill must be a Series");
            return nullptr;
        }
        fill = f->share();
    }
    auto r = core::without_gil(a->length(),
                               [&] { return a->reindex(keys, options, fill); });
    if (!r) {
        core::set_py_error(r.error());
        return nullptr;
    }
    return make_series(std::move(*r));
}

// _align(other, how) -> (left, right): the two columns on the joined labels
// (see dataframe::align_labels); how is a JoinHow (0 inner, 1 left, 2 right,
// 3 outer).
PyObject* Series_align(PyObject* self, PyObject* args) {
    Held held_a(self, Held::Required, true);
    Series* a = held_a.get();
    if (!a) return nullptr;
    PyObject* other_obj = nullptr;
    int how = 0;
    if (!PyArg_ParseTuple(args, "Oi", &other_obj, &how)) return nullptr;
    Held held_b(other_obj, Held::Required, true);
    Series* b = held_b.get();
    if (!b) return nullptr;
    auto r = core::without_gil(a->length() + b->length(), [&] {
        return dataframe::align_labels(*a, *b,
                                       static_cast<dataframe::JoinHow>(how));
    });
    if (!r) {
        core::set_py_error(r.error());
        return nullptr;
    }
    PyObject* left = make_series(std::move(r->left));
    PyObject* right = left ? make_series(std::move(r->right)) : nullptr;
    if (!right) {
        Py_XDECREF(left);
        return nullptr;
    }
    return Py_BuildValue("NN", left, right);
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
PyObject* Series_class_getitem(PyObject* cls, [[maybe_unused]] PyObject* args) {
#if PY_VERSION_HEX >= 0x03090000
    return Py_GenericAlias(cls, args);
#else
    Py_INCREF(cls);
    return cls;
#endif
}

PyMethodDef Series_methods[] = {
    {"add", DFTU_PYCFUNCTION((core::kw_one<"add", "other", Series_add>)),
     METH_VARARGS | METH_KEYWORDS,
     "Element-wise addition; mixed dtypes promote to a common type (numpy "
     "rules, e.g. uint64 + float64 -> float64)."},
    {"sub", DFTU_PYCFUNCTION((core::kw_one<"sub", "other", Series_sub>)),
     METH_VARARGS | METH_KEYWORDS,
     "Element-wise subtraction; mixed dtypes promote to a common type (numpy "
     "rules). Note: uint64 - uint64 wraps on underflow (like numpy); cast to "
     "a signed or float dtype first for interval/signed math."},
    {"mul", DFTU_PYCFUNCTION((core::kw_one<"mul", "other", Series_mul>)),
     METH_VARARGS | METH_KEYWORDS, "Elementwise a * b."},
    {"div", Series_div, METH_O, "Elementwise a / b."},
    {"add_scalar",
     DFTU_PYCFUNCTION((core::kw_one<"add_scalar", "v", Series_add_scalar>)),
     METH_VARARGS | METH_KEYWORDS, "Broadcast a + scalar."},
    {"sub_scalar",
     DFTU_PYCFUNCTION((core::kw_one<"sub_scalar", "v", Series_sub_scalar>)),
     METH_VARARGS | METH_KEYWORDS,
     "Broadcast a - scalar; a float scalar promotes an integer column to "
     "Float64 (numpy's weak-scalar rule). Note: uint64 - v wraps on "
     "underflow (like numpy); cast to a signed or float dtype first for "
     "interval/signed math."},
    {"mul_scalar",
     DFTU_PYCFUNCTION((core::kw_one<"mul_scalar", "v", Series_mul_scalar>)),
     METH_VARARGS | METH_KEYWORDS, "Broadcast a * scalar."},
    {"div_scalar",
     DFTU_PYCFUNCTION((core::kw_one<"div_scalar", "v", Series_div_scalar>)),
     METH_VARARGS | METH_KEYWORDS, "Broadcast a / scalar."},
    {"floordiv",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"floordiv", Series_floordiv, "other", "reverse">)),
     METH_VARARGS | METH_KEYWORDS,
     "floordiv(other, reverse=False) -> a // other (a Series or a number; "
     "reverse computes other // a), Python's floor rule; a zero divisor is "
     "null."},
    {"mod",
     DFTU_PYCFUNCTION((core::kw_pair<"mod", Series_mod, "other", "reverse">)),
     METH_VARARGS | METH_KEYWORDS,
     "mod(other, reverse=False) -> a % other with the divisor's sign; a zero "
     "divisor is null."},
    {"pow",
     DFTU_PYCFUNCTION((core::kw_pair<"pow", Series_pow, "other", "reverse">)),
     METH_VARARGS | METH_KEYWORDS,
     "pow(other, reverse=False) -> a ** other; Int64 for two integers (a "
     "negative exponent is null), else Float64."},
    {"_cast", Series_cast, METH_O,
     "Cast to a TypeId code; TypeError names both types for a pair the engine "
     "refuses."},
    {"prim", DFTU_PYCFUNCTION((core::kw_one<"prim", "op", Series_prim>)),
     METH_VARARGS | METH_KEYWORDS,
     "Apply a unary numeric primitive by Prim code (i64 in, i64 out)."},
    {"compare", Series_compare, METH_VARARGS,
     "compare(op, scalar) -> Bool column (op is a DFTU_CMP_* code)."},
    {"logical",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"logical", Series_logical, "op", "other">)),
     METH_VARARGS | METH_KEYWORDS,
     "logical(op, other) -> Bool column (op is a DFTU_LOGICAL_* code)."},
    {"__arrow_c_stream__", Series_arrow_c_stream, METH_VARARGS,
     "Stream the column: one Arrow array per chunk."},
    {"__arrow_c_array__", Series_arrow_c_array, METH_VARARGS,
     "Arrow PyCapsule export: (schema_capsule, array_capsule)."},
    {"quantile", DFTU_PYCFUNCTION(Series_quantile),
     METH_VARARGS | METH_KEYWORDS,
     "quantile(q=0.5, interpolation='linear') -> float: the exact quantile of "
     "the valid values. interpolation is 'linear', 'nearest', 'lower', "
     "'higher' or 'midpoint'; a NaN value ranks above every number."},
    {"median", Series_median, METH_NOARGS, "Median (quantile 0.5)."},
    {"variance", DFTU_PYCFUNCTION(Series_variance),
     METH_VARARGS | METH_KEYWORDS, "variance(sample=True) -> float."},
    {"stddev", DFTU_PYCFUNCTION(Series_stddev), METH_VARARGS | METH_KEYWORDS,
     "stddev(sample=True) -> float."},
    {"value_counts", Series_value_counts, METH_NOARGS,
     "value_counts() -> DataFrame of distinct values and counts, "
     "most-frequent first."},
    {"is_sorted", DFTU_PYCFUNCTION(Series_is_sorted),
     METH_VARARGS | METH_KEYWORDS, "is_sorted(descending=False) -> bool."},
    {"is_in", Series_is_in, METH_O,
     "is_in(values) -> Bool mask where the value is in the values Series."},
    {"sort", DFTU_PYCFUNCTION(Series_sort), METH_VARARGS | METH_KEYWORDS,
     "sort(descending=False, nulls_last=False) -> sorted Series; nulls first "
     "unless nulls_last."},
    {"head", DFTU_PYCFUNCTION(Series_head), METH_VARARGS | METH_KEYWORDS,
     "head(n=5) -> first n rows Series."},
    {"tail", DFTU_PYCFUNCTION(Series_tail), METH_VARARGS | METH_KEYWORDS,
     "tail(n=5) -> last n rows Series."},
    {"shift", Series_shift, METH_O,
     "shift(n) -> Series shifted by n (vacated rows null)."},
    {"top_k", DFTU_PYCFUNCTION((core::kw_one<"top_k", "k", Series_top_k>)),
     METH_VARARGS | METH_KEYWORDS, "top_k(k) -> the k largest rows Series."},
    {"bottom_k",
     DFTU_PYCFUNCTION((core::kw_one<"bottom_k", "k", Series_bottom_k>)),
     METH_VARARGS | METH_KEYWORDS,
     "bottom_k(k) -> the k smallest rows Series."},
    {"sample", DFTU_PYCFUNCTION(Series_sample), METH_VARARGS | METH_KEYWORDS,
     "sample(n, seed=0) -> deterministic sample of n rows Series."},
    {"rank", DFTU_PYCFUNCTION(Series_rank), METH_VARARGS | METH_KEYWORDS,
     "rank(method='average', descending=False) -> Float64 Series "
     "(method: average|min|dense|ordinal)."},
    {"rolling", DFTU_PYCFUNCTION(Series_rolling), METH_VARARGS | METH_KEYWORDS,
     "rolling(window, op=None) -> the Rolling window of window rows, or with "
     "op (sum|mean|min|max) the Float64 Series of that reduction."},
    {"rolling_sum",
     DFTU_PYCFUNCTION(
         (core::kw_one<"rolling_sum", "window", Series_rolling_sum>)),
     METH_VARARGS | METH_KEYWORDS,
     "rolling_sum(window) -> Float64 Series of per-window sums."},
    {"rolling_mean",
     DFTU_PYCFUNCTION(
         (core::kw_one<"rolling_mean", "window", Series_rolling_mean>)),
     METH_VARARGS | METH_KEYWORDS,
     "rolling_mean(window) -> Float64 Series of per-window means."},
    {"rolling_min",
     DFTU_PYCFUNCTION(
         (core::kw_one<"rolling_min", "window", Series_rolling_min>)),
     METH_VARARGS | METH_KEYWORDS,
     "rolling_min(window) -> Float64 Series of per-window minima."},
    {"rolling_max",
     DFTU_PYCFUNCTION(
         (core::kw_one<"rolling_max", "window", Series_rolling_max>)),
     METH_VARARGS | METH_KEYWORDS,
     "rolling_max(window) -> Float64 Series of per-window maxima."},
    {"ewm", DFTU_PYCFUNCTION(Series_ewm), METH_VARARGS | METH_KEYWORDS,
     "ewm(alpha=None, *, span=None, com=None, halflife=None) -> the Ewm "
     "window; pass exactly one parameter."},
    {"rolling_quantile",
     DFTU_PYCFUNCTION((core::kw_pair<"rolling_quantile",
                                     Series_rolling_quantile, "window", "q">)),
     METH_VARARGS | METH_KEYWORDS,
     "rolling_quantile(window, q) -> Float64 Series of per-window quantile q."},
    {"ewm_mean",
     DFTU_PYCFUNCTION((core::kw_one<"ewm_mean", "alpha", Series_ewm_mean>)),
     METH_VARARGS | METH_KEYWORDS,
     "ewm_mean(alpha) -> Float64 Series, exponentially weighted mean."},
    {"ewm_std",
     DFTU_PYCFUNCTION((core::kw_one<"ewm_std", "alpha", Series_ewm_std>)),
     METH_VARARGS | METH_KEYWORDS,
     "ewm_std(alpha) -> Float64 Series, exponentially weighted sample std."},
    {"cut", Series_cut, METH_VARARGS,
     "cut(breaks, flags=0) -> Int32 Series binning by the ascending breaks "
     "Series; flags 1 closes each interval on the right, 2 gives null outside "
     "the interior intervals and numbers them from 0."},
    {"qcut", DFTU_PYCFUNCTION((core::kw_one<"qcut", "q", Series_qcut>)),
     METH_VARARGS | METH_KEYWORDS,
     "qcut(q) -> Int32 Series binning by the column's q-quantile edges."},
    {"search_sorted", DFTU_PYCFUNCTION(Series_search_sorted),
     METH_VARARGS | METH_KEYWORDS,
     "search_sorted(element, side='left') -> Int64 Series of the insertion "
     "indices; side 'right' inserts after equal values ('any' is 'left')."},
    {"unique", DFTU_PYCFUNCTION(Series_unique), METH_VARARGS | METH_KEYWORDS,
     "unique(maintain_order=False) -> the distinct values, a null kept once: "
     "sorted with the null first, or in order of first occurrence."},
    {"duplicated", DFTU_PYCFUNCTION(Series_duplicated),
     METH_VARARGS | METH_KEYWORDS,
     "duplicated(keep='first') -> Bool mask of the repeats; keep 'first' or "
     "'last' leaves that row unmarked, False marks every repeated row."},
    {"dot", DFTU_PYCFUNCTION((core::kw_one<"dot", "other", Series_dot>)),
     METH_VARARGS | METH_KEYWORDS,
     "dot(other) -> scalar sum of x[i]*y[i] over non-null pairs (SIMD)."},
    {"sum", series_reduce_scalar<&Series::sum>, METH_NOARGS,
     "Sum of the valid values; a bool column counts True as 1, so (x > "
     "2).sum() counts the matches. Raises TypeError for a column type with "
     "no sum (string, most temporal types)."},
    {"min", series_reduce_scalar<&Series::min>, METH_NOARGS,
     "Smallest valid value, None when no value is valid. A bool column gives "
     "0 or 1 and a string column the bytewise smallest string; raises "
     "TypeError for a type with no order here."},
    {"max", series_reduce_scalar<&Series::max>, METH_NOARGS,
     "Largest valid value, over the same column types as min; None when no "
     "value is valid."},
    {"mean", series_reduce_f64<&Series::mean>, METH_NOARGS,
     "Mean of the valid values, None when no value is valid; a bool column "
     "gives the fraction that is True. Raises TypeError for a column type with "
     "no sum."},
    {"all", Series_all, METH_NOARGS,
     "Whether every non-null Bool value is true."},
    {"any", Series_any, METH_NOARGS,
     "Whether any non-null Bool value is true."},
    {"take", Series_take, METH_O,
     "take(indices) -> Series gathered at the given row indices."},
    {"where", Series_where, METH_VARARGS,
     "where(mask, other) -> Series: this value where the Bool mask is true, "
     "else other's (a null mask row takes other)."},
    {"filter",
     DFTU_PYCFUNCTION((core::kw_one<"filter", "predicate", Series_filter>)),
     METH_VARARGS | METH_KEYWORDS,
     "filter(mask) -> Series keeping rows where the Bool mask is true."},
    {"argsort", DFTU_PYCFUNCTION(Series_argsort), METH_VARARGS | METH_KEYWORDS,
     "argsort(descending=False, nulls_last=True) -> Int64 Series of the "
     "sorted row order; nulls last unless nulls_last is False."},
    {"arg_sort", DFTU_PYCFUNCTION(Series_arg_sort),
     METH_VARARGS | METH_KEYWORDS,
     "arg_sort(descending=False, nulls_last=False) -> Int64 Series of the "
     "sorted row order; nulls first unless nulls_last."},
    {"dictionary_encode", Series_dictionary_encode, METH_NOARGS,
     "Dictionary-encode a String/Binary column -> Series."},
    {"_str_into", Series_str_into, METH_VARARGS,
     "_str_into(out, na=None): a String column's rows into a numpy object "
     "array of its length, null rows holding `na`."},
    {"_np_parts", Series_np_parts, METH_VARARGS,
     "_np_parts(nullable=False, na=None) -> (values, mask, dtype): the column "
     "in one native pass for numpy.frombuffer (bytearrays; a list of str for "
     "a String column, null rows holding `na`); mask is 1 = null or None."},
    {"_to_pylist", Series_to_pylist, METH_NOARGS,
     "_to_pylist() -> list of Python values, None for null; TypeError for a "
     "column type with no native conversion."},
    {"_dictionary_parts", Series_dictionary_parts, METH_NOARGS,
     "_dictionary_parts() -> (codes, values): the FLAT Int32 codes and the "
     "values column of a dictionary-encoded column, sharing its buffers."},
    {"_values_buffer", Series_values_buffer, METH_NOARGS,
     "_values_buffer() -> memoryview: the values of a flat numeric column, "
     "nulls included (unspecified under a null), with no copy."},
    {"_plain_text", Series_plain_text, METH_NOARGS,
     "_plain_text() -> Series: the column with each view string column (at "
     "the top, in the dictionary or in a chunk) made flat; the rest shared."},
    {"_joined_dictionary", Series_joined_dictionary, METH_NOARGS,
     "_joined_dictionary() -> Series or None: a chunked column of dictionary "
     "chunks as one dictionary column; None for any other column."},
    {"_nulls_like", Series_nulls_like, METH_O,
     "_nulls_like(n) -> Series: n null rows of this column's full type."},
    {"_null_bytes", Series_null_bytes, METH_NOARGS,
     "_null_bytes() -> bytearray or None: a byte per row, 1 where the row is "
     "null; None when there are no nulls."},
    {"_physical", Series_physical, METH_NOARGS,
     "_physical() -> Series: the integers of a date, time, timestamp, "
     "duration or float16 column (buffers shared)."},
    {"to_bytes", Series_to_bytes, METH_NOARGS,
     "to_bytes() -> bytes: the column in the native frame format."},
    {"item", Series_item, METH_O,
     "item(i) -> the value at row i as a Python object (None for null)."},
    {"_item_unlabelled", Series_item_unlabelled, METH_O,
     "_item_unlabelled(i) -> the element at position i; NotImplementedError "
     "on a Series with row labels."},
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
    {"index_names", Series_index_names, METH_NOARGS,
     "index_names() -> list[str]\n\nThe row label levels, in order; [] for "
     "a positional Series."},
    {"_index_level", Series_index_level, METH_O,
     "_index_level(i) -> Series: the label column of level i."},
    {"_locate", Series_locate, METH_O,
     "_locate(keys) -> (positions, missing): the rows whose row labels equal "
     "the key Series (one per level), and the keys no label matches."},
    {"_locate_range", Series_locate_range, METH_VARARGS,
     "_locate_range(start, stop) -> (offset, length): the rows between two "
     "label bounds (lists of one-row Series, empty for an open end)."},
    {"_reindex", Series_reindex, METH_VARARGS,
     "_reindex(keys, method, limit, tolerance, fill) -> Series on the label "
     "key "
     "Series (one per level); method 0 none, 1 pad, 2 backfill, 3 nearest, "
     "limit -1 for none, tolerance infinite for none, fill a one-row Series or "
     "None, nanos a tolerance as whole nanoseconds for temporal labels."},
    {"_align", Series_align, METH_VARARGS,
     "_align(other, how) -> (left, right) on the joined labels; how is 0 "
     "inner, "
     "1 left, 2 right, 3 outer."},
    {"_replace", Series_replace, METH_O,
     "_replace(other) -> None: this Series takes the column, name and labels "
     "of `other` in place; BufferError while a buffer export is live."},
    {"_with_index", Series_with_index, METH_VARARGS,
     "_with_index(names, levels) -> Series sharing this column's buffers "
     "whose row labels are the level Series; empty lists drop them."},
    {"slice",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"slice", Series_slice, "offset", "length">)),
     METH_VARARGS | METH_KEYWORDS,
     "slice(offset, length) -> view Series of a row range, sharing the "
     "value buffers and keeping the layout; raises for list, map and struct."},
    {"str_eq", DFTU_PYCFUNCTION((core::kw_one<"str_eq", "rhs", Series_str_eq>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_eq(rhs) -> Bool Series where the string equals rhs."},
    {"str_contains",
     DFTU_PYCFUNCTION(
         (core::kw_one<"str_contains", "needle", Series_str_contains>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_contains(needle) -> Bool Series where the string contains needle."},
    {"str_starts_with",
     DFTU_PYCFUNCTION(
         (core::kw_one<"str_starts_with", "prefix", Series_str_starts_with>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_starts_with(prefix) -> Bool Series where the string starts with "
     "prefix."},
    {"str_ends_with",
     DFTU_PYCFUNCTION(
         (core::kw_one<"str_ends_with", "suffix", Series_str_ends_with>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_ends_with(suffix) -> Bool Series where the string ends with suffix."},
    {"str_like",
     DFTU_PYCFUNCTION((core::kw_one<"str_like", "pattern", Series_str_like>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_like(pattern) -> Bool Series matching the SQL LIKE / glob pattern "
     "(% any run, _ one char, backslash escapes a literal %/_/backslash)."},
    {"str_matches",
     DFTU_PYCFUNCTION(
         (core::kw_one<"str_matches", "pattern", Series_str_matches>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_matches(pattern) -> Bool Series where the whole string matches the "
     "duql regex."},
    {"str_search",
     DFTU_PYCFUNCTION(
         (core::kw_one<"str_search", "pattern", Series_str_search>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_search(pattern) -> Bool Series where the duql regex matches "
     "anywhere in the string."},
    {"str_find",
     DFTU_PYCFUNCTION((core::kw_one<"str_find", "needle", Series_str_find>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_find(needle) -> Int64 Series of the first byte index, or -1."},
    {"str_replace",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"str_replace", Series_str_replace, "pat", "repl">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_replace(pat, repl) -> String Series, first literal occurrence."},
    {"str_replace_all",
     DFTU_PYCFUNCTION((core::kw_pair<"str_replace_all", Series_str_replace_all,
                                     "pat", "repl">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_replace_all(pat, repl) -> String Series, all literal occurrences."},
    {"str_slice",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"str_slice", Series_str_slice, "start", "length">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_slice(start, length=-1) -> String Series substring by byte offset."},
    {"str_pad_start", DFTU_PYCFUNCTION(Series_str_pad_start),
     METH_VARARGS | METH_KEYWORDS,
     "str_pad_start(width, fill=' ') -> left-padded String Series."},
    {"str_pad_end", DFTU_PYCFUNCTION(Series_str_pad_end),
     METH_VARARGS | METH_KEYWORDS,
     "str_pad_end(width, fill=' ') -> right-padded String Series."},
    {"str_zfill",
     DFTU_PYCFUNCTION((core::kw_one<"str_zfill", "width", Series_str_zfill>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_zfill(width) -> String Series left-padded with '0' after any sign."},
    {"str_split",
     DFTU_PYCFUNCTION((core::kw_one<"str_split", "sep", Series_str_split>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_split(sep) -> List<String> Series split on the literal sep."},
    {"str_extract",
     DFTU_PYCFUNCTION((
         core::kw_pair<"str_extract", Series_str_extract, "pattern", "group">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_extract(pattern, group=1) -> String Series of the regex capture "
     "group per row, null where there is no match."},
    {"str_regex_replace",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"str_regex_replace", Series_str_regex_replace,
                        "pattern", "to">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_regex_replace(pattern, to) -> String Series with every regex match "
     "replaced by to ($n, ${n}, ${name} insert a group, $$ a dollar sign). "
     "Raises ValueError for an invalid pattern or replacement."},
    {"list_get",
     DFTU_PYCFUNCTION((core::kw_one<"list_get", "index", Series_list_get>)),
     METH_VARARGS | METH_KEYWORDS,
     "list_get(index) -> the element at index of each row's list (negative "
     "from the end), null where the list is too short."},
    {"list_join",
     DFTU_PYCFUNCTION((core::kw_one<"list_join", "sep", Series_list_join>)),
     METH_VARARGS | METH_KEYWORDS,
     "list_join(sep) -> each row's list of strings joined with sep."},
    {"str_case",
     DFTU_PYCFUNCTION((core::kw_one<"str_case", "code", Series_str_case>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_case(code) -> ASCII case form: 0 capitalize, 1 title, 2 swapcase."},
    {"str_is",
     DFTU_PYCFUNCTION((core::kw_one<"str_is", "code", Series_str_is>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_is(code) -> Bool: 0 alnum, 1 alpha, 2 digit, 3 decimal, 4 numeric, "
     "5 space, 6 lower, 7 upper, 8 title (ASCII; an empty string is false)."},
    {"str_count",
     DFTU_PYCFUNCTION((core::kw_one<"str_count", "pat", Series_str_count>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_count(pat) -> Int64 non-overlapping occurrences of the literal."},
    {"str_rfind",
     DFTU_PYCFUNCTION((core::kw_one<"str_rfind", "needle", Series_str_rfind>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_rfind(needle) -> Int64 byte index of the last occurrence, or -1."},
    {"str_remove_prefix",
     DFTU_PYCFUNCTION((core::kw_one<"str_remove_prefix", "prefix",
                                    Series_str_remove_prefix>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_remove_prefix(prefix) -> each row without a leading prefix."},
    {"str_remove_suffix",
     DFTU_PYCFUNCTION((core::kw_one<"str_remove_suffix", "suffix",
                                    Series_str_remove_suffix>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_remove_suffix(suffix) -> each row without a trailing suffix."},
    {"str_repeat",
     DFTU_PYCFUNCTION((core::kw_one<"str_repeat", "n", Series_str_repeat>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_repeat(n) -> each row repeated n times."},
    {"str_center", DFTU_PYCFUNCTION(Series_str_center),
     METH_VARARGS | METH_KEYWORDS,
     "str_center(width, fill=' ') -> each row centered in width bytes."},
    {"str_cat",
     DFTU_PYCFUNCTION((core::kw_one<"str_cat", "other", Series_str_cat>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_cat(other) -> row-wise concatenation with another String Series."},
    {"str_findall",
     DFTU_PYCFUNCTION(
         (core::kw_one<"str_findall", "pattern", Series_str_findall>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_findall(pattern) -> List<String> of every regex match per row."},
    {"str_partition", DFTU_PYCFUNCTION(Series_str_partition),
     METH_VARARGS | METH_KEYWORDS,
     "str_partition(sep, from_right=False) -> List<String> of (head, sep, "
     "tail) per row at the first (last) occurrence."},
    {"str_normalize",
     DFTU_PYCFUNCTION(
         (core::kw_one<"str_normalize", "form", Series_str_normalize>)),
     METH_VARARGS | METH_KEYWORDS,
     "str_normalize(form) -> each row in the Unicode form (0 NFC, 1 NFKC, "
     "2 NFD, 3 NFKD); null for a row that is not valid UTF-8."},
    {"str_translate",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"str_translate", Series_str_translate, "from_", "to">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_translate(from, to) -> each codepoint of every row replaced by the "
     "String Series pair: from holds one codepoint per row, to the "
     "replacement (null deletes)."},
    {"str_wrap",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"str_wrap", Series_str_wrap, "width", "flags">)),
     METH_VARARGS | METH_KEYWORDS,
     "str_wrap(width, flags) -> each row as textwrap.wrap lines joined by a "
     "newline; flags is an OR of 1 expand_tabs, 2 replace_whitespace, "
     "4 drop_whitespace, 8 break_long_words, 16 break_on_hyphens."},
    {"dt_part",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"dt_part", Series_dt_part, "code", "unit">)),
     METH_VARARGS | METH_KEYWORDS,
     "dt_part(code, unit=2) -> Int64 calendar part (0 year .. 16 epoch days) "
     "of a Timestamp / Date / Duration column, or an Int64 column read in "
     "unit (0 s, 1 ms, 2 us, 3 ns)."},
    {"dt_format",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"dt_format", Series_dt_format, "fmt", "unit">)),
     METH_VARARGS | METH_KEYWORDS,
     "dt_format(fmt, unit=2) -> String strftime of a Timestamp / Date column, "
     "or an Int64 column read in unit (0 s, 1 ms, 2 us, 3 ns); a Timestamp "
     "uses its own unit. Directives: %Y %y %m %d %H %I %M %S %f %j %a %A %b "
     "%B %p %F %T %s %z %Z %%. Raises ValueError for another directive."},
    {"dt_round",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"dt_round", Series_dt_round, "every", "mode">)),
     METH_VARARGS | METH_KEYWORDS,
     "dt_round(every, mode) -> each instant rounded to a multiple of every "
     "(the column's unit) by mode 0 floor, 1 ceil, 2 round (half to even)."},
    {"dt_as_unit",
     DFTU_PYCFUNCTION(
         (core::kw_pair<"dt_as_unit", Series_dt_as_unit, "unit", "mode">)),
     METH_VARARGS | METH_KEYWORDS,
     "dt_as_unit(unit, mode=0) -> this Timestamp or Duration column in unit "
     "(0 s, 1 ms, 2 us, 3 ns), the instant kept, a coarser unit floored "
     "(mode 0) or, for a Duration, truncated toward zero as polars (mode 1). "
     "Raises ValueError on overflow."},
    {"with_timezone",
     DFTU_PYCFUNCTION(
         (core::kw_one<"with_timezone", "tz", Series_with_timezone>)),
     METH_VARARGS | METH_KEYWORDS,
     "with_timezone(tz) -> this Timestamp column under zone tz ('' for "
     "naive), the same instants and buffers."},
    {"dt_tz_localize", DFTU_PYCFUNCTION(Series_tz_localize),
     METH_VARARGS | METH_KEYWORDS,
     "dt_tz_localize(tz, ambiguous=0, nonexistent=0, shift=0) -> naive "
     "Timestamp column read as wall times in zone tz, as UTC instants. "
     "ambiguous: 0 raise, 1 earliest, 2 latest, 3 null, 4 infer. nonexistent: "
     "0 raise, 1 shift forward, 2 shift backward, 3 null, 4 shift by `shift` "
     "ticks. A column with a zone and tz '' gives its wall times, naive."},
    {"is_null_at", Series_is_null, METH_O,
     "is_null_at(i) -> whether row i is null."},
    {"num_children", Series_num_children, METH_NOARGS,
     "Child count: 1 for a List, the field count for a Struct, else 0."},
    {"child", DFTU_PYCFUNCTION((core::kw_one<"child", "i", Series_child>)),
     METH_VARARGS | METH_KEYWORDS,
     "child(i) -> child Series (List: 0=values; Struct: field i)."},
#define API_PICK_Series(id, name, args, doc) API_ENTRY(id, name, args, doc)
#define API_PICK_Str(id, name, args, doc)
#define API_PICK_Dt(id, name, args, doc)
#define API_PICK_List(id, name, args, doc)
#define PY_METHOD(cls, name, op, args, result, name_rule, on_expr, kw0, kw1, \
                  err, doc)                                                  \
    API_PICK_##cls(core::api::row_id(#cls, #name), #name, args, doc)
#include <dftracer/utils/python/core/api.def>
    {nullptr, nullptr, 0, nullptr}};

PySequenceMethods Series_as_sequence = {};
PyNumberMethods Series_as_number = {};
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
    {"_decimal", Series_get_decimal, nullptr,
     "(precision, scale) of a Decimal column; (0, 0) for another type.",
     nullptr},
    {"_fixed_size", Series_get_fixed_size, nullptr,
     "Width of a FixedSizeBinary column or items per row of a FixedSizeList; "
     "0 for another type.",
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

bool series_list_from_py(PyObject* seq, const char* what,
                         dstd::Vector<dataframe::Series>& out) {
    PyObject* fast = PySequence_Fast(seq, what);
    if (!fast) return false;
    const Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    out.reserve(static_cast<std::size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        const dataframe::Series* s =
            unwrap_vec_column(PySequence_Fast_GET_ITEM(fast, i));
        if (!s) {
            PyErr_SetString(PyExc_TypeError, what);
            Py_DECREF(fast);
            return false;
        }
        out.push_back(s->share());
    }
    Py_DECREF(fast);
    return true;
}

bool row_indices_from_py(PyObject* obj, const char* what,
                         dataframe::Series& holder,
                         dstd::Vector<std::int64_t>& parsed,
                         dstd::Span<const std::int64_t>& out) {
    const dataframe::Series* column = unwrap_vec_column(obj);
    if (column && column->type() == dataframe::TypeId::Int64 &&
        column->null_count() == 0) {
        holder = column->materialize();
        out = dstd::Span<const std::int64_t>(
            holder.data<std::int64_t>(),
            static_cast<std::size_t>(holder.length()));
        return true;
    }
    if (!core::parse_int_seq(obj, what, parsed)) return false;
    out = dstd::Span<const std::int64_t>(parsed.data(), parsed.size());
    return true;
}

namespace {

PyObject* int_list(dstd::Span<const std::int64_t> v) {
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(v.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < v.size(); ++i) {
        PyObject* n = PyLong_FromLongLong(v[i]);
        if (!n) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), n);
    }
    return list;
}

}  // namespace

PyObject* located_to_py(dstd::Result<dataframe::Located>&& r) {
    if (!r) {
        core::set_py_error(r.error());
        return nullptr;
    }
    PyObject* positions = int_list(r->positions);
    PyObject* missing = positions ? int_list(r->missing) : nullptr;
    if (!missing) {
        Py_XDECREF(positions);
        return nullptr;
    }
    return Py_BuildValue("NN", positions, missing);
}

PyObject* label_span_to_py(dstd::Result<dataframe::LabelSpan>&& r) {
    if (!r) {
        core::set_py_error(r.error());
        return nullptr;
    }
    return Py_BuildValue("LL", static_cast<long long>(r->offset),
                         static_cast<long long>(r->length));
}

int init_series(PyObject* m) {
    using namespace series_detail;
    SeriesType = {};
    SeriesType.ob_base = {PyObject_HEAD_INIT(nullptr) 0};
    SeriesType.tp_name = "dftracer.utils.dftracer_utils_ext.Series";
    SeriesType.tp_basicsize = sizeof(SeriesObject);
    SeriesType.tp_weaklistoffset = WEAKLIST_OFFSET;
    SeriesType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    SeriesType.tp_doc = "Native columnar Series (SIMD ops + Arrow capsule).";
    SeriesType.tp_dealloc = reinterpret_cast<destructor>(Series_dealloc);
    Series_as_sequence.sq_length = Series_length_sq;
    SeriesType.tp_as_sequence = &Series_as_sequence;
    Series_as_number.nb_add = Series_nb_add;
    Series_as_number.nb_subtract = Series_nb_sub;
    Series_as_number.nb_multiply = Series_nb_mul;
    Series_as_number.nb_true_divide = Series_nb_true_divide;
    Series_as_number.nb_floor_divide = Series_nb_floor_divide;
    Series_as_number.nb_remainder = Series_nb_remainder;
    Series_as_number.nb_power = Series_nb_power;
    SeriesType.tp_as_number = &Series_as_number;
    SeriesType.tp_richcompare = Series_richcompare;
    Series_as_buffer.bf_getbuffer = Series_getbuffer;
    Series_as_buffer.bf_releasebuffer = Series_releasebuffer;
    SeriesType.tp_as_buffer = &Series_as_buffer;
    SeriesType.tp_methods = Series_methods;
    SeriesType.tp_getset = Series_getset;
    // Created by the kernels and the _series_from_* functions; the Python
    // subclass builds one from Python data in its own __new__.
    SeriesType.tp_new = nullptr;
    if (register_type(m, &SeriesType, "Series") < 0) return -1;
    ValuesType = {};
    ValuesType.ob_base = {PyObject_HEAD_INIT(nullptr) 0};
    ValuesType.tp_name = "dftracer.utils.dftracer_utils_ext._SeriesValues";
    ValuesType.tp_basicsize = sizeof(ValuesObject);
    ValuesType.tp_flags = Py_TPFLAGS_DEFAULT;
    ValuesType.tp_dealloc = reinterpret_cast<destructor>(Values_dealloc);
    Values_as_buffer.bf_getbuffer = Values_getbuffer;
    ValuesType.tp_as_buffer = &Values_as_buffer;
    if (PyType_Ready(&ValuesType) < 0) return -1;
    if (init_accessors(m) < 0) return -1;
    if (init_windows(m) < 0) return -1;

    static PyMethodDef api_rows_def = {
        "_api_rows",
        [](PyObject*, PyObject*) { return core::api::rows_as_tuple(); },
        METH_NOARGS,
        "_api_rows() -> the rows of api.def: (class, name, op, args, result, "
        "name_rule, on_expr, keywords, doc)."};
    PyObject* api_rows_fn = PyCFunction_NewEx(&api_rows_def, nullptr, nullptr);
    if (!api_rows_fn) return -1;
    if (PyModule_AddObject(m, "_api_rows", api_rows_fn) < 0) {
        Py_DECREF(api_rows_fn);
        return -1;
    }

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

    static PyMethodDef number_rows_def = {
        "_number_rows", number_rows, METH_VARARGS,
        "_number_rows(keys, sorted) -> (ids, first): the number of each row of "
        "the key columns (a null equals a null) and the first row of each "
        "number; by first appearance, or by ascending value with `sorted`."};
    PyObject* number_rows_fn =
        PyCFunction_NewEx(&number_rows_def, nullptr, nullptr);
    if (!number_rows_fn) return -1;
    if (PyModule_AddObject(m, "_number_rows", number_rows_fn) < 0) {
        Py_DECREF(number_rows_fn);
        return -1;
    }

    static PyMethodDef unstack_positions_def = {
        "_unstack_positions", unstack_positions, METH_VARARGS,
        "_unstack_positions(rows, cols, row_count, col_count) -> Series: the "
        "row that fills each cell of an unstack, column by column, -1 for "
        "none; "
        "ValueError when two rows fill one cell."};
    PyObject* unstack_positions_fn =
        PyCFunction_NewEx(&unstack_positions_def, nullptr, nullptr);
    if (!unstack_positions_fn) return -1;
    if (PyModule_AddObject(m, "_unstack_positions", unstack_positions_fn) < 0) {
        Py_DECREF(unstack_positions_fn);
        return -1;
    }

    static PyMethodDef unstack_columns_def = {
        "_unstack_columns", unstack_columns, METH_VARARGS,
        "_unstack_columns(values, positions, row_count) -> list of Series: "
        "the columns of an unstack, null where a cell has no row."};
    PyObject* unstack_columns_fn =
        PyCFunction_NewEx(&unstack_columns_def, nullptr, nullptr);
    if (!unstack_columns_fn) return -1;
    if (PyModule_AddObject(m, "_unstack_columns", unstack_columns_fn) < 0) {
        Py_DECREF(unstack_columns_fn);
        return -1;
    }

    static PyMethodDef tz_path_def = {
        "_tz_set_search_path", tz_set_search_path, METH_O,
        "_tz_set_search_path(paths) -> None: the directories (separated by "
        "':') searched first for time zone files."};
    PyObject* tz_path_fn = PyCFunction_NewEx(&tz_path_def, nullptr, nullptr);
    if (!tz_path_fn) return -1;
    if (PyModule_AddObject(m, "_tz_set_search_path", tz_path_fn) < 0) {
        Py_DECREF(tz_path_fn);
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
    static PyMethodDef list_of_def = {
        "_series_list_of", series_list_of, METH_VARARGS,
        "_series_list_of(values, ends) -> a List Series whose row i is "
        "values[ends[i - 1]:ends[i]] (the first row starts at 0); ends is a "
        "non-decreasing Int64 column."};
    PyObject* list_of_fn = PyCFunction_NewEx(&list_of_def, nullptr, nullptr);
    if (!list_of_fn) return -1;
    if (PyModule_AddObject(m, "_series_list_of", list_of_fn) < 0) {
        Py_DECREF(list_of_fn);
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

    static PyMethodDef from_dictionary_def = {
        "_series_from_dictionary", series_from_dictionary, METH_VARARGS,
        "_series_from_dictionary(codes, values) -> Series: a dictionary column "
        "over a flat Int32 `codes` column (buffer shared; a null or negative "
        "code is a null row) and `values` (shared); ValueError for a code "
        "past the values."};
    PyObject* dictionary_fn =
        PyCFunction_NewEx(&from_dictionary_def, nullptr, nullptr);
    if (!dictionary_fn) return -1;
    if (PyModule_AddObject(m, "_series_from_dictionary", dictionary_fn) < 0) {
        Py_DECREF(dictionary_fn);
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
