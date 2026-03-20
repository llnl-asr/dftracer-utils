#ifndef DFTRACER_UTILS_CORE_TASK_HANDLE_H
#define DFTRACER_UTILS_CORE_TASK_HANDLE_H

#include <dftracer/utils/core/common/typedefs.h>

#include <chrono>
#include <future>
#include <string>

namespace dftracer::utils {

/// Handle to a submitted void task. Non-blocking by default.
/// Call .wait() to block (re-raises on error) or .done() to check.
struct TaskHandle {
    std::shared_future<void> future;
    TaskIndex id{-1};
    std::string name;

    void get() { future.get(); }
    void wait() { future.get(); }
    bool done() const {
        return future.wait_for(std::chrono::seconds(0)) ==
               std::future_status::ready;
    }
};

/// Typed handle that can return a value via .get().
/// .wait() re-raises stored exceptions (same as .get() but discards value).
template <typename T>
struct TypedTaskHandle {
    std::shared_future<T> future;
    TaskIndex id{-1};
    std::string name;

    T get() { return future.get(); }
    void wait() { static_cast<void>(future.get()); }
    bool done() const {
        return future.wait_for(std::chrono::seconds(0)) ==
               std::future_status::ready;
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASK_HANDLE_H
