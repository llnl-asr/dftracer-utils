#ifndef DFTRACER_UTILS_PYTHON_PY_RUNTIME_MIXIN_H
#define DFTRACER_UTILS_PYTHON_PY_RUNTIME_MIXIN_H

#include <Python.h>
#include <dftracer/utils/python/runtime.h>

#include <string>

// Shared implementation for utility objects whose layout is exactly:
//     typedef struct { PyObject_HEAD PyObject *runtime_obj; } XObject;
// Each binding kept re-rolling an identical runtime-resolution / tp_new /
// tp_dealloc / tp_init quartet; these templates single-source it.

// Resolve the backing Runtime: the explicitly-bound one, else the default.
template <typename T>
dftracer::utils::Runtime *resolve_runtime(T *self) {
    if (self->runtime_obj)
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    return get_default_runtime();
}

template <typename T>
PyObject *runtime_backed_new(PyTypeObject *type, PyObject *, PyObject *) {
    T *self = (T *)type->tp_alloc(type, 0);
    if (self) self->runtime_obj = NULL;
    return (PyObject *)self;
}

template <typename T>
void runtime_backed_dealloc(T *self) {
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

// Parse an optional `runtime=` kwarg (a Runtime instance, an object exposing a
// `_native` Runtime, or None) and bind it into self->runtime_obj.
template <typename T>
int runtime_backed_init(T *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"runtime", NULL};
    PyObject *runtime_arg = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", (char **)kwlist,
                                     &runtime_arg)) {
        return -1;
    }
    if (runtime_arg && runtime_arg != Py_None) {
        if (PyObject_TypeCheck(runtime_arg, &RuntimeType)) {
            Py_INCREF(runtime_arg);
            self->runtime_obj = runtime_arg;
        } else {
            PyObject *native = PyObject_GetAttrString(runtime_arg, "_native");
            if (native && PyObject_TypeCheck(native, &RuntimeType)) {
                self->runtime_obj = native;
            } else {
                Py_XDECREF(native);
                PyErr_SetString(PyExc_TypeError,
                                "runtime must be a Runtime instance or None");
                return -1;
            }
        }
    }
    return 0;
}

// Run a blocking C++ body with the GIL released, translating any C++ exception
// into a Python RuntimeError. Returns true on success; on failure the Python
// error is set and the caller should return its error sentinel (NULL or -1).
// The body must not touch Python objects (the GIL is not held while it runs).
template <typename F>
bool run_blocking(F &&body) {
    std::string error_msg;
    Py_BEGIN_ALLOW_THREADS try { body(); } catch (const std::exception &e) {
        error_msg = e.what();
    } catch (...) {
        error_msg = "unknown C++ exception";
    }
    Py_END_ALLOW_THREADS if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return false;
    }
    return true;
}

#endif  // DFTRACER_UTILS_PYTHON_PY_RUNTIME_MIXIN_H
