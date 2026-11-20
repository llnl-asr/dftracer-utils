// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_TYPE_HELPERS_H
#define DFTRACER_UTILS_PYTHON_CORE_TYPE_HELPERS_H

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

// The offset of the weak reference list of a native type: the first member
// after PyObject_HEAD. A type that sets tp_weaklistoffset to it clears the list
// in its dealloc.
inline constexpr Py_ssize_t WEAKLIST_OFFSET = sizeof(PyObject);

// A native type that Python subclasses. `base` is the native type object and
// `*slot` the type its factories allocate: `base` until the Python subclass is
// bound, then that subclass, so a native method returns the public class.
struct TypeBinding {
    const char *name;
    PyTypeObject *base;
    PyTypeObject **slot;
};

// Whether `sub` is a class derived from `binding.base`. Sets a TypeError when
// it is not.
inline bool check_binding(const TypeBinding &binding, PyObject *sub) {
    if (PyType_Check(sub) &&
        PyType_IsSubtype(reinterpret_cast<PyTypeObject *>(sub), binding.base))
        return true;
    PyErr_Format(PyExc_TypeError, "%s must be a subclass of %s", binding.name,
                 binding.base->tp_name);
    return false;
}

// Make the factories of `binding` allocate `sub`, which check_binding accepted.
// Holds a reference to the bound class.
inline void bind_type(const TypeBinding &binding, PyObject *sub) {
    PyTypeObject *bound = reinterpret_cast<PyTypeObject *>(sub);
    Py_INCREF(bound);
    PyTypeObject *old = *binding.slot;
    *binding.slot = bound;
    if (old != binding.base) Py_DECREF(old);
}

#endif  // DFTRACER_UTILS_PYTHON_CORE_TYPE_HELPERS_H
