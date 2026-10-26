#ifndef DFTRACER_UTILS_STORE_WRITER_EXTENTS_H
#define DFTRACER_UTILS_STORE_WRITER_EXTENTS_H

#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/manager.h>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <mutex>

namespace dftracer::utils {

class Executor;

}  // namespace dftracer::utils

namespace dftracer::utils::store {

/// What a writer holds now, the most it ever held, and how its takes went.
struct WriterExtentsCounters {
    /// The bytes a take() charges while anything is held.
    std::uint64_t quota = 0;
    /// The bytes of the segments appended and not yet written.
    std::uint64_t held = 0;
    /// The most `held` ever was.
    std::uint64_t peak_held = 0;
    /// How often take() refused because bytes were still held.
    std::uint64_t busy = 0;
    /// How often take() charged bytes past the quota.
    std::uint64_t over_quota = 0;
    /// Bytes release() was asked to give back beyond what was held; always
    /// zero unless the same bytes are released twice.
    std::uint64_t over_released = 0;
    /// The bytes the reservation holds now.
    std::uint64_t reservation = 0;
    /// The bytes the pool holds of them now.
    std::uint64_t reserved = 0;
};

/**
 * @brief The segment bytes one writer holds: appended to an extent and not
 * written yet, charged as a reservation of `pool`.
 *
 * take() refuses with Busy while the bytes of an earlier commit are not
 * written, so the bytes of the extents in flight are the back pressure of the
 * writer. A segment that does not fit the quota is still taken when nothing is
 * held, because then no write can free room, and reported as TakenOverQuota so
 * the caller knows the quota did not bound it. `held` falls only in release(),
 * which runs as an extent write completes, after the writer sealed the extent
 * it was in: a commit that returns Busy seals the open extents first, so the
 * bytes it waits for are in flight and not sitting in an extent nobody writes.
 *
 * Bytes a failed extent write discarded are never released, so a writer that
 * sees such an error must latch() it: every wait ends at the latch.
 *
 * Thread safe. A waiter is resumed through the executor it parked with, never
 * on the thread that released the bytes.
 */
class WriterExtents {
   public:
    /// What take() did with the bytes.
    enum class Take : std::uint8_t {
        Taken,           ///< charged, and the reservation holds them
        TakenOverQuota,  ///< charged past the quota; the reservation may not
                         ///< hold them
        Busy,            ///< nothing charged: earlier bytes are not written yet
    };

    /// @p quota_bytes is the most take() charges while anything is held; it
    /// must not be zero.
    WriterExtents(std::uint64_t quota_bytes, memory::Pool& pool) noexcept;
    ~WriterExtents();
    WriterExtents(const WriterExtents&) = delete;
    WriterExtents& operator=(const WriterExtents&) = delete;
    WriterExtents(WriterExtents&&) = delete;
    WriterExtents& operator=(WriterExtents&&) = delete;

    /// Charges @p bytes, or refuses them. Never blocks and never waits for
    /// the pool: a refused reservation past the quota is taken anyway.
    Take take(std::uint64_t bytes);
    /// Lowers `held` by @p bytes, up to what it holds, and resumes every
    /// waiter of this count. Runs on the thread that completed the write.
    void release(std::uint64_t bytes) noexcept;
    /// Ends every wait of this count at once, now and later. Runs from any
    /// thread that observed an error the writer cannot recover from.
    void latch() noexcept;
    /// Counts as a release of no bytes: moves the epoch and resumes every
    /// waiter, so a waiter that saw a write in flight and did not seal looks
    /// again once that write is done.
    void nudge() noexcept;
    bool latched() const noexcept {
        return latched_.load(std::memory_order_acquire);
    }

    /**
     * @brief Suspends the calling coroutine until the bytes it waits for are
     * written.
     *
     * Ready once a release happened after @p epoch, nothing is held, or the
     * count is latched. It reports no result: the caller retries the commit
     * that was Busy, or finds the error there. Await it from a thread of the
     * writer's executor, whose resumption is scheduled, never run inline.
     */
    class SpaceWait {
       public:
        SpaceWait(WriterExtents& extents, std::uint64_t epoch) noexcept
            : extents_(&extents), epoch_(epoch) {}
        SpaceWait(const SpaceWait&) = delete;
        SpaceWait& operator=(const SpaceWait&) = delete;
        /// Drops the parked handle when the waiting coroutine is destroyed
        /// before a release resumed it, so no release resumes a dead frame.
        ~SpaceWait();

        bool await_ready() const;
        bool await_suspend(std::coroutine_handle<> handle);
        void await_resume() const noexcept {}

       private:
        WriterExtents* extents_;
        std::uint64_t epoch_;
        std::coroutine_handle<> parked_;
    };

    /// A wait for a release after @p epoch, from WaitAnnounce::epoch().
    SpaceWait wait_release(std::uint64_t epoch) noexcept {
        return SpaceWait(*this, epoch);
    }

    /**
     * @brief An announced wait, ended when it is destroyed.
     *
     * Create it before the waiter seals the open extents. An appender reads
     * waiting() after its append and then seals the open extents: either it
     * sees the announce, or the waiter's seal sees its bytes, so no byte a
     * waiter needs stays in an open extent nobody writes.
     */
    class WaitAnnounce {
       public:
        explicit WaitAnnounce(WriterExtents& extents) noexcept;
        WaitAnnounce(const WaitAnnounce&) = delete;
        WaitAnnounce& operator=(const WaitAnnounce&) = delete;
        ~WaitAnnounce();

        /// The release count when the wait was announced.
        std::uint64_t epoch() const noexcept { return epoch_; }

       private:
        WriterExtents* extents_;
        std::uint64_t epoch_ = 0;
    };

    /// True while a wait is announced.
    bool waiting() const noexcept;

    WriterExtentsCounters counters() const;

   private:
    /// A parked wait: the coroutine and the executor it must resume through.
    struct Waiter {
        std::coroutine_handle<> handle;
        Executor* executor = nullptr;
    };

    /// True when a wait for @p epoch must not park; the caller holds the mutex.
    bool ready_locked(std::uint64_t epoch) const noexcept;
    /// Charges @p bytes in the reservation, false when the pool refused.
    bool grow(std::uint64_t bytes) noexcept;
    /// Asks the pool for @p bytes anyway, so a manager under pressure sees it.
    void grow_forced(std::uint64_t bytes) noexcept;
    void charge(std::uint64_t bytes) noexcept;
    void wake(dstd::Vector<Waiter>& waiters) noexcept;

    mutable std::mutex mutex_;
    /// The bytes charged; taken apart by release().
    memory::Reservation reservation_;
    dstd::Vector<Waiter> waiters_;
    memory::Pool* pool_ = nullptr;
    std::uint64_t quota_ = 0;
    std::uint64_t held_ = 0;
    std::uint64_t epoch_ = 0;
    std::uint64_t peak_held_ = 0;
    std::uint64_t busy_count_ = 0;
    std::uint64_t over_quota_count_ = 0;
    std::uint64_t over_released_ = 0;
    std::atomic<bool> latched_{false};
    std::atomic<std::uint64_t> waits_{0};
};

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_WRITER_EXTENTS_H
