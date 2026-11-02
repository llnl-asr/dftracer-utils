// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_INDEX_H
#define DFTRACER_UTILS_PYTHON_INDEX_H

#include <Python.h>

namespace dftracer::utils::index {
class Index;
}  // namespace dftracer::utils::index

// Index: the native handle behind the Python Index, one C++ index::Index. Each
// call takes its own snapshot, so it sees the commits of other processes.
typedef struct {
    PyObject_HEAD dftracer::utils::index::Index *index;
} IndexObject;

extern PyTypeObject IndexType;

namespace dftracer::utils::python {

int init_index(PyObject *m);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_INDEX_H
