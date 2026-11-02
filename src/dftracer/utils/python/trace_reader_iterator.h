// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_TRACE_READER_ITERATOR_H
#define DFTRACER_UTILS_PYTHON_TRACE_READER_ITERATOR_H

#include <Python.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/python/memoryview_batch.h>
#include <dftracer/utils/runtime/task_handle.h>
#include <dftracer/utils/trace/args_map.h>
#include <dftracer/utils/utilities/reader/internal/json_dict_builder.h>

#include <memory>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/utilities/arrow/arrow_export.h>

typedef struct {
    PyObject_HEAD dftracer::utils::utilities::arrow::ArrowExportResult *result;
} ArrowBatchCapsuleObject;

extern PyTypeObject ArrowBatchCapsuleType;
#endif

enum class IteratorMode {
    Memoryview,
    JsonDict,
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    Arrow,
#endif
};

#ifdef DFTRACER_UTILS_ENABLE_ARROW
struct ArrowIteratorState {
    using BatchType = dftracer::utils::utilities::arrow::ArrowExportResult;
    dftracer::utils::dstd::SharedPtr<dftracer::utils::coro::Channel<BatchType>>
        channel;
    std::mutex error_mtx;
    std::exception_ptr error;
    std::atomic<bool> cancelled{false};
    std::size_t memory_budget_bytes = 0;
    std::atomic<std::size_t> bytes_in_queue{0};
    dftracer::utils::dstd::SharedFuture<void> task_future;

    void set_error(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(error_mtx);
        if (!error) error = e;
    }
};
#endif

using ArgsValue = dftracer::utils::trace::ArgsValue;
using ArgsMap = dftracer::utils::trace::ArgsMap;

using dftracer::utils::utilities::reader::internal::JsonDictBatch;
using dftracer::utils::utilities::reader::internal::JsonDictEvent;

struct JsonDictIteratorState {
    dftracer::utils::dstd::SharedPtr<
        dftracer::utils::coro::Channel<JsonDictBatch>>
        channel;
    std::mutex error_mtx;
    std::exception_ptr error;
    std::atomic<bool> cancelled{false};
    std::size_t memory_budget_bytes = 0;
    std::atomic<std::size_t> bytes_in_queue{0};
    dftracer::utils::dstd::SharedFuture<void> task_future;

    void set_error(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(error_mtx);
        if (!error) error = e;
    }
};

using dftracer::utils::python::MemoryViewBatchIteratorState;
using dftracer::utils::python::MemoryViewBatchObject;
using dftracer::utils::python::MemoryViewBatchType;

typedef struct {
    PyObject_HEAD dftracer::utils::dstd::SharedPtr<MemoryViewBatchIteratorState>
        batch_state;
    PyObject *current_batch;
    Py_ssize_t batch_index;
    dftracer::utils::dstd::SharedPtr<JsonDictIteratorState> json_dict_state;
    dftracer::utils::dstd::SharedPtr<JsonDictBatch> json_dict_current_batch;
    Py_ssize_t json_dict_index;
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    dftracer::utils::dstd::SharedPtr<ArrowIteratorState> arrow_state;
#endif
    IteratorMode mode;
} TraceReaderIteratorObject;

extern PyTypeObject TraceReaderIteratorType;

namespace dftracer::utils::python {

int init_trace_reader_iterator(PyObject *m);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_TRACE_READER_ITERATOR_H
