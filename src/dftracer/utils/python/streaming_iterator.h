#ifndef DFTRACER_UTILS_PYTHON_STREAMING_ITERATOR_H
#define DFTRACER_UTILS_PYTHON_STREAMING_ITERATOR_H

#include <Python.h>
#include <dftracer/utils/core/common/config.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#endif

namespace dftracer::utils::python {

/// Generic streaming state for bridging C++ async producers to Python sync
/// consumers.
///
/// Producer (C++ coroutine on Runtime executor):
///   - Calls push() to enqueue items
///   - Calls complete() when done
///   - Calls fail() on error
///
/// Consumer (Python tp_iternext):
///   - Calls pull() which blocks (with GIL released) until item available
///   - Returns std::nullopt on completion or error
template <typename ItemT>
class StreamingState {
   public:
    explicit StreamingState(std::size_t memory_budget_bytes)
        : memory_budget_bytes_(memory_budget_bytes) {}

    bool push(ItemT item, std::size_t item_bytes) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_producer_.wait(lock, [this] {
            return bytes_in_queue_.load(std::memory_order_acquire) <
                       memory_budget_bytes_ ||
                   cancelled_.load(std::memory_order_acquire);
        });
        if (cancelled_.load(std::memory_order_acquire)) {
            return false;
        }
        bytes_in_queue_.fetch_add(item_bytes, std::memory_order_acq_rel);
        queue_.push({std::move(item), item_bytes});
        lock.unlock();
        cv_consumer_.notify_one();
        return true;
    }

    void complete() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            done_.store(true, std::memory_order_release);
        }
        cv_consumer_.notify_all();
    }

    void fail(std::exception_ptr ex) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            error_ = std::move(ex);
            done_.store(true, std::memory_order_release);
        }
        cv_consumer_.notify_all();
    }

    void cancel() {
        cancelled_.store(true, std::memory_order_release);
        cv_producer_.notify_all();
        cv_consumer_.notify_all();
    }

    std::optional<ItemT> pull() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_consumer_.wait(lock, [this] {
            return !queue_.empty() ||
                   cancelled_.load(std::memory_order_acquire) ||
                   done_.load(std::memory_order_acquire);
        });

        if (cancelled_.load(std::memory_order_acquire) && queue_.empty()) {
            return std::nullopt;
        }

        if (queue_.empty()) {
            return std::nullopt;
        }

        auto [item, size] = std::move(queue_.front());
        queue_.pop();
        bytes_in_queue_.fetch_sub(size, std::memory_order_acq_rel);
        lock.unlock();
        cv_producer_.notify_one();
        return std::move(item);
    }

    std::exception_ptr error() const { return error_; }

    bool cancelled() const {
        return cancelled_.load(std::memory_order_acquire);
    }

    bool done() const { return done_.load(std::memory_order_acquire); }

    void set_task_future(std::shared_future<void> future) {
        task_future_ = std::move(future);
    }

   private:
    struct QueueEntry {
        ItemT item;
        std::size_t size;
    };
    std::queue<QueueEntry> queue_;
    std::mutex mtx_;
    std::condition_variable cv_producer_;
    std::condition_variable cv_consumer_;
    std::exception_ptr error_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> done_{false};
    std::size_t memory_budget_bytes_;
    std::atomic<std::size_t> bytes_in_queue_{0};
    std::shared_future<void> task_future_;
};

#ifdef DFTRACER_UTILS_ENABLE_ARROW

using utilities::common::arrow::ArrowExportResult;

/// Internal C++ state for ArrowStreamingIterator.
/// Stored as a pointer to avoid C++ object layout issues with Python.
struct ArrowStreamingIteratorState {
    std::shared_ptr<void> state;
    std::function<std::optional<ArrowExportResult>()> pull_next;
    std::function<std::exception_ptr()> get_error;
    std::function<void()> cancel;
};

/// Type-erased Arrow streaming iterator for Python.
///
/// This allows different producer types (AggregationBatch, ArrowExportResult,
/// etc.) to share the same Python iterator mechanics.
struct ArrowStreamingIteratorObject {
    PyObject_HEAD

        /// Pointer to C++ state (owned, allocated with new).
        ArrowStreamingIteratorState* cpp_state;
};

extern PyTypeObject ArrowStreamingIteratorType;

/// Initialize the ArrowStreamingIteratorType.
int init_arrow_streaming_iterator(PyObject* m);

#endif  // DFTRACER_UTILS_ENABLE_ARROW

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_STREAMING_ITERATOR_H
