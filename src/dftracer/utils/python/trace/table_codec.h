// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_TRACE_TABLE_CODEC_H
#define DFTRACER_UTILS_PYTHON_TRACE_TABLE_CODEC_H

#include <Python.h>

namespace dftracer::utils::python::trace {

// Registers _write_table and _read_table, the private entry points of the
// table file codec.
int init_table_codec(PyObject *m);

}  // namespace dftracer::utils::python::trace

#endif  // DFTRACER_UTILS_PYTHON_TRACE_TABLE_CODEC_H
