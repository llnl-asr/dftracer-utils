#ifndef DFTRACER_UTILS_INDEX_BUILD_PARSE_GATE_H
#define DFTRACER_UTILS_INDEX_BUILD_PARSE_GATE_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/memory/manager.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::build {

/// Lets the fold drivers of a build parse pieces of their files at once while
/// workers are free and memory is. Every parse takes a slot, one per worker;
/// a file takes one slot at least by force, so it always makes progress, and
/// more only while one is free. A piece beyond the first of a file also
/// reserves its bytes from the build's pool. Thread safe.
class ParseGate {
   public:
    /// With a null @p pool a piece needs no reservation.
    ParseGate(memory::Pool* pool, std::size_t workers) noexcept
        : pool_(pool),
          workers_(static_cast<std::int64_t>(workers)),
          free_(workers_) {}
    ParseGate(const ParseGate&) = delete;
    ParseGate& operator=(const ParseGate&) = delete;

    bool try_slot() noexcept {
        std::int64_t n = free_.load(std::memory_order_relaxed);
        while (n > 0)
            if (free_.compare_exchange_weak(n, n - 1,
                                            std::memory_order_acquire))
                return true;
        return false;
    }
    /// Takes a slot even when none is free.
    void take_slot() noexcept { free_.fetch_sub(1, std::memory_order_relaxed); }
    void give_slot() noexcept { free_.fetch_add(1, std::memory_order_release); }

    /// A file in flight keeps one worker, parsing or waiting on its reads.
    void file_in() noexcept { files_.fetch_add(1, std::memory_order_relaxed); }
    void file_out() noexcept { files_.fetch_sub(1, std::memory_order_relaxed); }

    /// Whether enough workers have no file for a file to parse more than one
    /// piece at a time.
    bool spare_workers() const noexcept {
        return (workers_ - files_.load(std::memory_order_relaxed)) *
                   static_cast<std::int64_t>(
                       constants::file_admission::SPAWN_FREE_DIVISOR) >=
               workers_;
    }

    static std::uint64_t piece_bytes(std::size_t plaintext) noexcept {
        return plaintext *
               constants::file_admission::PIECE_BYTES_PER_PLAINTEXT_BYTE;
    }

    /// Reserves @p bytes for a piece beyond the first of a file without
    /// waiting. False when the pool is full or as many extra pieces as workers
    /// are held already. The caller calls drop_piece() when it lets go.
    bool try_piece(std::uint64_t bytes, memory::Reservation& out) noexcept {
        std::int64_t n = extra_.load(std::memory_order_relaxed);
        do {
            if (n >= workers_) return false;
        } while (
            !extra_.compare_exchange_weak(n, n + 1, std::memory_order_acquire));
        if (pool_ != nullptr) {
            out = pool_->try_reserve(bytes);
            if (!out) {
                extra_.fetch_sub(1, std::memory_order_release);
                return false;
            }
        }
        return true;
    }
    void drop_piece() noexcept {
        extra_.fetch_sub(1, std::memory_order_release);
    }

   private:
    memory::Pool* pool_;
    std::int64_t workers_;
    std::atomic<std::int64_t> free_;
    std::atomic<std::int64_t> files_{0};
    std::atomic<std::int64_t> extra_{0};
};

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_PARSE_GATE_H
