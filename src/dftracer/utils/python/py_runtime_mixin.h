#ifndef DFTRACER_UTILS_PYTHON_PY_RUNTIME_MIXIN_H
#define DFTRACER_UTILS_PYTHON_PY_RUNTIME_MIXIN_H

#include <Python.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/runtime.h>

#include <stdexcept>
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

// Run a blocking C++ body with the GIL released
template <typename F>
bool run_blocking(F &&body) {
    bool failed = false;
    std::string error_msg;
    PyObject *exc_type = nullptr;  // pointer read only; no Python API off-GIL
    Py_BEGIN_ALLOW_THREADS try {
        body();
    } catch (const dftracer::utils::DFTUtilsException &e) {
        failed = true;
        error_msg = e.what();
        exc_type = py_error_type_for(e.code());
    } catch (const std::invalid_argument &e) {
        failed = true;
        error_msg = e.what();
        exc_type = g_dft_value_error;
    } catch (const std::exception &e) {
        failed = true;
        error_msg = e.what();
    } catch (...) {
        failed = true;
        error_msg = "unknown C++ exception";
    }
    Py_END_ALLOW_THREADS if (failed) {
        if (exc_type == nullptr) exc_type = g_dft_error;
        PyErr_SetString(exc_type ? exc_type : PyExc_RuntimeError,
                        error_msg.c_str());
        return false;
    }
    return true;
}

#endif  // DFTRACER_UTILS_PYTHON_PY_RUNTIME_MIXIN_H
