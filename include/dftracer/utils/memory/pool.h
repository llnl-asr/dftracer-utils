// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_MEMORY_POOL_H
#define DFTRACER_UTILS_MEMORY_POOL_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dftracer::utils::memory {

class Pool;

namespace detail {
class ReserveWait;
class ReclaimSlot;
class Reclaimer;
class ReclaimGuard;
struct PoolAccess;
struct ManagerAccess;
struct PoolInternals;
}  // namespace detail

enum class PoolKind : std::uint8_t {
    /// Query state, arenas, hash tables. Closing it with live bytes or open
    /// children is a leak.
    Scratch,
    /// Results that outlive the query. Closing it leaves its live bytes in
    /// place and reports them as adopted by the parent.
    Results,
};

enum class Status : std::uint8_t {
    Ok,
    /// Holders were asked for bytes that are not free yet.
    Pending,
    OverBudget,
    OverCap,
    PoolTableFull,
    /// Every reclaimer or external slot is taken.
    SlotTableFull,
    /// The wait ended because the executor is shutting down. A retry cannot
    /// succeed.
    Cancelled,
};

struct Limits {
    /// Largest reserved total of the pool and the pools below it, rounded up
    /// to the reservation quantum. Zero is no limit.
    std::size_t bytes = 0;
};

/**
 * @brief A reservation of bytes in one pool, released on destruction.
 *
 * Empty when @ref Pool::try_reserve found no room. Then status() says why.
 * A reservation counts against the limit of its pool and of every pool above
 * it, and against the memory budget, but it allocates nothing. Moving transfers
 * the bytes. It must not outlive its pool. The object is not thread safe.
 */
class Reservation {
   public:
    Reservation() noexcept = default;
    Reservation(Reservation&& other) noexcept;
    Reservation& operator=(Reservation&& other) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation();

    explicit operator bool() const noexcept {
        return pool_ != nullptr && status_ == Status::Ok;
    }
    Status status() const noexcept { return status_; }
    std::size_t bytes() const noexcept { return bytes_; }
    /// Give the bytes back now. The reservation is empty afterwards.
    void release() noexcept;

   private:
    friend class Pool;
    friend struct detail::PoolAccess;
    friend class detail::ReserveWait;
    Reservation(Pool* pool, std::size_t bytes, Status status) noexcept
        : pool_(pool), bytes_(bytes), status_(status) {}

    Status grow(std::size_t bytes) noexcept;
    Status try_grow(std::size_t bytes) noexcept;
    std::size_t take_from(Reservation& other, std::size_t bytes) noexcept;

    Pool* pool_ = nullptr;
    std::size_t bytes_ = 0;
    Status status_ = Status::Ok;
};

/**
 * @brief Owns an open pool and closes it on destruction.
 *
 * Empty when @ref Pool::child failed. Then status() says why. Close each child
 * before its parent. Moving transfers the pool. The handle is not thread safe,
 * the pool it holds is.
 */
class PoolHandle {
   public:
    PoolHandle() noexcept = default;
    PoolHandle(PoolHandle&& other) noexcept;
    PoolHandle& operator=(PoolHandle&& other) noexcept;
    PoolHandle(const PoolHandle&) = delete;
    PoolHandle& operator=(const PoolHandle&) = delete;
    ~PoolHandle();

    explicit operator bool() const noexcept { return pool_ != nullptr; }
    Status status() const noexcept { return status_; }
    Pool* get() const noexcept { return pool_; }
    Pool& operator*() const noexcept { return *pool_; }
    Pool* operator->() const noexcept { return pool_; }
    void close() noexcept;

   private:
    friend class Pool;
    PoolHandle(Pool* pool, std::uint32_t generation, Status status) noexcept
        : pool_(pool), generation_(generation), status_(status) {}

    Pool* pool_ = nullptr;
    std::uint32_t generation_ = 0;
    Status status_ = Status::Ok;
};

