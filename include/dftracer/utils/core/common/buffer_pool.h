#ifndef DFTRACER_UTILS_CORE_COMMON_BUFFER_POOL_H
#define DFTRACER_UTILS_CORE_COMMON_BUFFER_POOL_H

#include <concurrentqueue.h>

#include <cstddef>
#include <memory>
#include <utility>

namespace dftracer::utils {

struct DefaultReset {
    template <typename T>
    void operator()(T& v) const {
        v.clear();
    }
};

struct NoOpReset {
    template <typename T>
    void operator()(T&) const {}
};

/**
 * @brief Thread-safe typed buffer pool. Zero allocations after warmup.
 *
 * @tparam T Buffer type. Must support move semantics.
 */
template <typename T>
class BufferPool {
   public:
    virtual ~BufferPool() = default;
    virtual T acquire() = 0;
    virtual void release(T buf) = 0;
};

template <typename T, typename Init, typename Reset = DefaultReset>
class BufferPoolImpl : public BufferPool<T> {
   public:
    BufferPoolImpl(std::size_t capacity, Init init, Reset reset = Reset{})
        : queue_(capacity), init_(std::move(init)), reset_(std::move(reset)) {
        for (std::size_t i = 0; i < capacity; ++i) {
            queue_.enqueue(init_());
        }
    }

    T acquire() override {
        T item;
        if (queue_.try_dequeue(item)) {
            return item;
        }
        return init_();
    }

    void release(T buf) override {
        reset_(buf);
        queue_.enqueue(std::move(buf));
    }

   private:
    moodycamel::ConcurrentQueue<T> queue_;
    Init init_;
    Reset reset_;
};

template <typename T, typename Init>
auto make_buffer_pool(std::size_t capacity, Init&& init) {
    using Pool = BufferPoolImpl<T, std::decay_t<Init>>;
    return std::make_shared<Pool>(capacity, std::forward<Init>(init));
}

template <typename T, typename Init, typename Reset>
auto make_buffer_pool(std::size_t capacity, Init&& init, Reset&& reset) {
    using Pool = BufferPoolImpl<T, std::decay_t<Init>, std::decay_t<Reset>>;
    return std::make_shared<Pool>(capacity, std::forward<Init>(init),
                                  std::forward<Reset>(reset));
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_BUFFER_POOL_H
