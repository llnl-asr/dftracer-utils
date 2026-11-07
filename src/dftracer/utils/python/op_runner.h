// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_OP_RUNNER_H
#define DFTRACER_UTILS_PYTHON_OP_RUNNER_H

#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif
#include <Python.h>

namespace dftracer::utils::python {

// Register jit_run_op on module `m`: the standalone driver for a
// Python-authored compose op (jit_op.Op). Returns 0 on success, -1 with a
// Python error set.
int init_op_runner(PyObject* m);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_OP_RUNNER_H
