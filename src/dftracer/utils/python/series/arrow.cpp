#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/dataframe/arrow_bridge.h>
#include <dftracer/utils/python/series_detail.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <exception>
#include <new>
#include <utility>

namespace dftracer::utils::python::series_detail {

namespace {

void schema_capsule_destructor(PyObject* cap) {
    auto* s =
        static_cast<ArrowSchema*>(PyCapsule_GetPointer(cap, "arrow_schema"));
    if (s) {
        if (s->release) s->release(s);
        delete s;
    }
}
void array_capsule_destructor(PyObject* cap) {
    auto* a =
        static_cast<ArrowArray*>(PyCapsule_GetPointer(cap, "arrow_array"));
    if (a) {
        if (a->release) a->release(a);
        delete a;
    }
}

}  // namespace

PyObject* Series_arrow_c_array(PyObject* self, PyObject* /*args*/) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    auto* schema = new (std::nothrow) ArrowSchema{};
    auto* array = new (std::nothrow) ArrowArray{};
    if (!schema || !array) {
        delete schema;
        delete array;
        return PyErr_NoMemory();
    }
    try {
        dataframe::to_arrow(*a, schema, array);
    } catch (const std::exception& e) {
        delete schema;
        delete array;
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }
    PyObject* scap =
        PyCapsule_New(schema, "arrow_schema", schema_capsule_destructor);
    if (!scap) {
        if (schema->release) schema->release(schema);
        delete schema;
        if (array->release) array->release(array);
        delete array;
        return nullptr;
    }
    PyObject* acap =
        PyCapsule_New(array, "arrow_array", array_capsule_destructor);
    if (!acap) {
        Py_DECREF(scap);  // releases + frees the schema
        if (array->release) array->release(array);
        delete array;
        return nullptr;
    }
    PyObject* result = PyTuple_Pack(2, scap, acap);
    Py_DECREF(scap);
    Py_DECREF(acap);
    return result;
}

// Import one Arrow array (any object with __arrow_c_array__) into a
// dataframe::Series. Returns an invalid Series with a Python error set on
// failure.
Series import_arrow_column(PyObject* obj) {
    PyObject* capsules = PyObject_CallMethod(obj, "__arrow_c_array__", nullptr);
    if (!capsules) return Series{};
    if (!PyTuple_Check(capsules) || PyTuple_GET_SIZE(capsules) != 2) {
        Py_DECREF(capsules);
        PyErr_SetString(PyExc_TypeError,
                        "__arrow_c_array__ must return (schema, array)");
        return Series{};
    }
    auto* schema = static_cast<ArrowSchema*>(
        PyCapsule_GetPointer(PyTuple_GET_ITEM(capsules, 0), "arrow_schema"));
    auto* array = static_cast<ArrowArray*>(
        PyCapsule_GetPointer(PyTuple_GET_ITEM(capsules, 1), "arrow_array"));
    if (!schema || !array) {
        Py_DECREF(capsules);
        return Series{};
    }
    Series col;
    try {
        // from_arrow adopts `array` (nulls its release), so the array capsule's
        // destructor becomes a no-op; the schema capsule still owns the schema.
        col = dataframe::from_arrow(schema, array);
        if (!col.valid())
            PyErr_SetString(PyExc_ValueError, "unsupported Arrow column");
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
        col = Series{};
    }
    Py_DECREF(capsules);
    return col;
}

PyObject* vec_from_arrow(PyObject* /*self*/, PyObject* obj) {
    Series col = import_arrow_column(obj);
    if (!col.valid()) return nullptr;
    return make_series(std::move(col));
}

