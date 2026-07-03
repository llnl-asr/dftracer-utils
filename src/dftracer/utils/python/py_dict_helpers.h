#ifndef DFTRACER_UTILS_PYTHON_PY_DICT_HELPERS_H
#define DFTRACER_UTILS_PYTHON_PY_DICT_HELPERS_H

#include <Python.h>

#include <cstddef>

// Set dict[key] = value and release the caller's reference to value.
// PyDict_SetItemString INCREFs but does not steal, so passing a freshly
// created object inline leaks one reference. NULL-safe.
// Returns 0 on success, -1 on failure (with the Python error set).
inline int dict_set_steal(PyObject *d, const char *key, PyObject *value) {
    if (!value) return -1;
    int rc = PyDict_SetItemString(d, key, value);
    Py_DECREF(value);
    return rc;
}

// Typed convenience setters. Each builds the Python object, stores it, and
// releases the temporary reference. Return 0 on success, -1 on failure.
inline int dict_set_str(PyObject *d, const char *key, const char *value) {
    return dict_set_steal(d, key, PyUnicode_FromString(value));
}

inline int dict_set_f64(PyObject *d, const char *key, double value) {
    return dict_set_steal(d, key, PyFloat_FromDouble(value));
}

inline int dict_set_size(PyObject *d, const char *key, std::size_t value) {
    return dict_set_steal(d, key, PyLong_FromSize_t(value));
}

inline int dict_set_u64(PyObject *d, const char *key,
                        unsigned long long value) {
    return dict_set_steal(d, key, PyLong_FromUnsignedLongLong(value));
}

inline int dict_set_i64(PyObject *d, const char *key, long long value) {
    return dict_set_steal(d, key, PyLong_FromLongLong(value));
}

inline int dict_set_bool(PyObject *d, const char *key, bool value) {
    return dict_set_steal(d, key, PyBool_FromLong(value ? 1 : 0));
}

#endif
