#ifndef DFTRACER_UTILS_PYTHON_PY_DICT_HELPERS_H
#define DFTRACER_UTILS_PYTHON_PY_DICT_HELPERS_H

#include <Python.h>

// Set dict[key] = value and release the caller's reference to value.
// PyDict_SetItemString INCREFs but does not steal, so passing a freshly
// created object inline leaks one reference. NULL-safe.
inline int dict_set_steal(PyObject *d, const char *key, PyObject *value) {
    if (!value) return -1;
    int rc = PyDict_SetItemString(d, key, value);
    Py_DECREF(value);
    return rc;
}

#endif