namespace {

// Map a native-byte-order buffer-protocol format code to a fixed-width TypeId.
// Rejects explicit non-native byte order (we borrow the raw bytes) and any
// non-numeric or composite format. `itemsize` disambiguates 'l'/'L' (int vs
// long) across platforms.
bool numpy_format_type(const char* fmt, Py_ssize_t itemsize, TypeId* out) {
    if (!fmt || !*fmt) return false;
    char order = *fmt;
    if (order == '@' || order == '=' || order == '<' || order == '>' ||
        order == '!') {
        if (order != '@' && order != '=') return false;
        ++fmt;
    }
    if (fmt[0] == '\0' || fmt[1] != '\0') return false;  // one type code only
    switch (fmt[0]) {
        case 'b':
            *out = TypeId::Int8;
            return itemsize == 1;
        case 'B':
            *out = TypeId::Uint8;
            return itemsize == 1;
        case 'h':
            *out = TypeId::Int16;
            return itemsize == 2;
        case 'H':
            *out = TypeId::Uint16;
            return itemsize == 2;
        case 'i':
            *out = TypeId::Int32;
            return itemsize == 4;
        case 'I':
            *out = TypeId::Uint32;
            return itemsize == 4;
        case 'l':
            *out = itemsize == 8 ? TypeId::Int64 : TypeId::Int32;
            return itemsize == 8 || itemsize == 4;
        case 'L':
            *out = itemsize == 8 ? TypeId::Uint64 : TypeId::Uint32;
            return itemsize == 8 || itemsize == 4;
        case 'q':
            *out = TypeId::Int64;
            return itemsize == 8;
        case 'Q':
            *out = TypeId::Uint64;
            return itemsize == 8;
        case 'f':
            *out = TypeId::Float32;
            return itemsize == 4;
        case 'd':
            *out = TypeId::Float64;
            return itemsize == 8;
        default:
            return false;
    }
}

// Drop a borrowed numpy buffer once the column's last reference is gone. Runs
// from Series destruction, which may be off the interpreter thread, so it
// re-acquires the GIL before touching CPython state.
void numpy_buffer_release(void* ctx) {
    auto* view = static_cast<Py_buffer*>(ctx);
    PyGILState_STATE gil = PyGILState_Ensure();
    PyBuffer_Release(view);
    PyGILState_Release(gil);
    delete view;
}

}  // namespace

// Import a 1-D C-contiguous numeric numpy array (anything exposing the buffer
// protocol) into a native Series with NO pyarrow. Zero-copy borrow: the
// column's data buffer points straight at the numpy memory and holds the
// Py_buffer alive until the column is freed. On any unsupported case a Python
// error is set and NULL returned, so the Python wrapper falls back to Arrow.
PyObject* vec_from_numpy(PyObject* /*self*/, PyObject* obj) {
    auto* view = new (std::nothrow) Py_buffer{};
    if (!view) return PyErr_NoMemory();
    if (PyObject_GetBuffer(obj, view,
                           PyBUF_FORMAT | PyBUF_ND | PyBUF_STRIDES) != 0) {
        delete view;
        return nullptr;  // no buffer protocol; error already set
    }
    TypeId type = TypeId::Int64;
    const bool contiguous =
        view->strides == nullptr || view->strides[0] == view->itemsize;
    if (view->ndim != 1 ||
        !numpy_format_type(view->format, view->itemsize, &type) ||
        !contiguous) {
        PyBuffer_Release(view);
        delete view;
        PyErr_SetString(
            PyExc_TypeError,
            "_series_from_numpy needs a 1-D C-contiguous fixed-width "
            "numeric array");
        return nullptr;
    }
    std::int64_t n = view->shape ? view->shape[0] : 0;
    // An empty array has no memory to borrow (the release would not run on a
    // null/zero-length buffer), so copy the (empty) column and free the view.
    if (n == 0 || view->buf == nullptr) {
        Series col = Series::flat(type, view->buf, n, nullptr);
        PyBuffer_Release(view);
        delete view;
        if (!col.valid()) {
            PyErr_SetString(PyExc_RuntimeError,
                            "failed to build an empty column");
            return nullptr;
        }
        return make_series(std::move(col));
    }
    dftu_series* h =
        dftu_series_new_flat_borrowed(static_cast<dftu_dtype>(type), view->buf,
                                      n, nullptr, numpy_buffer_release, view);
    if (!h) {
        numpy_buffer_release(view);  // releases the buffer and deletes the view
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to borrow the numpy buffer");
        return nullptr;
    }
    return make_series(Series{h});
}

}  // namespace dftracer::utils::python::series_detail

#endif  // DFTRACER_UTILS_ENABLE_ARROW
