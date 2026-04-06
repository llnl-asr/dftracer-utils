#ifndef DFTRACER_UTILS_CORE_ROCKSDB_ASYNC_H
#define DFTRACER_UTILS_CORE_ROCKSDB_ASYNC_H

#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace dftracer::utils::io {
class IoThreadPool;
}  // namespace dftracer::utils::io

namespace dftracer::utils::rocksdb {

io::IoThreadPool* get_db_pool();
void db_async_submit(io::IoThreadPool* pool, std::function<void()> fn);
void db_async_resume_on(void* executor, std::coroutine_handle<> h);
void* get_current_executor_opaque();

template <typename T>
class DbAwaitable {
    io::IoThreadPool* pool_;
    void* executor_;
    std::function<T()> fn_;
    std::optional<T> result_;
    std::exception_ptr error_;
    std::coroutine_handle<> handle_;

   public:
    DbAwaitable(io::IoThreadPool* pool, void* executor, std::function<T()> fn)
        : pool_(pool), executor_(executor), fn_(std::move(fn)) {}

    bool await_ready() noexcept {
        if (pool_ == nullptr) {
            try {
                auto fn = std::move(fn_);
                fn_ = {};
                result_.emplace(fn());
            } catch (...) {
                error_ = std::current_exception();
            }
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        handle_ = h;
        auto* self = this;
        db_async_submit(pool_, [self] {
            try {
                auto fn = std::move(self->fn_);
                self->fn_ = {};
                self->result_.emplace(fn());
            } catch (...) {
                self->error_ = std::current_exception();
            }
            db_async_resume_on(self->executor_, self->handle_);
        });
    }

    T await_resume() {
        if (error_ != nullptr) {
            std::rethrow_exception(error_);
        }
        return std::move(*result_);
    }
};

template <>
class DbAwaitable<void> {
    io::IoThreadPool* pool_;
    void* executor_;
    std::function<void()> fn_;
    std::exception_ptr error_;
    std::coroutine_handle<> handle_;

   public:
    DbAwaitable(io::IoThreadPool* pool, void* executor,
                std::function<void()> fn)
        : pool_(pool), executor_(executor), fn_(std::move(fn)) {}

    bool await_ready() noexcept {
        if (pool_ == nullptr) {
            try {
                auto fn = std::move(fn_);
                fn_ = {};
                fn();
            } catch (...) {
                error_ = std::current_exception();
            }
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        handle_ = h;
        auto* self = this;
        db_async_submit(pool_, [self] {
            try {
                auto fn = std::move(self->fn_);
                self->fn_ = {};
                fn();
            } catch (...) {
                self->error_ = std::current_exception();
            }
            db_async_resume_on(self->executor_, self->handle_);
        });
    }

    void await_resume() {
        if (error_ != nullptr) {
            std::rethrow_exception(error_);
        }
    }
};

template <typename F>
auto run(F&& fn) -> DbAwaitable<decltype(fn())> {
    using R = decltype(fn());
    auto* pool = get_db_pool();
    auto* executor = get_current_executor_opaque();
    return DbAwaitable<R>(pool, executor, std::forward<F>(fn));
}

}  // namespace dftracer::utils::rocksdb

#endif  // DFTRACER_UTILS_CORE_ROCKSDB_ASYNC_H
