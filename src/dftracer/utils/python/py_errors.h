#ifndef DFTRACER_UTILS_PYTHON_PY_ERRORS_H
#define DFTRACER_UTILS_PYTHON_PY_ERRORS_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/core/common/error.h>

#include <exception>

// Typed Python exception objects
extern PyObject *g_dft_error;              // DFTUtilsError (base)
extern PyObject *g_dft_value_error;        // DFTUtilsValueError
extern PyObject *g_dft_not_found_error;    // DFTUtilsNotFoundError
extern PyObject *g_dft_io_error;           // DFTUtilsIOError
extern PyObject *g_dft_parse_error;        // DFTUtilsParseError
extern PyObject *g_dft_compression_error;  // DFTUtilsCompressionError
extern PyObject *g_dft_query_error;        // DFTUtilsQueryError
extern PyObject *g_dft_reader_error;       // DFTUtilsReaderError
extern PyObject *g_dft_indexer_error;      // DFTUtilsIndexerError
extern PyObject *g_dft_pipeline_error;     // DFTUtilsPipelineError
extern PyObject *g_dft_aggregation_error;  // DFTUtilsAggregationError

// Register the exception hierarchy on module `m`. Returns 0 on success, or -1
// with a Python error set.
int init_py_errors(PyObject *m);

// Map a coarse ErrorCode to its Python exception type (borrowed reference).
// Falls back to the DFTUtilsError base for UNKNOWN/INTERNAL.
PyObject *py_error_type_for(dftracer::utils::ErrorCode code);

// Set the current Python error from a caught C++ exception, choosing the typed
// DFTUtils* exception (DFTUtilsException -> its code; std::invalid_argument ->
// DFTUtilsValueError; else the DFTUtilsError base). Mirrors run_blocking's
// dispatch for the bindings that catch and translate inline.
void set_typed_py_error(const std::exception &e);

#endif  // DFTRACER_UTILS_PYTHON_PY_ERRORS_H
