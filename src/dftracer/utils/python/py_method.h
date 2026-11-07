// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_PY_METHOD_H
#define DFTRACER_UTILS_PYTHON_PY_METHOD_H

#include <Python.h>

// Cast a typed C-API callback to the generic PyCFunction slot in PyMethodDef,
// routing through void(*)() so -Wcast-function-type stays quiet. Replaces the
// private CPython _PyCFunction_CAST, which is not defined on every toolchain.
#define DFTU_PYCFUNCTION(f) \
    (reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(f)))

#endif  // DFTRACER_UTILS_PYTHON_PY_METHOD_H
