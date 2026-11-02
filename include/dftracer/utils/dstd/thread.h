// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_THREAD_H
#define DFTRACER_UTILS_DSTD_THREAD_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <pthread.h>

#include <type_traits>
#include <utility>

namespace dftracer::utils::dstd {

/**
 * @brief A thread on pthread_create whose start block comes from
 * memory::allocate, so it counts in the memory budget.
 *
 * The destructor joins a thread that is still joinable, so a Thread never
 * ends the process the way a joinable std::thread does. A thread that cannot
 * be created ends the process with a message, since no caller can go on
 * without it.
 */
class Thread {
   public:
    /// Names a thread for comparison, as std::thread::id does.
    class Id {
       public:
        Id() noexcept = default;
        bool operator==(const Id& other) const noexcept;
        bool operator!=(const Id& other) const noexcept {
            return !(*this == other);
        }

       private:
        friend class Thread;
        explicit Id(pthread_t handle) noexcept : handle_(handle), set_(true) {}
        pthread_t handle_{};
        bool set_{false};
    };

    Thread() noexcept = default;

    /// Runs `fn()` on a new thread. A non-empty `name` is the name a debugger
    /// and `top -H` show, cut to 15 bytes on Linux.
    template <class Fn>
        requires std::is_invocable_v<std::decay_t<Fn>&>
    explicit Thread(Fn&& fn, StringView name = {}) {
        using Body = std::decay_t<Fn>;
        auto start = dstd::make_unique<Start<Body>>(std::forward<Fn>(fn), name);
        launch(&Start<Body>::run, start.get());
        (void)start.release();
    }

    Thread(Thread&& other) noexcept
        : handle_(other.handle_), joinable_(other.joinable_) {
        other.joinable_ = false;
    }
    /// Joins the thread this one held before taking `other`.
    Thread& operator=(Thread&& other) noexcept;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    ~Thread() { join(); }

    bool joinable() const noexcept { return joinable_; }
    /// Waits for the thread to end. Does nothing when not joinable.
    void join() noexcept;
    /// Lets the thread run on alone. Its body must not use anything that ends
    /// before the process does.
    void detach() noexcept;

    Id id() const noexcept { return joinable_ ? Id(handle_) : Id(); }
    static Id current_id() noexcept { return Id(pthread_self()); }

   private:
    template <class Body>
    struct Start {
        Body body;
        String name;
        Start(Body&& b, StringView n) : body(std::move(b)), name(n) {}
        Start(const Body& b, StringView n) : body(b), name(n) {}

        static void run(void* raw) {
            UniquePtr<Start> self(static_cast<Start*>(raw));
            if (!self->name.empty()) set_current_name(self->name);
            self->body();
        }
    };

    static void set_current_name(const String& name) noexcept;
    void launch(void (*run)(void*), void* arg);

    pthread_t handle_{};
    bool joinable_{false};
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_THREAD_H
