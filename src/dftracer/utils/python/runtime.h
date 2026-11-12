// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_RUNTIME_H
#define DFTRACER_UTILS_PYTHON_RUNTIME_H

#include <Python.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/runtime/runtime.h>

typedef struct {
    PyObject_HEAD dftracer::utils::dstd::SharedPtr<dftracer::utils::Runtime>
        runtime;
} RuntimeObject;

extern PyTypeObject RuntimeType;

namespace dftracer::utils::python {

int init_runtime(PyObject *m);

// Default global runtime (lazy-initialized, never NULL after first call)
dftracer::utils::Runtime *get_default_runtime();

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_RUNTIME_H
