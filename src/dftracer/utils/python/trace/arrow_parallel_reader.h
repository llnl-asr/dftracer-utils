// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_TRACE_ARROW_PARALLEL_READER_H
#define DFTRACER_UTILS_PYTHON_TRACE_ARROW_PARALLEL_READER_H

#include <Python.h>
#include <dftracer/utils/common/config.h>

namespace dftracer::utils::python::trace {

int init_arrow_parallel_reader(PyObject* m);

}  // namespace dftracer::utils::python::trace

#endif  // DFTRACER_UTILS_PYTHON_TRACE_ARROW_PARALLEL_READER_H
