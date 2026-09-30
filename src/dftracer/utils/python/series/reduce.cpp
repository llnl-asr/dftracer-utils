#include <dftracer/utils/python/series_detail.h>

#include <cstdint>

namespace dftracer::utils::python::series_detail {

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
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyBool_FromLong(a->all());
}
PyObject* Series_any(PyObject* self, PyObject*) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    return PyBool_FromLong(a->any());
}
PyObject* Series_dot(PyObject* self, PyObject* other) {
    Series* a = as_series(self);
    if (!a) return nullptr;
    Series* b = as_series(other);
    if (!b) return nullptr;
    return scalar_to_py(dftu_series_dot(a->handle(), b->handle()));
}

}  // namespace dftracer::utils::python::series_detail
