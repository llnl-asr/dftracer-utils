#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_RESULT_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_RESULT_H

#include <dftracer/utils/core/common/typedefs.h>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <utility>

namespace dftracer::utils {

class TaskContext;
class ExecutorContext;
template <typename O>
class TaskResult {
   public:
    TaskResult() : id_(-1), future_(), context_(nullptr) {}
    TaskResult(const TaskResult&) = delete;
    TaskResult& operator=(const TaskResult&) = delete;

    using Status = std::future_status;
    using Future = std::future<O>;
    using SharedFuture = std::shared_future<O>;

    TaskResult(TaskResult&& other)
        : id_(other.id_),
          future_(std::move(other.future_)),
          context_(other.context_) {
        other.context_ = nullptr;
    }
    TaskResult& operator=(TaskResult&& other) {
        if (this != &other) {
            if (context_) context_->release_user_ref(id_);
            id_ = other.id_;
            future_ = std::move(other.future_);
            context_ = other.context_;
            other.context_ = nullptr;
        }
        return *this;
    }

    ~TaskResult() {
        if (context_) context_->release_user_ref(id_);
    }

    O get() {
        auto result = future_.get();
        if (context_) {
            context_->release_user_ref(id_);
            context_ = nullptr;
        }
        return result;
    }

    inline void wait() { future_.wait(); }
    template <class Clock, class Duration>
    inline Status wait_until(
        const std::chrono::duration<Clock, Duration>& timeout_time) {
        return future_.wait_until(timeout_time);
    }
    template <class Rep, class Period>
    inline Status wait_for(
        const std::chrono::duration<Rep, Period>& timeout_duration) {
        return future_.wait_for(timeout_duration);
    }
    inline TaskIndex id() const { return id_; }
    inline Future& future() { return future_; }
    inline const Future& future() const { return future_; }
    inline SharedFuture share() { return future_.share(); }
    inline bool valid() const { return future_.valid(); }
    inline bool is_ready() const {
        return wait_for(std::chrono::seconds(0)) == Status::ready;
    }

   private:
    TaskIndex id_;
    Future future_;
    ExecutorContext* context_;

    TaskResult(TaskIndex id, Future future, ExecutorContext* ctx)
        : id_(id), future_(std::move(future)), context_(ctx) {
        if (context_) context_->increment_user_ref(id_);
    }

    TaskResult(TaskIndex task_id, std::future<O> future)
        : id_(task_id), future_(std::move(future)), context_(nullptr) {}

    friend class Pipeline;
    friend class TaskContext;
};
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_RESULT_H
