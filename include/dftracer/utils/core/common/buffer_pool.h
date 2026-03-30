#ifndef DFTRACER_UTILS_CORE_COMMON_BUFFER_POOL_H
#define DFTRACER_UTILS_CORE_COMMON_BUFFER_POOL_H

#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

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
 * Buffers are never dropped. Released buffers are always kept for reuse.
 * The init factory is only called when the pool is empty (during warmup
 * or under unexpected load).
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

/**
 * @brief Concrete buffer pool with typed Init and Reset callables.
 *
 * Init and Reset are stored by value to avoid std::function overhead.
 */
template <typename T, typename Init, typename Reset = DefaultReset>
class BufferPoolImpl : public BufferPool<T> {
   public:
    BufferPoolImpl(std::size_t capacity, Init init, Reset reset = Reset{})
        : init_(std::move(init)), reset_(std::move(reset)) {
        pool_.reserve(capacity);
        for (std::size_t i = 0; i < capacity; ++i) {
            pool_.push_back(init_());
        }
    }

    T acquire() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!pool_.empty()) {
            T item = std::move(pool_.back());
            pool_.pop_back();
            return item;
        }
        return init_();
    }

    void release(T buf) override {
        reset_(buf);
        std::lock_guard<std::mutex> lock(mu_);
        pool_.push_back(std::move(buf));
    }

   private:
    std::mutex mu_;
    std::vector<T> pool_;
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
