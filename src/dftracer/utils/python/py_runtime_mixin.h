#ifndef DFTRACER_UTILS_PYTHON_PY_RUNTIME_MIXIN_H
#define DFTRACER_UTILS_PYTHON_PY_RUNTIME_MIXIN_H

#include <Python.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/runtime.h>

#include <exception>
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
    return dftracer::utils::python::get_default_runtime();
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

// Parse an optional `runtime=` kwarg and bind it into self->runtime_obj.
template <typename T>
int runtime_backed_init(T *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"runtime", NULL};
    PyObject *runtime_arg = NULL;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|O", const_cast<char **>(kwlist), &runtime_arg)) {
        return -1;
    }
    return bind_runtime_arg(self, runtime_arg);
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
        exc_type = dftracer::utils::python::py_error_type_for(e);
    } catch (const std::invalid_argument &e) {
        failed = true;
        error_msg = e.what();
        exc_type = dftracer::utils::python::g_dft_value_error;
    } catch (const std::exception &e) {
        failed = true;
        error_msg = e.what();
    } catch (...) {
        failed = true;
        error_msg = "unknown C++ exception";
    }
    Py_END_ALLOW_THREADS if (failed) {
        if (exc_type == nullptr)
            exc_type = dftracer::utils::python::g_dft_error;
        PyErr_SetString(exc_type ? exc_type : PyExc_RuntimeError,
                        error_msg.c_str());
        return false;
    }
    return true;
}

// Like run_blocking, but for a body that produces a value. Runs `body` with
// the GIL released, assigning its result into `out`. On a thrown exception the
// exception is captured off-GIL, the GIL is re-acquired, the typed Python error
// is raised, and false is returned. `out` is only modified on success.
template <typename F, typename T>
bool run_blocking_r(F &&body, T &out) {
    std::exception_ptr eptr;
    Py_BEGIN_ALLOW_THREADS try { out = body(); } catch (...) {
        eptr = std::current_exception();
    }
    Py_END_ALLOW_THREADS if (eptr) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception &e) {
            dftracer::utils::python::set_typed_py_error(e);
        } catch (...) {
            PyErr_SetString(dftracer::utils::python::g_dft_error
                                ? dftracer::utils::python::g_dft_error
                                : PyExc_RuntimeError,
                            "unknown C++ exception");
        }
        return false;
    }
    return true;
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

#endif  // DFTRACER_UTILS_PYTHON_PY_RUNTIME_MIXIN_H
