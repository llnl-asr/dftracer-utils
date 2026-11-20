// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_ARROW_HELPERS_H
#define DFTRACER_UTILS_PYTHON_CORE_ARROW_HELPERS_H

#include <Python.h>
#include <dftracer/utils/arrow/batch.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/memory.h>

namespace dftracer::utils::python::core {

using dftracer::utils::arrow::Batch;

/// Wrap a Batch in an ArrowBatchCapsuleObject.
/// Returns a new reference, or NULL on error.
PyObject *wrap_arrow_result(Batch result);

/// Sets NotImplementedError and returns false when @p requested_schema, the
/// argument of an __arrow_c_stream__ call, is given and not None.
bool no_requested_schema(PyObject *requested_schema);

/// Hands @p stream to a new "arrow_array_stream" capsule, whose destructor
/// releases and frees it. On failure the stream is released and freed too.
/// Returns a new reference, or NULL on error.
PyObject *stream_capsule(dstd::UniquePtr<ArrowArrayStream> stream);

}  // namespace dftracer::utils::python::core

#endif  // DFTRACER_UTILS_PYTHON_CORE_ARROW_HELPERS_H
