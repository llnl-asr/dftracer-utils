// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_TRACE_TRACE_WRITER_H
#define DFTRACER_UTILS_PYTHON_TRACE_TRACE_WRITER_H

#include <Python.h>

namespace dftracer::utils::python::trace {

// TraceWriter: the native handle behind the Python TraceWriter, one
// trace::writer::TraceWriter that writes DFTracer records to a file.
int init_trace_writer(PyObject *m);

}  // namespace dftracer::utils::python::trace

#endif  // DFTRACER_UTILS_PYTHON_TRACE_TRACE_WRITER_H
