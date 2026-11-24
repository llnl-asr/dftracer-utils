// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_SCALAR_HELPERS_H
#define DFTRACER_UTILS_PYTHON_CORE_SCALAR_HELPERS_H

#include <Python.h>
#include <dftracer/utils/dataframe/c_api.h>

#include <cstring>

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
        s->len = static_cast<std::uint32_t>(len);
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

// As py_to_scalar, with None as the NULL scalar: the fill value of fill_nan,
// which turns a NaN into a null.
inline bool py_to_fill_scalar(PyObject* o, dftu_scalar* s) {
    if (o != Py_None) return py_to_scalar(o, s);
    *s = dftu_scalar{};
    s->kind = DFTU_SCALAR_TAG_NULL;
    return true;
}

// The scalar operand of the registry op `op`: None is a value only for
// fill_nan.
inline bool py_to_op_scalar(const char* op, PyObject* o, dftu_scalar* s) {
    return std::strcmp(op, "dftu.series.fill_nan") == 0
               ? py_to_fill_scalar(o, s)
               : py_to_scalar(o, s);
}

#endif  // DFTRACER_UTILS_PYTHON_CORE_SCALAR_HELPERS_H
