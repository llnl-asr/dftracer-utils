// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_MEMORYVIEW_BATCH_H
#define DFTRACER_UTILS_PYTHON_CORE_MEMORYVIEW_BATCH_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>

namespace dftracer::utils::python::core {

struct MemoryViewBatchData : dstd::HeapObject {
    dstd::Vector<char> buffer;
    dstd::Vector<Py_ssize_t> offsets;
    dstd::Vector<Py_ssize_t> lengths;

    std::size_t entry_count() const { return offsets.size(); }
};

struct MemoryViewBatchObject {
    PyObject_HEAD MemoryViewBatchData *data;
};

extern PyTypeObject MemoryViewBatchType;

PyObject *MemoryViewBatch_item(MemoryViewBatchObject *self, Py_ssize_t i);

struct MemoryViewBatchIteratorState {
    dstd::SharedPtr<dftracer::utils::coro::Channel<MemoryViewBatchData>>
        channel;
    std::mutex error_mtx;
    dstd::Optional<dstd::Error> error;
    std::atomic<bool> cancelled{false};
    std::size_t memory_budget_bytes = 0;
    std::atomic<std::size_t> bytes_in_queue{0};
    dstd::SharedFuture<void> task_future;

    void set_error(dstd::Error e) {
        std::lock_guard<std::mutex> lock(error_mtx);
        if (!error) error.emplace(std::move(e));
    }
};

int init_memoryview_batch(PyObject *m);

}  // namespace dftracer::utils::python::core

#endif  // DFTRACER_UTILS_PYTHON_CORE_MEMORYVIEW_BATCH_H
