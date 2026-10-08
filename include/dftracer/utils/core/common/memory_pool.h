#ifndef DFTRACER_UTILS_CORE_COMMON_MEMORY_POOL_H
#define DFTRACER_UTILS_CORE_COMMON_MEMORY_POOL_H

#include <atomic>
#include <cstdint>
#include <utility>

namespace dftracer::utils {

/// A byte budget that consumers charge for memory they really hold. A reserve
/// that does not fit fails, it is never clamped or queued: the consumer spills
/// or reports the shortfall, so a request above the capacity is never admitted.
/// Thread-safe. Must outlive every Reservation drawn from it.
class MemoryPool {
   public:
    explicit MemoryPool(std::uint64_t capacity) noexcept
        : capacity_(capacity) {}
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    std::uint64_t capacity() const noexcept { return capacity_; }
    std::uint64_t used() const noexcept {
        return used_.load(std::memory_order_relaxed);
    }

    bool try_reserve(std::uint64_t n) noexcept {
        std::uint64_t cur = used_.load(std::memory_order_relaxed);
        do {
            if (n > capacity_ - cur) return false;
        } while (!used_.compare_exchange_weak(cur, cur + n,
                                              std::memory_order_relaxed));
        return true;
    }

    void release(std::uint64_t n) noexcept {
        used_.fetch_sub(n, std::memory_order_relaxed);
    }

   private:
    const std::uint64_t capacity_;
    std::atomic<std::uint64_t> used_{0};
};

/// Bytes held from a MemoryPool, returned on destruction or shrink.
class Reservation {
   public:
    Reservation() noexcept = default;
    explicit Reservation(MemoryPool& pool) noexcept : pool_(&pool) {}
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    Reservation(Reservation&& o) noexcept
        : pool_(o.pool_), bytes_(std::exchange(o.bytes_, 0)) {}
    Reservation& operator=(Reservation&& o) noexcept {
        if (this != &o) {
            reset();
            pool_ = o.pool_;
            bytes_ = std::exchange(o.bytes_, 0);
        }
        return *this;
    }
    ~Reservation() { reset(); }

    std::uint64_t bytes() const noexcept { return bytes_; }

    bool try_grow(std::uint64_t n) noexcept {
        if (!pool_ || !pool_->try_reserve(n)) return false;
        bytes_ += n;
        return true;
    }

    void shrink(std::uint64_t n) noexcept {
        if (n > bytes_) n = bytes_;
        pool_->release(n);
        bytes_ -= n;
    }

    void reset() noexcept {
        if (pool_ && bytes_) pool_->release(bytes_);
        bytes_ = 0;
    }

   private:
    MemoryPool* pool_ = nullptr;
    std::uint64_t bytes_ = 0;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_MEMORY_POOL_H
