#ifndef DFTRACER_UTILS_PYTHON_SERIES_H
#define DFTRACER_UTILS_PYTHON_SERIES_H

#include <Python.h>

namespace dftracer::utils::dataframe {
class Series;
}

namespace dftracer::utils::python {

// Register the Series type and the `vec_from_arrow` factory on module `m`.
// Returns 0 on success, -1 with the Python error set on failure.
int init_series(PyObject* m);

// Wrap an owned dataframe::Series in a new Series Python object (steals the
// column). Requires the arrow-enabled build; returns nullptr with a Python
// error otherwise. Used by the trace view to hand its scanned batch to Python.
PyObject* wrap_vec_column(dftracer::utils::dataframe::Series&& col);

// Borrow the dataframe::Series backing a Series object, or nullptr (without
// setting a Python error) if `o` is not a Series. The pointer is valid for
// the lifetime of `o`.
const dftracer::utils::dataframe::Series* unwrap_vec_column(PyObject* o);

// Import one Arrow array (any object exposing __arrow_c_array__) into an owned
// dataframe::Series. Returns an invalid Series with a Python error set on
// failure.
dftracer::utils::dataframe::Series column_from_arrow(PyObject* arrow_array);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_SERIES_H
