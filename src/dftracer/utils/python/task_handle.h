// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_TASK_HANDLE_H
#define DFTRACER_UTILS_PYTHON_TASK_HANDLE_H

#include <Python.h>
#include <dftracer/utils/dstd/any.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/task_handle.h>

typedef struct {
    PyObject_HEAD

        // The underlying shared_future<void> for waiting
        dftracer::utils::dstd::SharedFuture<void>
            future;

    // Typed result via type erasure. Empty for void tasks.
    dftracer::utils::dstd::SharedFuture<dftracer::utils::dstd::Any>
        typed_future;
    bool has_typed_future;

    dftracer::utils::dstd::String name;
    dftracer::utils::common::TaskIndex task_id;
} TaskHandleObject;

extern PyTypeObject TaskHandleType;

namespace dftracer::utils::python {

int init_task_handle(PyObject *m);

}  // namespace dftracer::utils::python

// Create a TaskHandle Python object for a typed task (result stored as
#endif  // DFTRACER_UTILS_PYTHON_TASK_HANDLE_H