/**
 * @brief A node of the pool tree: the process, a query, a task, an operator.
 *
 * The limit given to child() caps the reservations of the pool and of the
 * pools below it, not single allocations. Blocks are charged to the pool that
 * was the scope when they were allocated, or to the pool a @ref PoolAllocator
 * was built with. A Pool reference stays readable after the pool closes. The
 * counters are exact while no thread allocates or frees and a snapshot
 * otherwise. Every member is thread safe.
 */
class Pool {
   public:
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    /**
     * @brief Open a pool below this one.
     *
     * The name is cut at 47 bytes. @p limits caps the reserved total of the new
     * pool. The handle is empty with Status::PoolTableFull when all 65,535
     * slots are taken. Close the handle before the handle of this pool.
     */
    PoolHandle child(std::string_view name, PoolKind kind = PoolKind::Scratch,
                     Limits limits = {});

    /**
     * @brief Reserve @p bytes in this pool without waiting.
     *
     * The limit of a pool caps the reservations of the pool and of the pools
     * below it, so the call needs room in this pool and in every pool above it.
     * The result is empty, with a status that says why, when there is no room.
     * Allocations through a @ref PoolAllocator are counted in live() and
     * peak() but never refused, so reserve the bytes before the work that
     * allocates them. Thread safe. The reservation must not outlive the pool.
     */
    Reservation try_reserve(std::size_t bytes) noexcept;

    PoolKind kind() const noexcept { return kind_; }
    std::string_view name() const noexcept { return {name_, name_size_}; }
    /// Names from the root joined by '/', for example "root/query/scan".
    std::string path() const;
    /// The limit set at child(). Zero is no limit.
    std::size_t limit() const noexcept {
        return limit_.load(std::memory_order_relaxed);
    }

    /// Bytes and blocks held by this pool and every pool below it.
    std::size_t live() const;
    std::size_t blocks() const;
    /// Largest live() seen by a counter read. The heap does not sample it.
    std::size_t peak() const;
    /// Reserved bytes, with every pool below counted by its rounded amount.
    std::size_t reserved() const noexcept;

   private:
    friend struct detail::PoolAccess;
    friend struct detail::PoolInternals;
    friend struct detail::ManagerAccess;
    friend class PoolHandle;
    friend class Reservation;
    friend class detail::ReclaimGuard;
    friend class detail::ReclaimSlot;
    Pool() = default;

    Reservation reserve(std::size_t bytes) noexcept;
    void raise_limit(std::size_t bytes) noexcept;
    detail::ReclaimSlot attach(detail::Reclaimer& reclaimer);

    std::uint16_t id() const noexcept { return id_; }
    std::uint32_t generation() const noexcept {
        return generation_.load(std::memory_order_relaxed);
    }
    std::uint16_t parent() const noexcept { return parent_; }
    std::size_t adopted() const;
    std::size_t exported() const noexcept;
    void add_exported(std::size_t bytes) noexcept;
    void sub_exported(std::size_t bytes) noexcept;

    static constexpr std::size_t NAME_CAPACITY = 48;

    std::atomic<std::ptrdiff_t> live_{0};
    std::atomic<std::ptrdiff_t> blocks_{0};
    std::atomic<std::ptrdiff_t> reserved_{0};
    std::atomic<std::ptrdiff_t> charged_{0};
    std::atomic<std::ptrdiff_t> peak_{0};
    std::atomic<std::ptrdiff_t> exported_{0};
    std::atomic<std::size_t> limit_{0};
    std::atomic<std::uint32_t> generation_{0};
    std::atomic<std::uint32_t> state_{0};
    std::atomic<std::uint32_t> children_{0};
    std::atomic<std::uint32_t> open_children_{0};
    std::atomic<std::uint32_t> reclaiming_{0};
    std::atomic<std::uint16_t> next_free_{0};
    std::uint16_t id_ = 0;
    std::atomic<std::uint16_t> parent_{0};
    PoolKind kind_ = PoolKind::Scratch;
    std::uint8_t name_size_ = 0;
    char name_[NAME_CAPACITY] = {};
};

/**
 * @brief The root of the pool tree, the parent of every pool a user opens.
 *
 * Lives for the whole process. Blocks allocated with no scope go to its child
 * "general", not to the root itself.
 */
Pool& root_pool() noexcept;

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_POOL_H
