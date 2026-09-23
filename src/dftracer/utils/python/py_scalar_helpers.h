#ifndef DFTRACER_UTILS_PYTHON_PY_SCALAR_HELPERS_H
#define DFTRACER_UTILS_PYTHON_PY_SCALAR_HELPERS_H

#include <Python.h>
#include <dftracer/utils/dataframe/abi.h>

// Fill dftu_scalar from a Python number: float -> F64, int -> I64, or U64 for
// an integer above INT64_MAX (so the whole uint64 range stays exact); a str
// -> STR borrowing the object's UTF-8 buffer, valid while the object lives
// (the caller runs the kernel before returning to Python).
inline bool py_to_scalar(PyObject* o, dftu_scalar* s) {
    if (PyUnicode_Check(o)) {
        Py_ssize_t len = 0;
        const char* text = PyUnicode_AsUTF8AndSize(o, &len);
        if (!text) return false;
        s->kind = DFTU_SCALAR_TAG_STR;
        s->value.s = text;
        s->len = static_cast<uint32_t>(len);
        return true;
    }
    if (PyFloat_Check(o)) {
        s->kind = DFTU_SCALAR_TAG_F64;
        s->value.d = PyFloat_AsDouble(o);
        return !PyErr_Occurred();
    }
    long long v = PyLong_AsLongLong(o);
    if (v == -1 && PyErr_Occurred()) {
        PyErr_Clear();  // may be a positive value in (INT64_MAX, UINT64_MAX]
        unsigned long long u = PyLong_AsUnsignedLongLong(o);
        if (u == static_cast<unsigned long long>(-1) && PyErr_Occurred())
            return false;
        s->kind = DFTU_SCALAR_TAG_U64;
        s->value.u = u;
        return true;
    }
    s->kind = DFTU_SCALAR_TAG_I64;
    s->value.i = v;
    return true;
}

#endif  // DFTRACER_UTILS_PYTHON_PY_SCALAR_HELPERS_H
