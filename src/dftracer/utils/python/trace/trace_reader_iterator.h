// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_TRACE_TRACE_READER_ITERATOR_H
#define DFTRACER_UTILS_PYTHON_TRACE_TRACE_READER_ITERATOR_H

#include <Python.h>
#include <dftracer/utils/arrow/batch.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/python/core/memoryview_batch.h>
#include <dftracer/utils/runtime/task_handle.h>

typedef struct {
    PyObject_HEAD dftracer::utils::arrow::Batch *result;
} ArrowBatchCapsuleObject;

extern PyTypeObject ArrowBatchCapsuleType;

enum class IteratorMode {
    Memoryview,
    Arrow,
};

struct ArrowIteratorState {
    using BatchType = dftracer::utils::arrow::Batch;
    dftracer::utils::dstd::SharedPtr<dftracer::utils::coro::Channel<BatchType>>
        channel;
    std::mutex error_mtx;
    dftracer::utils::dstd::Optional<dftracer::utils::dstd::Error> error;
    std::atomic<bool> cancelled{false};
    std::size_t memory_budget_bytes = 0;
    std::atomic<std::size_t> bytes_in_queue{0};
    dftracer::utils::dstd::SharedFuture<void> task_future;

    void set_error(dftracer::utils::dstd::Error e) {
        std::lock_guard<std::mutex> lock(error_mtx);
        if (!error) error.emplace(std::move(e));
    }
};

using dftracer::utils::python::core::MemoryViewBatchIteratorState;
using dftracer::utils::python::core::MemoryViewBatchObject;
using dftracer::utils::python::core::MemoryViewBatchType;

typedef struct {
    PyObject_HEAD dftracer::utils::dstd::SharedPtr<
        dftracer::utils::python::core::MemoryViewBatchIteratorState>
        batch_state;
    PyObject *current_batch;
    Py_ssize_t batch_index;
    dftracer::utils::dstd::SharedPtr<ArrowIteratorState> arrow_state;
    IteratorMode mode;
} TraceReaderIteratorObject;

extern PyTypeObject TraceReaderIteratorType;

namespace dftracer::utils::python::trace {

int init_trace_reader_iterator(PyObject *m);

}  // namespace dftracer::utils::python::trace

#endif  // DFTRACER_UTILS_PYTHON_TRACE_TRACE_READER_ITERATOR_H
