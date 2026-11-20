// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_RUNTIME_MIXIN_H
#define DFTRACER_UTILS_PYTHON_CORE_RUNTIME_MIXIN_H

#include <Python.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/python/core/call.h>
#include <dftracer/utils/python/core/errors.h>
#include <dftracer/utils/python/runtime/runtime.h>

#include <exception>
#include <stdexcept>

// Shared implementation for utility objects whose layout is exactly:
//     typedef struct { PyObject_HEAD PyObject *runtime_obj; } XObject;
// Each binding kept re-rolling an identical runtime-resolution / tp_new /
// tp_dealloc / tp_init quartet; these templates single-source it.

// Resolve the backing Runtime: the explicitly-bound one, else the default.
template <typename T>
dftracer::utils::Runtime *resolve_runtime(T *self) {
    if (self->runtime_obj)
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    return dftracer::utils::python::runtime::get_default_runtime();
}

// The same Runtime as an owning pointer, for a task that must outlive the call.
template <typename T>
dftracer::utils::dstd::SharedPtr<dftracer::utils::Runtime> shared_runtime(
    T *self) {
    if (self->runtime_obj) return ((RuntimeObject *)self->runtime_obj)->runtime;
    return dftracer::utils::dstd::SharedPtr<dftracer::utils::Runtime>(
        dftracer::utils::dstd::SharedPtr<void>(),
        dftracer::utils::python::runtime::get_default_runtime());
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

// Bind a `runtime` argument (a Runtime instance, an object exposing a
// `_native` Runtime, or None/NULL) into self->runtime_obj.
template <typename T>
int bind_runtime_arg(T *self, PyObject *runtime_arg) {
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

// The Runtime a `runtime=` argument names: a Runtime, an object exposing a
// `_native` Runtime, or None/NULL for the default. Null with a Python error set
// on any other argument.
inline dftracer::utils::dstd::SharedPtr<dftracer::utils::Runtime>
runtime_from_arg(PyObject *runtime_arg) {
    if (!runtime_arg || runtime_arg == Py_None)
        return dftracer::utils::dstd::SharedPtr<dftracer::utils::Runtime>(
            dftracer::utils::dstd::SharedPtr<void>(),
            dftracer::utils::python::runtime::get_default_runtime());
    if (PyObject_TypeCheck(runtime_arg, &RuntimeType))
        return ((RuntimeObject *)runtime_arg)->runtime;
    PyObject *native = PyObject_GetAttrString(runtime_arg, "_native");
    if (native && PyObject_TypeCheck(native, &RuntimeType)) {
        dftracer::utils::dstd::SharedPtr<dftracer::utils::Runtime> rt =
            ((RuntimeObject *)native)->runtime;
        Py_DECREF(native);
        return rt;
    }
    Py_XDECREF(native);
    PyErr_Clear();
    PyErr_SetString(PyExc_TypeError,
                    "runtime must be a Runtime instance or None");
    return nullptr;
}

// Parse an optional `runtime=` kwarg and bind it into self->runtime_obj.
template <typename T>
int runtime_backed_init(T *self, PyObject *args, PyObject *kwds) {
    PyObject *runtime_arg = NULL;
    if (!dftracer::utils::python::core::parse_kw<"|O">(args, kwds, {"runtime"},
                                                       &runtime_arg)) {
        return -1;
    }
    return bind_runtime_arg(self, runtime_arg);
}

// Like run_blocking, but for a body that produces a value. Runs `body` with
// the GIL released, assigning its result into `out`. On a thrown exception the
// GIL is re-acquired, the typed Python error is raised, and false is returned.
// `out` is only modified on success.
template <typename F, typename T>
bool run_blocking_r(F &&body, T &out) {
    return dftracer::utils::python::core::guard([&] {
        out = dftracer::utils::python::core::release_gil(body);
        return true;
    });
}

// Run a blocking C++ body with the GIL released. A thrown exception becomes a
// typed Python error and the result is false.
template <typename F>
bool run_blocking(F &&body) {
    int unused = 0;
    return run_blocking_r(
        [&body] {
            body();
            return 0;
        },
        unused);
}

// Generate the tp_dealloc / tp_new / tp_init shims for a utility object whose
// layout is `struct { PyObject_HEAD PyObject *runtime_obj; }`. PREFIX is the
// type's function-name prefix (e.g. Aggregator); OBJ is its object struct.
#define DFTRACER_UTILS_RUNTIME_BACKED_SLOTS(PREFIX, OBJ)                      \
    static void PREFIX##_dealloc(OBJ *self) { runtime_backed_dealloc(self); } \
    static PyObject *PREFIX##_new(PyTypeObject *type, PyObject *args,         \
                                  PyObject *kwds) {                           \
        return runtime_backed_new<OBJ>(type, args, kwds);                     \
    }                                                                         \
    static int PREFIX##_init(OBJ *self, PyObject *args, PyObject *kwds) {     \
        return runtime_backed_init(self, args, kwds);                         \
    }

#endif  // DFTRACER_UTILS_PYTHON_CORE_RUNTIME_MIXIN_H
