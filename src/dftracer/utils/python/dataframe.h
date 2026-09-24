#ifndef DFTRACER_UTILS_PYTHON_DATAFRAME_H
#define DFTRACER_UTILS_PYTHON_DATAFRAME_H

#include <Python.h>

namespace dftracer::utils::dataframe {
struct DataFrame;
}

namespace dftracer::utils::python {

// Register the DataFrame type on module `m`. Returns 0 on success, -1 with the
// Python error set on failure.
int init_dataframe(PyObject* m);

// Wrap an owned dataframe::DataFrame in a new DataFrame Python object (steals
// the batch). Requires the arrow-enabled build; returns nullptr with a Python
// error otherwise. Used by the trace view to hand its scanned batch to Python
// as native vec columns; Arrow is produced only on an explicit
// to_arrow()/to_pandas().
PyObject* wrap_dataframe(dftracer::utils::dataframe::DataFrame&& batch);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_DATAFRAME_H
