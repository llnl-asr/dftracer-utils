#include "dftracer/utils/core/pipeline/io_executor.h"

#include <algorithm>
#include <stdexcept>

#include "dftracer/utils/core/pipeline/io_backend.h"
#include "dftracer/utils/core/tasks/task.h"

namespace dftracer::utils {

// ============================================================================
// IOExecutor Implementation
// ============================================================================

std::unique_ptr<IOExecutor> IOExecutor::create(
    std::size_t num_io_threads, std::size_t num_workers,
    moodycamel::BlockingConcurrentQueue<TaskItem>* slow_path_queue) {
    return std::unique_ptr<IOExecutor>(
        new IOExecutor(num_io_threads, num_workers, slow_path_queue));
}

IOExecutor::IOExecutor(
    std::size_t num_io_threads, std::size_t num_workers,
    moodycamel::BlockingConcurrentQueue<TaskItem>* slow_path_queue)
    : num_io_threads_(num_io_threads),
      num_workers_(num_workers),
      slow_path_queue_(slow_path_queue) {
    if (num_io_threads == 0) {
        throw std::invalid_argument("num_io_threads must be > 0");
    }
    if (num_workers == 0) {
        throw std::invalid_argument("num_workers must be > 0");
    }
    if (slow_path_queue == nullptr) {
        throw std::invalid_argument("slow_path_queue cannot be nullptr");
    }

    // Create platform-specific I/O backend
    io_backend_ = IOBackend::create();
    if (!io_backend_) {
        throw std::runtime_error("Failed to create I/O backend");
    }

    // Initialize fast path queues (per worker)
    fast_path_queues_.resize(num_workers);
    for (std::size_t i = 0; i < num_workers; ++i) {
        // Each worker gets multiple queues for I/O thread round-robin
        fast_path_queues_[i].resize(num_io_threads);
        for (std::size_t j = 0; j < num_io_threads; ++j) {
            fast_path_queues_[i][j] = std::make_unique<FastPathQueue>();
        }
    }
}

IOExecutor::~IOExecutor() { shutdown(); }

void IOExecutor::start() {
    if (running_.exchange(true)) {
        return;  // Already running
    }

    // Spawn I/O threads
    io_threads_.reserve(num_io_threads_);
    for (std::size_t i = 0; i < num_io_threads_; ++i) {
        io_threads_.emplace_back(&IOExecutor::io_thread_loop, this, i);
    }
}

void IOExecutor::shutdown() {
    if (!running_.exchange(false)) {
        return;  // Already stopped
    }

    // Join I/O threads
    for (auto& thread : io_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    io_threads_.clear();
}

std::uint64_t IOExecutor::submit_io_operation(
    std::function<void()> io_func, std::coroutine_handle<> continuation,
    std::size_t preferred_worker_id) {
    if (!running_.load()) {
        throw std::runtime_error("IOExecutor is not running");
    }

    // Create I/O request
    IORequest request(std::move(io_func), continuation, preferred_worker_id);
    request.request_id = next_request_id_.fetch_add(1);

    // Enqueue request for I/O threads to pick up
    io_request_queue_.enqueue(request);
    pending_io_ops_.fetch_add(1);

    return request.request_id;
}

bool IOExecutor::try_pop_fast_path(std::size_t worker_id, TaskItem& item) {
    if (worker_id >= num_workers_) {
        return false;
    }

    // Try to pop from any of this worker's fast path queues
    for (std::size_t i = 0; i < num_io_threads_; ++i) {
        if (fast_path_queues_[worker_id][i]->queue.try_dequeue(item)) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Private Methods
// ============================================================================

void IOExecutor::io_thread_loop(std::size_t thread_id) {
    // I/O thread main loop
    while (running_.load()) {
        // Try to dequeue I/O request (blocking with timeout)
        IORequest request;
        if (io_request_queue_.wait_dequeue_timed(
                request, std::chrono::milliseconds(10))) {
            // Execute the I/O request
            execute_io_request(thread_id, request);
        }
    }
}

void IOExecutor::execute_io_request(std::size_t io_thread_id,
                                    const IORequest& request) {
    // Execute the I/O operation (this will block on actual I/O)
    if (request.io_func) {
        request.io_func();
    }

    // Decrement pending count
    pending_io_ops_.fetch_sub(1);
    completed_io_ops_.fetch_add(1);

    // If there's a coroutine to resume, we have two options:
    // 1. Resume directly on I/O thread (fastest, but may block I/O thread)
    // 2. Route to worker queue (better load balancing, but adds latency)
    if (request.continuation) {
        // Determine target worker
        std::size_t target_worker = request.target_worker_id;
        if (target_worker == SIZE_MAX) {
            target_worker = select_target_worker();
        }

        // Create a task that wraps the coroutine resumption
        // This allows the worker thread to execute the continuation
        auto resume_task = make_task(
            [coro = request.continuation]() {
                // Resume the coroutine on the worker thread
                if (coro) {
                    coro.resume();
                }
                return 0;  // Dummy return value
            },
            "IO_Continuation");

        // Create task item
        TaskItem completion_item;
        completion_item.task = resume_task;
        completion_item.input = std::make_shared<std::any>(0);

        // Route completion to worker
        route_completion_to_worker(io_thread_id, target_worker,
                                   completion_item);
    }
}

void IOExecutor::route_completion_to_worker(std::size_t io_thread_id,
                                            std::size_t target_worker_id,
                                            const TaskItem& item) {
    // Validate worker ID
    if (target_worker_id >= num_workers_) {
        target_worker_id = select_target_worker();
    }

    // Try fast path first (SPSC queue: this I/O thread -> target worker)
    // Note: The fast_path_queues_ matrix is [worker_id][io_thread_id]
    if (fast_path_queues_[target_worker_id][io_thread_id]->queue.try_enqueue(
            item)) {
        fast_path_enqueues_.fetch_add(1);
        return;
    }

    // Fast path full, fall back to slow path (MPMC queue)
    slow_path_queue_->enqueue(item);
    slow_path_fallbacks_.fetch_add(1);
}

std::size_t IOExecutor::select_target_worker() {
    return next_target_worker_.fetch_add(1) % num_workers_;
}

bool IOExecutor::has_fast_path_work(std::size_t worker_id) const {
    if (worker_id >= num_workers_) {
        return false;
    }

    // Check if any I/O thread has enqueued work for this worker
    for (std::size_t i = 0; i < num_io_threads_; ++i) {
        if (fast_path_queues_[worker_id][i]->queue.size_approx() > 0) {
            return true;
        }
    }
    return false;
}

}  // namespace dftracer::utils
