// SPDX-License-Identifier: MIT
#include <dftracer/utils/python/frame/series_detail.h>

#include <cstdint>

namespace dftracer::utils::python::frame::series_detail {

// A tagged scalar as the matching Python value: a number, a str for a STR
// scalar, or a TypeError carrying the message of an ERR scalar (a reduction the
// engine refuses).
PyObject* scalar_to_py(dftu_scalar v) {
    switch (v.kind) {
        case DFTU_SCALAR_TAG_I64:
            return PyLong_FromLongLong(v.value.i);
        case DFTU_SCALAR_TAG_U64:
            return PyLong_FromUnsignedLongLong(v.value.u);
        case DFTU_SCALAR_TAG_F64:
            return PyFloat_FromDouble(v.value.d);
        case DFTU_SCALAR_TAG_STR:
            return PyUnicode_DecodeUTF8(v.value.s ? v.value.s : "",
                                        static_cast<Py_ssize_t>(v.len),
                                        "strict");
        case DFTU_SCALAR_TAG_NULL:
            Py_RETURN_NONE;
        case DFTU_SCALAR_TAG_ERR:
            PyErr_SetString(PyExc_TypeError,
                            v.value.err != nullptr && v.value.err->message
                                ? v.value.err->message
                                : "reduction failed");
            return nullptr;
    }
    PyErr_SetString(PyExc_RuntimeError, "unknown scalar tag");
    return nullptr;
}

PyObject* Series_all(PyObject* self, PyObject*) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    return PyBool_FromLong(without_gil(a->length(), [&] { return a->all(); }));
}
PyObject* Series_any(PyObject* self, PyObject*) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    return PyBool_FromLong(without_gil(a->length(), [&] { return a->any(); }));
}
PyObject* Series_dot(PyObject* self, PyObject* other) {
    Held held_a(self);
    Series* a = held_a.get();
    if (!a) return nullptr;
    Held held_b(other);
    Series* b = held_b.get();
    if (!b) return nullptr;
    return scalar_to_py(without_gil(a->length(), [&] {
        return dftu_series_dot(a->handle(), b->handle());
    }));
}

}  // namespace dftracer::utils::python::frame::series_detail
