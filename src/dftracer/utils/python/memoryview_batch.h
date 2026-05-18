#ifndef DFTRACER_UTILS_PYTHON_MEMORYVIEW_BATCH_H
#define DFTRACER_UTILS_PYTHON_MEMORYVIEW_BATCH_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/core/coro/channel.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace dftracer::utils::python {

struct MemoryViewBatchData {
    std::vector<char> buffer;
    std::vector<Py_ssize_t> offsets;
    std::vector<Py_ssize_t> lengths;

    std::size_t num_entries() const { return offsets.size(); }
};

struct MemoryViewBatchObject {
    PyObject_HEAD MemoryViewBatchData *data;
};

extern PyTypeObject MemoryViewBatchType;

PyObject *MemoryViewBatch_item(MemoryViewBatchObject *self, Py_ssize_t i);

struct MemoryViewBatchIteratorState {
    std::shared_ptr<dftracer::utils::coro::Channel<MemoryViewBatchData>>
        channel;
    std::mutex error_mtx;
    std::exception_ptr error;
    std::atomic<bool> cancelled{false};
    std::size_t memory_budget_bytes = 0;
    std::atomic<std::size_t> bytes_in_queue{0};
    std::shared_future<void> task_future;

    void set_error(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(error_mtx);
        if (!error) error = e;
    }
};

int init_memoryview_batch(PyObject *m);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_MEMORYVIEW_BATCH_H
