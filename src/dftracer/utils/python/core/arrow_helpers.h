// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_ARROW_HELPERS_H
#define DFTRACER_UTILS_PYTHON_CORE_ARROW_HELPERS_H

#include <Python.h>
#include <dftracer/utils/arrow/batch.h>
#include <dftracer/utils/common/config.h>

namespace dftracer::utils::python::core {

using dftracer::utils::arrow::Batch;

/// Wrap a Batch in an ArrowBatchCapsuleObject.
/// Returns a new reference, or NULL on error.
PyObject *wrap_arrow_result(Batch result);

/// Wrap a Python list of ArrowBatchCapsule objects in an ArrowTable.
/// Steals a reference to batch_list on success.
/// Returns a new reference, or NULL on error.
PyObject *wrap_arrow_table(PyObject *batch_list);

/// Wrap an _ArrowBatchStream (or any __arrow_c_stream__ provider) in an
/// ArrowTable. Steals a reference to stream_obj on success.
}  // namespace dftracer::utils::python::core

#endif  // DFTRACER_UTILS_PYTHON_CORE_ARROW_HELPERS_H
