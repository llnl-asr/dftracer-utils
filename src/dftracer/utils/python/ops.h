#ifndef DFTRACER_UTILS_PYTHON_OPS_H
#define DFTRACER_UTILS_PYTHON_OPS_H

#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif
#include <Python.h>

namespace dftracer::utils::python {

// Register op_list / op_info / op_run on module `m`: the Python view of the
// dataframe op registry (dftu_op_*). Returns 0 on success, -1 with a Python
// error set.
int init_ops(PyObject* m);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_OPS_H
