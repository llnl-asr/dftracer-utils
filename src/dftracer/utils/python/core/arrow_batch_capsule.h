// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_ARROW_BATCH_CAPSULE_H
#define DFTRACER_UTILS_PYTHON_CORE_ARROW_BATCH_CAPSULE_H

#include <Python.h>
#include <dftracer/utils/arrow/batch.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/runtime/task_handle.h>

#include <atomic>
#include <cstddef>
#include <mutex>

typedef struct {
    PyObject_HEAD dftracer::utils::arrow::Batch *result;
} ArrowBatchCapsuleObject;

extern PyTypeObject ArrowBatchCapsuleType;

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

namespace dftracer::utils::python::core {

int init_arrow_batch_capsule(PyObject *m);

}  // namespace dftracer::utils::python::core

#endif  // DFTRACER_UTILS_PYTHON_CORE_ARROW_BATCH_CAPSULE_H
