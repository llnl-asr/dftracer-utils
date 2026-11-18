// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FRAME_SERIES_H
#define DFTRACER_UTILS_PYTHON_FRAME_SERIES_H

#include <Python.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/core/type_helpers.h>

namespace dftracer::utils::python::frame {

// Register the Series type and its factories on module `m`.
// Returns 0 on success, -1 with the Python error set on failure.
int init_series(PyObject* m);

// The Series type and the slot its factories allocate, for _bind_types.
TypeBinding series_binding();

// Wrap an owned dataframe::Series in a new Series Python object (steals the
// column). Used by the trace view to hand its scanned batch to Python.
PyObject* wrap_vec_column(dftracer::utils::dataframe::Series&& col);

// Borrow the dataframe::Series backing a Series object, or nullptr (without
// setting a Python error) if `o` is not a Series. The pointer is valid for
// the lifetime of `o`.
const dftracer::utils::dataframe::Series* unwrap_vec_column(PyObject* o);

// Import one Arrow array (any object exposing __arrow_c_array__) into an owned
// dataframe::Series. Returns an invalid Series with a Python error set on
// failure.
dftracer::utils::dataframe::Series column_from_arrow(PyObject* arrow_array);

// The Series of a sequence (as a list or tuple of Series), sharing their
// buffers. False with a TypeError set when `seq` is no sequence or holds
// another object.
bool series_list_from_py(
    PyObject* seq, const char* what,
    dftracer::utils::dstd::Vector<dftracer::utils::dataframe::Series>& out);

// The label lookup results as Python values: `(positions, missing)`, two lists
// of ints; `(offset, length)`. A failed lookup sets the Python exception its
// error maps to and returns nullptr.
PyObject* located_to_py(
    dftracer::utils::dstd::Result<dftracer::utils::dataframe::Located>&& r);
PyObject* label_span_to_py(
    dftracer::utils::dstd::Result<dftracer::utils::dataframe::LabelSpan>&& r);

}  // namespace dftracer::utils::python::frame

#endif  // DFTRACER_UTILS_PYTHON_FRAME_SERIES_H
