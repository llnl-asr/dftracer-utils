#ifndef DFTRACER_UTILS_PYTHON_TASK_HANDLE_H
#define DFTRACER_UTILS_PYTHON_TASK_HANDLE_H

#include <Python.h>
#include <dftracer/utils/runtime/task_handle.h>

#include <any>
#include <future>
#include <string>

typedef struct {
    PyObject_HEAD

        // The underlying shared_future<void> for waiting
        std::shared_future<void>
            future;

    // Typed result via type erasure. Empty for void tasks.
    std::shared_future<std::any> typed_future;
    bool has_typed_future;

    std::string name;
    dftracer::utils::TaskIndex task_id;
} TaskHandleObject;

extern PyTypeObject TaskHandleType;

namespace dftracer::utils::python {

int init_task_handle(PyObject *m);

}  // namespace dftracer::utils::python

// Create a TaskHandle Python object for a typed task (result stored as
#endif  // DFTRACER_UTILS_PYTHON_TASK_HANDLE_H
