#ifndef DFTRACER_UTILS_CORE_SQLITE_ASYNC_H
#define DFTRACER_UTILS_CORE_SQLITE_ASYNC_H

#include <coroutine>
#include <functional>
#include <utility>

namespace dftracer::utils::io {
class IoThreadPool;
}  // namespace dftracer::utils::io

namespace dftracer::utils::sqlite {

// Returns the sqlite IoThreadPool from Executor::current(), or nullptr.
// Defined in async.cpp to avoid Executor header dependency.
io::IoThreadPool *get_sqlite_pool();

// Non-template helper — submits work to the pool.
// Defined in async.cpp where IoThreadPool is visible.
void sqlite_async_submit(io::IoThreadPool *pool, std::function<void()> fn);

// Resumes the coroutine on the executor (if available) or inline.
// Defined in async.cpp to avoid Executor header dependency.
void sqlite_async_resume(std::coroutine_handle<> h);

template <typename T>
class SqliteAwaitable {
    io::IoThreadPool *pool_;
    std::function<T()> fn_;
    T result_{};
    std::coroutine_handle<> handle_;

   public:
    SqliteAwaitable(io::IoThreadPool *pool, std::function<T()> fn)
        : pool_(pool), fn_(std::move(fn)) {}

    bool await_ready() noexcept {
        if (pool_ == nullptr) {
            result_ = fn_();
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        handle_ = h;
        auto *self = this;
        sqlite_async_submit(pool_, [self] {
            self->result_ = self->fn_();
            sqlite_async_resume(self->handle_);
        });
    }

    T await_resume() { return std::move(result_); }
};

template <>
class SqliteAwaitable<void> {
    io::IoThreadPool *pool_;
    std::function<void()> fn_;
    std::coroutine_handle<> handle_;

   public:
    SqliteAwaitable(io::IoThreadPool *pool, std::function<void()> fn)
        : pool_(pool), fn_(std::move(fn)) {}

    bool await_ready() noexcept {
        if (pool_ == nullptr) {
            fn_();
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        handle_ = h;
        auto *self = this;
        sqlite_async_submit(pool_, [self] {
            self->fn_();
            sqlite_async_resume(self->handle_);
        });
    }

    void await_resume() {}
};

// Free function — offload arbitrary work to the sqlite thread pool.
// Use when you don't have a SqliteDatabase instance yet (e.g. the
// lambda creates its own database internally).
// Returns the pool-backed awaitable, or runs fn inline when no pool.
template <typename F>
auto run(F &&fn) -> SqliteAwaitable<decltype(fn())> {
    using R = decltype(fn());
    auto *pool = get_sqlite_pool();
    return SqliteAwaitable<R>(pool, std::forward<F>(fn));
}

}  // namespace dftracer::utils::sqlite

#endif  // DFTRACER_UTILS_CORE_SQLITE_ASYNC_H
