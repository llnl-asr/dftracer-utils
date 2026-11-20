// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FRAME_UDF_PYTHON_H
#define DFTRACER_UTILS_PYTHON_FRAME_UDF_PYTHON_H

#include <Python.h>

namespace dftracer::utils::python::frame {

// Register the module functions behind `@dft.udf`: udf_register_library (a
// JIT-compiled UDF), udf_register_batch (a per-batch Python UDF run on the one
// Python lane thread), udf_unregister, udf_lane_stats and udf_lane_shutdown.
// Returns 0 on success, -1 with the Python error set on failure.
int init_udf_python(PyObject* m);

}  // namespace dftracer::utils::python::frame

#endif  // DFTRACER_UTILS_PYTHON_FRAME_UDF_PYTHON_H
