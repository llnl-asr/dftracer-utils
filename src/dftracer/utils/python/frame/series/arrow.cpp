// SPDX-License-Identifier: MIT
#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/stream.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/frame/series_detail.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <exception>
#include <new>
#include <utility>

namespace dftracer::utils::python::frame::series_detail {

namespace {

void schema_capsule_destructor(PyObject* cap) {
    auto* s =
        static_cast<ArrowSchema*>(PyCapsule_GetPointer(cap, "arrow_schema"));
    if (s) {
        if (s->release) s->release(s);
        dftracer::utils::dstd::UniquePtr<ArrowSchema>(s).reset();
    }
}
void array_capsule_destructor(PyObject* cap) {
    auto* a =
        static_cast<ArrowArray*>(PyCapsule_GetPointer(cap, "arrow_array"));
    if (a) {
        if (a->release) a->release(a);
        dftracer::utils::dstd::UniquePtr<ArrowArray>(a).reset();
    }
}

}  // namespace

PyObject* Series_arrow_c_array(PyObject* self, PyObject* /*args*/) {
    Series* a = as_native<Series>(self);
    if (!a) return nullptr;
    dftracer::utils::dstd::UniquePtr<ArrowSchema> schema;
    dftracer::utils::dstd::UniquePtr<ArrowArray> array;
    schema = dftracer::utils::dstd::make_unique<ArrowSchema>();
    array = dftracer::utils::dstd::make_unique<ArrowArray>();
    auto batch = arrow::to_arrow(*a);
    if (!batch) {
        dftracer::utils::python::core::set_py_error(batch.error());
        return nullptr;
    }
    batch->release_into(schema.get(), array.get());
    PyObject* scap =
        PyCapsule_New(schema.get(), "arrow_schema", schema_capsule_destructor);
    if (!scap) {
        if (schema->release) schema->release(schema.get());
        if (array->release) array->release(array.get());
        return nullptr;
    }
    schema.release();
    PyObject* acap =
        PyCapsule_New(array.get(), "arrow_array", array_capsule_destructor);
    if (!acap) {
        Py_DECREF(scap);  // releases + frees the schema
        if (array->release) array->release(array.get());
        return nullptr;
    }
    array.release();
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
    guard([&] {
        // Batch::adopt takes both structs, so both capsules' destructors become
        // no-ops; the batch releases the schema and the array.
        auto imported =
            arrow::series_from_arrow(arrow::Batch::adopt(schema, array));
        if (!imported) {
            core::set_py_error(imported.error());
            return false;
        }
        col = std::move(*imported);
        return true;
    });
    Py_DECREF(capsules);
    return col;
}

namespace {

// A ChunkedArray has only __arrow_c_stream__: each batch becomes a chunk.
Series import_arrow_stream(PyObject* obj) {
    PyObject* cap = PyObject_CallMethod(obj, "__arrow_c_stream__", nullptr);
    if (!cap) return Series{};
    auto* stream = static_cast<ArrowArrayStream*>(
        PyCapsule_GetPointer(cap, "arrow_array_stream"));
    if (!stream) {
        Py_DECREF(cap);
        return Series{};
    }
    auto moved = dftracer::utils::dstd::make_unique<ArrowArrayStream>(*stream);
    stream->release = nullptr;
    Py_DECREF(cap);
    auto imported = arrow::series_from_arrow_stream(moved.get());
    if (!imported) {
        core::set_py_error(imported.error());
        return Series{};
    }
    return std::move(*imported);
}

}  // namespace

PyObject* vec_from_arrow(PyObject* /*self*/, PyObject* obj) {
    Series col = !PyObject_HasAttrString(obj, "__arrow_c_array__") &&
                         PyObject_HasAttrString(obj, "__arrow_c_stream__")
                     ? import_arrow_stream(obj)
                     : import_arrow_column(obj);
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
        case 'e':
            *out = TypeId::Float16;
            return itemsize == 2;
        case '?':
            *out = TypeId::Bool;
            return itemsize == 1;
        default:
            return false;
    }
}

// What a borrowed numpy column owns: the Py_buffer that keeps the data alive,
// and a copy of the null mask, because the mask the caller built lives on this
// frame's stack and the column only borrows bitmaps.
struct BorrowedColumn {
    Py_buffer* view = nullptr;
    dftracer::utils::dstd::Vector<std::uint8_t> mask;
};

// Drop a borrowed numpy buffer once the column's last reference is gone. Runs
// from Series destruction, which may be off the interpreter thread, so it
// re-acquires the GIL before touching CPython state.
void numpy_buffer_release(void* ctx) {
    dftracer::utils::dstd::UniquePtr<BorrowedColumn> held(
        static_cast<BorrowedColumn*>(ctx));
    if (!held) return;
    dftracer::utils::dstd::UniquePtr<Py_buffer> view(held->view);
    PyGILState_STATE gil = PyGILState_Ensure();
    PyBuffer_Release(view.get());
    PyGILState_Release(gil);
}

}  // namespace

