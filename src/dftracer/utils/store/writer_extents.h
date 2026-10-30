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
    std::uint64_t quota = 0;
    std::uint64_t held = 0;
    std::uint64_t peak_held = 0;
    std::uint64_t busy = 0;
    std::uint64_t over_quota = 0;
    /// Bytes release() was asked to give back beyond what was held. Always
    /// zero unless the same bytes are released twice.
    std::uint64_t over_released = 0;
    std::uint64_t reservation = 0;
    std::uint64_t reserved = 0;
};

/// The segment bytes one writer holds, charged as a reservation of `pool`.
/// take() refuses with Busy while earlier bytes are not written, except when
/// nothing is held. A writer that sees a failed extent write must latch(),
/// since discarded bytes are never released. Thread safe. Waiters resume on
/// their executor.
class WriterExtents {
   public:
    enum class Take : std::uint8_t {
        Taken,           ///< charged, and the reservation holds them
        TakenOverQuota,  ///< charged past the quota; the reservation may not
                         ///< hold them
        Busy,            ///< nothing charged: earlier bytes are not written yet
    };

    /// @p quota_bytes is the most take() charges while anything is held. It
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

    /// Suspends the calling coroutine until the bytes it waits for are written
    /// (a release after @p epoch, nothing held, or latched). It reports no
    /// result, so retry the commit. Await it on the writer's executor.
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

    SpaceWait wait_release(std::uint64_t epoch) noexcept {
        return SpaceWait(*this, epoch);
    }

    /**
     * @brief An announced wait, ended when it is destroyed. Create it before
     * the waiter seals, so an appender sees it or the seal sees the appender's
     * bytes.
     */
    class WaitAnnounce {
       public:
        explicit WaitAnnounce(WriterExtents& extents) noexcept;
        WaitAnnounce(const WaitAnnounce&) = delete;
        WaitAnnounce& operator=(const WaitAnnounce&) = delete;
        ~WaitAnnounce();

        std::uint64_t epoch() const noexcept { return epoch_; }

       private:
        WriterExtents* extents_;
        std::uint64_t epoch_ = 0;
    };

    bool waiting() const noexcept;

    WriterExtentsCounters counters() const;

   private:
    struct Waiter {
        std::coroutine_handle<> handle;
        Executor* executor = nullptr;
    };

    /// The caller holds the mutex.
    bool ready_locked(std::uint64_t epoch) const noexcept;
    bool grow(std::uint64_t bytes) noexcept;
    void grow_forced(std::uint64_t bytes) noexcept;
    void charge(std::uint64_t bytes) noexcept;
    void wake(dstd::Vector<Waiter>& waiters) noexcept;

    mutable std::mutex mutex_;
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
