// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_STORE_H
#define DFTRACER_UTILS_PYTHON_STORE_H

#include <Python.h>

namespace dftracer::utils::python {

int init_store(PyObject *m);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_STORE_H
