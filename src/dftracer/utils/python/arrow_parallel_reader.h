#ifndef DFTRACER_UTILS_PYTHON_ARROW_PARALLEL_READER_H
#define DFTRACER_UTILS_PYTHON_ARROW_PARALLEL_READER_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <Python.h>

namespace dftracer::utils::python {

int init_arrow_parallel_reader(PyObject* m);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
#endif  // DFTRACER_UTILS_PYTHON_ARROW_PARALLEL_READER_H
