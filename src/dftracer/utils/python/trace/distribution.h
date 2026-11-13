// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_TRACE_DISTRIBUTION_H
#define DFTRACER_UTILS_PYTHON_TRACE_DISTRIBUTION_H

#include <Python.h>

namespace dftracer::utils::python::trace {

int init_distribution(PyObject *m);

}  // namespace dftracer::utils::python::trace

#endif  // DFTRACER_UTILS_PYTHON_TRACE_DISTRIBUTION_H