// Import a 1-D C-contiguous numeric numpy array (anything exposing the buffer
// protocol) into a native Series with NO pyarrow. Zero-copy borrow: the
// column's data buffer points straight at the numpy memory and holds the
// Py_buffer alive until the column is freed. On any unsupported case a Python
// error is set and NULL returned, so the Python wrapper falls back to Arrow.
PyObject* vec_from_numpy(PyObject* /*self*/, PyObject* call_args) {
    PyObject* obj = nullptr;
    PyObject* mask_obj = Py_None;
    if (!PyArg_ParseTuple(call_args, "O|O", &obj, &mask_obj)) return nullptr;
    auto view_owner = dftracer::utils::dstd::make_unique<Py_buffer>();
    Py_buffer* view = view_owner.get();
    if (PyObject_GetBuffer(obj, view,
                           PyBUF_FORMAT | PyBUF_ND | PyBUF_STRIDES) != 0) {
        return nullptr;  // no buffer protocol; error already set
    }
    TypeId type = TypeId::Int64;
    const bool contiguous =
        view->strides == nullptr || view->strides[0] == view->itemsize;
    if (view->ndim != 1 ||
        !numpy_format_type(view->format, view->itemsize, &type) ||
        !contiguous) {
        PyBuffer_Release(view);
        PyErr_SetString(
            PyExc_TypeError,
            "_series_from_numpy needs a 1-D C-contiguous fixed-width "
            "numeric array");
        return nullptr;
    }
    const std::int64_t n = view->shape ? view->shape[0] : 0;

    // Null rows: a byte per row, nonzero where null.
    dstd::Vector<std::uint8_t> validity;
    if (mask_obj != Py_None) {
        Py_buffer m{};
        if (PyObject_GetBuffer(mask_obj, &m, PyBUF_SIMPLE) != 0) {
            PyBuffer_Release(view);
            return nullptr;
        }
        if (m.len != n) {
            PyBuffer_Release(&m);
            PyBuffer_Release(view);
            PyErr_SetString(PyExc_ValueError,
                            "mask and array differ in length");
            return nullptr;
        }
        validity.assign(static_cast<std::size_t>((n + 7) / 8), 0);
        const auto* bytes = static_cast<const std::uint8_t*>(m.buf);
        for (std::int64_t i = 0; i < n; ++i)
            if (!bytes[i])
                validity[static_cast<std::size_t>(i >> 3)] |=
                    static_cast<std::uint8_t>(1U << (i & 7));
        PyBuffer_Release(&m);
    }
    const std::uint8_t* valid = validity.empty() ? nullptr : validity.data();

    if (type == TypeId::Bool) {  // one byte per bool -> bit-packed
        dstd::Vector<std::uint8_t> packed(static_cast<std::size_t>((n + 7) / 8),
                                          0);
        const auto* bytes = static_cast<const std::uint8_t*>(view->buf);
        for (std::int64_t i = 0; i < n; ++i)
            if (bytes[i])
                packed[static_cast<std::size_t>(i >> 3)] |=
                    static_cast<std::uint8_t>(1U << (i & 7));
        PyBuffer_Release(view);
        return make_series(Series::flat(TypeId::Bool, packed.data(), n, valid));
    }
    // An empty array has no memory to borrow (the release would not run on a
    // null/zero-length buffer), so copy the (empty) column and free the view.
    if (n == 0 || view->buf == nullptr) {
        Series col = Series::flat(type, view->buf, n, valid);
        PyBuffer_Release(view);
        if (!col.valid()) {
            PyErr_SetString(PyExc_RuntimeError,
                            "failed to build an empty column");
            return nullptr;
        }
        return make_series(std::move(col));
    }
    // The column borrows the data and the bitmap, so the release context keeps
    // both alive: the Py_buffer and a heap copy of the mask, which lives on
    // this frame.
    dftracer::utils::dstd::UniquePtr<BorrowedColumn> held =
        dftracer::utils::dstd::make_unique<BorrowedColumn>();
    held->view = view;
    held->mask = std::move(validity);
    view_owner.release();
    BorrowedColumn* ctx = held.release();
    dftu_series* h = dftu_series_new_flat_borrowed(
        static_cast<dftu_dtype>(type), view->buf, n,
        ctx->mask.empty() ? nullptr : ctx->mask.data(), numpy_buffer_release,
        ctx);
    if (!h) {
        // The constructor already ran the release, which freed `ctx`.
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to borrow the numpy buffer");
        return nullptr;
    }
    return make_series(Series{h});
}

}  // namespace dftracer::utils::python::frame::series_detail
