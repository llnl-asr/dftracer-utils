// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_STORE_STORE_H
#define DFTRACER_UTILS_PYTHON_STORE_STORE_H

#include <Python.h>

namespace dftracer::utils::python::store {

int init_store(PyObject *m);

}  // namespace dftracer::utils::python::store

#endif  // DFTRACER_UTILS_PYTHON_STORE_STORE_H
