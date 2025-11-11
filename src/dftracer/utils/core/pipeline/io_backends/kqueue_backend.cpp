#ifdef __APPLE__

#include <dftracer/utils/core/pipeline/io_backends/kqueue_backend.h>
#include <sys/event.h>
#include <unistd.h>

#include <cerrno>
#include <coroutine>
#include <cstring>
#include <stdexcept>

namespace dftracer::utils {

KqueuePendingOperation::KqueuePendingOperation(std::coroutine_handle<> h,
                                               std::size_t s, std::size_t off,
                                               int file_fd, bool read,
                                               std::uint64_t id)
    : coro_handle(h),
      size(s),
      offset(off),
      fd(file_fd),
      is_read(read),
      operation_id(id) {
    if (is_read) {
        buffer.resize(size);
    }
}

KqueueBackend::KqueueBackend() {
    // Initialize kqueue
    kq_ = kqueue();
    if (kq_ < 0) {
        throw std::runtime_error("Failed to create kqueue: " +
                                 std::string(std::strerror(errno)));
    }
}

KqueueBackend::~KqueueBackend() {
    if (kq_ >= 0) {
        close(kq_);
        kq_ = -1;
    }
}

std::uint64_t KqueueBackend::submit_read(int fd, std::size_t offset,
                                         std::size_t size,
                                         std::coroutine_handle<> handle) {
    std::uint64_t op_id = next_op_id_.fetch_add(1);

    // Store pending operation
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_ops_.emplace(op_id, KqueuePendingOperation(handle, size, offset,
                                                           fd, true, op_id));
    }

    // Register read event with kqueue
    struct kevent kev;
    // Use operation ID as udata to match completions
    EV_SET(&kev, fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0,
           reinterpret_cast<void*>(op_id));

    if (kevent(kq_, &kev, 1, nullptr, 0, nullptr) < 0) {
        // Failed to register - remove pending op
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_ops_.erase(op_id);
        return 0;  // Return 0 to indicate failure
    }

    return op_id;
}

std::uint64_t KqueueBackend::submit_write(int fd, std::size_t offset,
                                          const std::vector<char>& data,
                                          std::coroutine_handle<> handle) {
    std::uint64_t op_id = next_op_id_.fetch_add(1);

    // Store pending operation
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto& pending_op = pending_ops_[op_id];
        pending_op = KqueuePendingOperation(handle, data.size(), offset, fd,
                                            false, op_id);
        pending_op.buffer = data;  // Copy data for async write
    }

    // Register write event with kqueue
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0,
           reinterpret_cast<void*>(op_id));

    if (kevent(kq_, &kev, 1, nullptr, 0, nullptr) < 0) {
        // Failed to register - remove pending op
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_ops_.erase(op_id);
        return 0;  // Return 0 to indicate failure
    }

    return op_id;
}

std::vector<IOCompletion> KqueueBackend::wait_for_completions(
    std::size_t max_batch, std::chrono::milliseconds timeout) {
    std::vector<IOCompletion> completions;
    completions.reserve(max_batch);

    // Convert timeout to timespec
    struct timespec ts;
    ts.tv_sec = timeout.count() / 1000;
    ts.tv_nsec = (timeout.count() % 1000) * 1000000;

    // Wait for events
    std::vector<struct kevent> events(max_batch);
    int n = kevent(kq_, nullptr, 0, events.data(), static_cast<int>(max_batch),
                   &ts);

    if (n < 0) {
        if (errno == EINTR) {
            // Interrupted by signal - return empty
            return completions;
        }
        // Other error - return empty
        return completions;
    }

    // Process events
    for (int i = 0; i < n; ++i) {
        auto& event = events[i];
        std::uint64_t op_id = reinterpret_cast<std::uint64_t>(event.udata);

        // Find pending operation
        KqueuePendingOperation pending_op;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_ops_.find(op_id);
            if (it == pending_ops_.end()) {
                continue;  // Unknown operation
            }
            pending_op = std::move(it->second);
            pending_ops_.erase(it);
        }

        int error_code = 0;

        // Check for errors
        if (event.flags & EV_ERROR) {
            error_code = static_cast<int>(event.data);
        } else {
            // Perform actual I/O now that file is ready
            if (pending_op.is_read) {
                // Read data
                ssize_t bytes_read =
                    pread(pending_op.fd, pending_op.buffer.data(),
                          pending_op.size, pending_op.offset);
                if (bytes_read < 0) {
                    error_code = errno;
                    pending_op.buffer.clear();
                } else {
                    pending_op.buffer.resize(bytes_read);
                }
            } else {
                // Write data
                ssize_t bytes_written =
                    pwrite(pending_op.fd, pending_op.buffer.data(),
                           pending_op.size, pending_op.offset);
                if (bytes_written < 0) {
                    error_code = errno;
                }
            }
        }

        // Create completion
        completions.emplace_back(pending_op.coro_handle,
                                 std::move(pending_op.buffer), error_code,
                                 op_id);
    }

    return completions;
}

bool KqueueBackend::cancel_operation(std::uint64_t operation_id) {
    std::lock_guard<std::mutex> lock(pending_mutex_);

    auto it = pending_ops_.find(operation_id);
    if (it == pending_ops_.end()) {
        return false;  // Operation not found
    }

    auto& pending_op = it->second;

    // Remove kqueue event
    struct kevent kev;
    int filter = pending_op.is_read ? EVFILT_READ : EVFILT_WRITE;
    EV_SET(&kev, pending_op.fd, filter, EV_DELETE, 0, 0, nullptr);
    kevent(kq_, &kev, 1, nullptr, 0, nullptr);

    // Remove from pending operations
    pending_ops_.erase(it);

    return true;
}

std::size_t KqueueBackend::get_pending_count() const {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    return pending_ops_.size();
}

bool KqueueBackend::is_available() const {
    // kqueue is always available on macOS/BSD
    return kq_ >= 0;
}

const char* KqueueBackend::name() const { return "kqueue"; }

}  // namespace dftracer::utils

#endif  // __APPLE__
