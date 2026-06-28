#ifndef DFTRACER_UTILS_PYTHON_PY_TYPE_HELPERS_H
#define DFTRACER_UTILS_PYTHON_PY_TYPE_HELPERS_H

#include <Python.h>

// Ready a type and add it to the module under `name`. Returns 0 on success,
// -1 on failure with the Python error set.
//
// Note: this deliberately does NOT decref the module on failure. PyInit_*
// returns NULL without owning a balancing reference to the module, so an extra
// Py_DECREF(m) here would be an over-release bug.
inline int register_type(PyObject *m, PyTypeObject *type, const char *name) {
    if (PyType_Ready(type) < 0) return -1;
    Py_INCREF(type);
    if (PyModule_AddObject(m, name, reinterpret_cast<PyObject *>(type)) < 0) {
        Py_DECREF(type);
        return -1;
    }
    return 0;
}

#endif  // DFTRACER_UTILS_PYTHON_PY_TYPE_HELPERS_H
