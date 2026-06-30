#include <dftracer/utils/python/py_errors.h>

#include <stdexcept>
#include <string>

PyObject *g_dft_error = nullptr;
PyObject *g_dft_value_error = nullptr;
PyObject *g_dft_not_found_error = nullptr;
PyObject *g_dft_io_error = nullptr;
PyObject *g_dft_parse_error = nullptr;
PyObject *g_dft_compression_error = nullptr;
PyObject *g_dft_query_error = nullptr;
PyObject *g_dft_reader_error = nullptr;
PyObject *g_dft_indexer_error = nullptr;
PyObject *g_dft_pipeline_error = nullptr;
PyObject *g_dft_aggregation_error = nullptr;

using dftracer::utils::ErrorCode;

namespace {

// Create exception "dftracer_utils_ext.<short_name>", register it on m, and
// keep a strong ref in *slot.
int add_exc(PyObject *m, const char *short_name, PyObject *base,
            PyObject **slot) {
    const std::string qualified =
        std::string("dftracer_utils_ext.") + short_name;
    PyObject *exc = PyErr_NewException(qualified.c_str(), base, nullptr);
    if (exc == nullptr) return -1;
    Py_INCREF(exc);  // keep one strong ref in *slot in addition to the module's
    if (PyModule_AddObject(m, short_name, exc) < 0) {
        Py_DECREF(exc);  // undo AddObject's intended steal
        Py_DECREF(exc);  // undo our INCREF
        return -1;
    }
    *slot = exc;
    return 0;
}

}  // namespace

int init_py_errors(PyObject *m) {
    if (add_exc(m, "DFTUtilsError", PyExc_RuntimeError, &g_dft_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsValueError", g_dft_error, &g_dft_value_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsNotFoundError", g_dft_error,
                &g_dft_not_found_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsIOError", g_dft_error, &g_dft_io_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsParseError", g_dft_error, &g_dft_parse_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsCompressionError", g_dft_error,
                &g_dft_compression_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsQueryError", g_dft_error, &g_dft_query_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsReaderError", g_dft_error, &g_dft_reader_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsIndexerError", g_dft_error, &g_dft_indexer_error) <
        0)
        return -1;
    if (add_exc(m, "DFTUtilsPipelineError", g_dft_error,
                &g_dft_pipeline_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsAggregationError", g_dft_error,
                &g_dft_aggregation_error) < 0)
        return -1;
    return 0;
}

PyObject *py_error_type_for(ErrorCode code) {
    switch (code) {
        case ErrorCode::INVALID_ARGUMENT:
            return g_dft_value_error;
        case ErrorCode::NOT_FOUND:
            return g_dft_not_found_error;
        case ErrorCode::IO:
            return g_dft_io_error;
        case ErrorCode::PARSE:
            return g_dft_parse_error;
        case ErrorCode::COMPRESSION:
            return g_dft_compression_error;
        case ErrorCode::QUERY:
            return g_dft_query_error;
        case ErrorCode::READER:
            return g_dft_reader_error;
        case ErrorCode::INDEXER:
            return g_dft_indexer_error;
        case ErrorCode::PIPELINE:
            return g_dft_pipeline_error;
        case ErrorCode::AGGREGATION:
            return g_dft_aggregation_error;
        case ErrorCode::UNKNOWN:
        case ErrorCode::INTERNAL:
            return g_dft_error;
    }
    return g_dft_error;
}

void set_typed_py_error(const std::exception &e) {
    if (const auto *de =
            dynamic_cast<const dftracer::utils::DFTUtilsException *>(&e)) {
        PyErr_SetString(py_error_type_for(de->code()), e.what());
    } else if (dynamic_cast<const std::invalid_argument *>(&e)) {
        PyErr_SetString(g_dft_value_error, e.what());
    } else {
        PyErr_SetString(g_dft_error, e.what());
    }
}
