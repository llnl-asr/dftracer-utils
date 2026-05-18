#ifndef DFTRACER_UTILS_PYTHON_ARROW_HELPERS_H
#define DFTRACER_UTILS_PYTHON_ARROW_HELPERS_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <Python.h>
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

namespace dftracer::utils::python {

using utilities::common::arrow::ArrowExportResult;

/// Wrap an ArrowExportResult in an ArrowBatchCapsuleObject.
/// Returns a new reference, or NULL on error.
PyObject *wrap_arrow_result(ArrowExportResult result);

/// Wrap a Python list of ArrowBatchCapsule objects in an ArrowTable.
/// Steals a reference to batch_list on success.
/// Returns a new reference, or NULL on error.
PyObject *wrap_arrow_table(PyObject *batch_list);

/// Convenience: wrap a single ArrowExportResult as a 1-batch ArrowTable.
/// Returns a new reference, or NULL on error.
PyObject *arrow_result_to_table(ArrowExportResult result);

/// Wrap an _ArrowBatchStream (or any __arrow_c_stream__ provider) in an
/// ArrowTable. Steals a reference to stream_obj on success.
/// Returns a new reference, or NULL on error.
PyObject *wrap_arrow_stream_table(PyObject *stream_obj);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_PYTHON_ARROW_HELPERS_H
