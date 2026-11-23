// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FORMATS_BIND_H
#define DFTRACER_UTILS_PYTHON_FORMATS_BIND_H

#include <Python.h>

namespace dftracer::utils::python::formats {

/// Registers the types, the kind enums and the functions of
/// dftracer.utils.formats.json and dftracer.utils.formats.yaml on `m`.
int init_formats(PyObject *m);

}  // namespace dftracer::utils::python::formats

#endif  // DFTRACER_UTILS_PYTHON_FORMATS_BIND_H
