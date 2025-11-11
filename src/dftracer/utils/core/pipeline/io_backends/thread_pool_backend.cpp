#include <dftracer/utils/core/pipeline/io_backends/thread_pool_backend.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <coroutine>
#include <thread>

namespace dftracer::utils {

ThreadPoolIOBackend::ThreadPoolIOBackend() {
    // Start worker threads
    for (std::size_t i = 0; i < DEFAULT_THREAD_POOL_SIZE; ++i) {
        worker_threads_.emplace_back(&ThreadPoolIOBackend::worker_loop, this);
    }
}

ThreadPoolIOBackend::~ThreadPoolIOBackend() {
    // Signal shutdown
    shutdown_requested_ = true;

    // Join all threads
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void ThreadPoolIOBackend::worker_loop() {
    while (!shutdown_requested_) {
        std::function<void()> task;

        // Try to dequeue a task (non-blocking)
        if (task_queue_.try_dequeue(task)) {
            if (task) {
                task();
            }
        } else {
            // No task available, brief sleep to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    // Process remaining tasks before exiting
    std::function<void()> task;
    while (task_queue_.try_dequeue(task)) {
        if (task) {
            task();
        }
    }
}

void ThreadPoolIOBackend::enqueue_task(std::function<void()> task) {
    task_queue_.enqueue(std::move(task));
}

void ThreadPoolIOBackend::complete_operation(std::uint64_t op_id,
                                             std::vector<char> data,
                                             int error) {
    ThreadPoolPendingOperation pending_op;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_ops_.find(op_id);
        if (it == pending_ops_.end()) {
            return;  // Operation cancelled or unknown
        }
        pending_op = std::move(it->second);
        pending_ops_.erase(it);
    }

    // Add to completed queue
    completed_ops_.enqueue(
        IOCompletion(pending_op.coro_handle, std::move(data), error, op_id));
}

std::uint64_t ThreadPoolIOBackend::submit_read(int fd, std::size_t offset,
                                               std::size_t size,
                                               std::coroutine_handle<> handle) {
    std::uint64_t op_id = next_op_id_.fetch_add(1);

    // Store pending operation
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_ops_.emplace(op_id, ThreadPoolPendingOperation(handle, op_id));
    }

    // Enqueue I/O task
    enqueue_task([this, fd, offset, size, op_id]() {
        std::vector<char> buffer(size);
        ssize_t bytes_read = pread(fd, buffer.data(), size, offset);

        int error_code = 0;
        if (bytes_read < 0) {
            error_code = errno;
            buffer.clear();
        } else {
            buffer.resize(bytes_read);
        }

        complete_operation(op_id, std::move(buffer), error_code);
    });

    return op_id;
}

std::uint64_t ThreadPoolIOBackend::submit_write(
    int fd, std::size_t offset, const std::vector<char>& data,
    std::coroutine_handle<> handle) {
    std::uint64_t op_id = next_op_id_.fetch_add(1);

    // Store pending operation
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_ops_.emplace(op_id, ThreadPoolPendingOperation(handle, op_id));
    }

    // Copy data for async write
    std::vector<char> data_copy = data;

    // Enqueue I/O task
    enqueue_task([this, fd, offset, data_copy = std::move(data_copy), op_id]() {
        ssize_t bytes_written =
            pwrite(fd, data_copy.data(), data_copy.size(), offset);

        int error_code = 0;
        if (bytes_written < 0) {
            error_code = errno;
        }

        complete_operation(op_id, {}, error_code);
    });

    return op_id;
}

std::vector<IOCompletion> ThreadPoolIOBackend::wait_for_completions(
    std::size_t max_batch, std::chrono::milliseconds timeout) {
    std::vector<IOCompletion> completions;
    completions.reserve(max_batch);

    auto start_time = std::chrono::steady_clock::now();

    while (completions.size() < max_batch) {
        IOCompletion completion;

        // Try to dequeue completed operations (non-blocking)
        while (completions.size() < max_batch &&
               completed_ops_.try_dequeue(completion)) {
            completions.push_back(std::move(completion));
        }

        // If we got some completions or timeout expired, return
        if (!completions.empty()) {
            break;
        }

        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= timeout) {
            break;
        }

        // Brief sleep to avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return completions;
}

bool ThreadPoolIOBackend::cancel_operation(std::uint64_t operation_id) {
    std::lock_guard<std::mutex> lock(pending_mutex_);

    auto it = pending_ops_.find(operation_id);
    if (it == pending_ops_.end()) {
        return false;  // Operation not found or already completed
    }

    // Remove from pending operations
    // Note: We cannot actually cancel the I/O if it's already queued/running
    pending_ops_.erase(it);

    return true;
}

std::size_t ThreadPoolIOBackend::get_pending_count() const {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    return pending_ops_.size();
}

bool ThreadPoolIOBackend::is_available() const { return true; }

const char* ThreadPoolIOBackend::name() const { return "thread_pool"; }

}  // namespace dftracer::utils
