#ifndef DFTRACER_UTILS_PYTHON_RUNTIME_H
#define DFTRACER_UTILS_PYTHON_RUNTIME_H

#include <Python.h>
#include <dftracer/utils/runtime/runtime.h>

#include <memory>

typedef struct {
    PyObject_HEAD std::shared_ptr<dftracer::utils::Runtime> runtime;
} RuntimeObject;

extern PyTypeObject RuntimeType;

namespace dftracer::utils::python {

int init_runtime(PyObject *m);

// Default global runtime (lazy-initialized, never NULL after first call)
dftracer::utils::Runtime *get_default_runtime();

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_RUNTIME_H
