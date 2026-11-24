// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/errc.h>
#include <dftracer/utils/python/core/errors.h>

#include <atomic>
#include <new>
#include <stdexcept>

namespace dftracer::utils::python::core {

PyObject *g_dft_error = nullptr;
PyObject *g_dft_value_error = nullptr;
PyObject *g_dft_not_found_error = nullptr;
PyObject *g_dft_io_error = nullptr;
PyObject *g_dft_parse_error = nullptr;
PyObject *g_dft_compression_error = nullptr;
PyObject *g_dft_duql_error = nullptr;
PyObject *g_dft_reader_error = nullptr;
PyObject *g_dft_indexer_error = nullptr;
PyObject *g_dft_pipeline_error = nullptr;
PyObject *g_dft_aggregation_error = nullptr;

namespace {

// Create exception "dftracer.utils.dftracer_utils_ext.<short_name>", register
// it on m, and keep a strong ref in *slot.
int add_exc(PyObject *m, const char *short_name, PyObject *base,
            PyObject **slot) {
    dstd::String qualified("dftracer.utils.dftracer_utils_ext.");
    qualified.append(short_name);
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
    PyObject *value_bases = PyTuple_Pack(2, g_dft_error, PyExc_ValueError);
    if (value_bases == nullptr) return -1;
    const int value_status =
        add_exc(m, "DFTUtilsValueError", value_bases, &g_dft_value_error);
    Py_DECREF(value_bases);
    if (value_status < 0) return -1;
    PyObject *not_found_bases = PyTuple_Pack(2, g_dft_error, PyExc_KeyError);
    if (not_found_bases == nullptr) return -1;
    const int not_found_status = add_exc(
        m, "DFTUtilsNotFoundError", not_found_bases, &g_dft_not_found_error);
    Py_DECREF(not_found_bases);
    if (not_found_status < 0) return -1;
    // KeyError.__str__ quotes the message; keep the plain one.
    PyObject *plain_str =
        PyObject_GetAttrString(PyExc_BaseException, "__str__");
    if (plain_str == nullptr) return -1;
    const int str_status =
        PyObject_SetAttrString(g_dft_not_found_error, "__str__", plain_str);
    Py_DECREF(plain_str);
    if (str_status < 0) return -1;
    if (add_exc(m, "DFTUtilsIOError", g_dft_error, &g_dft_io_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsParseError", g_dft_error, &g_dft_parse_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsCompressionError", g_dft_error,
                &g_dft_compression_error) < 0)
        return -1;
    if (add_exc(m, "DFTUtilsDuqlError", g_dft_error, &g_dft_duql_error) < 0)
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

PyObject *py_error_type_for(common::ErrorCode code) {
    switch (code) {
        case common::ErrorCode::InvalidArgument:
            return g_dft_value_error;
        case common::ErrorCode::NotFound:
            return g_dft_not_found_error;
        case common::ErrorCode::Io:
            return g_dft_io_error;
        case common::ErrorCode::Parse:
            return g_dft_parse_error;
        case common::ErrorCode::Compression:
            return g_dft_compression_error;
        case common::ErrorCode::Duql:
            return g_dft_duql_error;
        case common::ErrorCode::Reader:
            return g_dft_reader_error;
        case common::ErrorCode::Indexer:
            return g_dft_indexer_error;
        case common::ErrorCode::Pipeline:
            return g_dft_pipeline_error;
        case common::ErrorCode::Aggregation:
            return g_dft_aggregation_error;
        case common::ErrorCode::Unknown:
        case common::ErrorCode::Internal:
        case common::ErrorCode::MemoryBudgetExceeded:
            return g_dft_error;
    }
    return g_dft_error;
}

PyObject *py_error_type_for(const dftracer::utils::dstd::Error &error) {
    // Domain-specific types take precedence over the portable-condition map.
    if (error.domain().id == dftracer::utils::duql::ERROR_DOMAIN.id)
        return g_dft_duql_error;
    if (error.condition() == dftracer::utils::dstd::Condition::OutOfMemory)
        return PyExc_MemoryError;
    return py_error_type_for(common::errorcode_of(error));
}

namespace {

std::atomic<bool> g_exit_cancel{false};
std::atomic<unsigned long> g_exit_thread{0};

bool set_exit_cancel(const dftracer::utils::dstd::Error &error) {
    if (!error.is(dftracer::utils::dstd::Condition::Cancelled) ||
        !g_exit_cancel.load(std::memory_order_acquire) ||
        PyThread_get_thread_ident() ==
            g_exit_thread.load(std::memory_order_relaxed)) {
        return false;
    }
    // Bare, so its code is 0. A subclass or a message would be printed.
    PyErr_SetNone(PyExc_SystemExit);
    return true;
}

}  // namespace

void mark_exit_cancel() noexcept {
    g_exit_thread.store(PyThread_get_thread_ident(), std::memory_order_relaxed);
    g_exit_cancel.store(true, std::memory_order_release);
}

void set_py_error(const dftracer::utils::dstd::Error &error) {
    if (set_exit_cancel(error)) return;
    PyErr_SetString(py_error_type_for(error), error.c_str());
}

void set_typed_py_error(const std::exception &e) {
    if (const auto *de = dynamic_cast<const PyErrorException *>(&e)) {
        if (set_exit_cancel(de->error())) return;
        PyErr_SetString(py_error_type_for(de->error()), e.what());
    } else if (dynamic_cast<const std::invalid_argument *>(&e)) {
        PyErr_SetString(g_dft_value_error, e.what());
    } else if (dynamic_cast<const std::out_of_range *>(&e)) {
        PyErr_SetString(PyExc_KeyError, e.what());
    } else if (dynamic_cast<const std::bad_alloc *>(&e)) {
        PyErr_NoMemory();
    } else {
        PyErr_SetString(g_dft_error, e.what());
    }
}

}  // namespace dftracer::utils::python::core
