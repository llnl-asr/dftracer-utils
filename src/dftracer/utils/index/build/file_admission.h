#ifndef DFTRACER_UTILS_INDEX_BUILD_FILE_ADMISSION_H
#define DFTRACER_UTILS_INDEX_BUILD_FILE_ADMISSION_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/index/build/parse_gate.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/cancelled.h>
#include <dftracer/utils/runtime/coro/reserve_async.h>
#include <dftracer/utils/runtime/coro/task.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::index::build {

/// Admits the files of an index build or a view bootstrap by the bytes each
/// holds in flight, through reservations in a pool of its own, not by worker
/// count. A file that does not fit waits in the pool's queue, in order, and
/// runs once earlier files release their bytes. A file larger than the budget
/// holds all but 1 / PIECE_ROOM_DIVISOR of it, which runs it alone, and the
/// rest is room for its pieces.
///
/// Thread safe. It must outlive every Ticket and every driver that uses its
/// gate, and is destroyed on a thread that may close the pool it opened.
class FileAdmission {
   public:
    /// What a file in flight holds, its reservation released on destruction.
    class Ticket {
       public:
        Ticket() noexcept = default;
        Ticket(Ticket&& o) noexcept
            : owner_(std::exchange(o.owner_, nullptr)),
              res_(std::move(o.res_)),
              elastic_(o.elastic_) {}
        Ticket& operator=(Ticket&& o) noexcept {
            if (this != &o) {
                reset();
                owner_ = std::exchange(o.owner_, nullptr);
                res_ = std::move(o.res_);
                elastic_ = o.elastic_;
            }
            return *this;
        }
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        ~Ticket() { reset(); }

        /// Bytes of the reservation that the file's spillable state may use
        /// before it goes to disk; NO_SPILL_BUDGET when the build is unbounded.
        std::uint64_t elastic_bytes() const noexcept { return elastic_; }

       private:
        friend class FileAdmission;
        void reset() noexcept {
            if (owner_ != nullptr) owner_->left();
            owner_ = nullptr;
            res_.release();
        }

        FileAdmission* owner_ = nullptr;
        memory::Reservation res_;
        std::uint64_t elastic_ = memory::NO_SPILL_BUDGET;
    };

    /// @p budget is the resolved bytes the build may hold,
    /// memory::NO_SPILL_BUDGET for none; @p sink_bytes of it are set aside for
    /// the workers' sinks.
    FileAdmission(std::uint64_t budget, std::size_t workers,
                  std::uint64_t sink_bytes, std::size_t files)
        : workers_(std::max<std::size_t>(workers, 1)),
          files_(files),
          gate_(open(budget, sink_bytes), workers_) {}

    FileAdmission(const FileAdmission&) = delete;
    FileAdmission& operator=(const FileAdmission&) = delete;

    /// The workers that run and the bytes each sink holds before it commits,
    /// for a build of @p workers workers: a worker keeps a sink of up to
    /// @p max_sink bytes, and the sinks take a share of @p budget.
    struct SinkPlan {
        std::size_t workers;
        std::size_t sink_bytes;
    };
    static SinkPlan plan_sinks(std::uint64_t budget, std::size_t workers,
                               std::size_t max_sink) noexcept {
        namespace fa = constants::file_admission;
        workers = std::max<std::size_t>(workers, 1);
        if (budget == memory::NO_SPILL_BUDGET) return {workers, max_sink};
        const std::uint64_t share = budget / fa::SINK_BUDGET_DIVISOR;
        workers = static_cast<std::size_t>(
            std::clamp<std::uint64_t>(share / fa::MIN_SINK_BYTES, 1, workers));
        const std::uint64_t each =
            std::min<std::uint64_t>(max_sink, share / workers);
        return {workers, static_cast<std::size_t>(each)};
    }

    /// Bytes of reservation a file of @p plaintext bytes asks for, and how many
    /// of them are elastic.
    struct Estimate {
        std::uint64_t total;
        std::uint64_t elastic;
    };
    static Estimate estimate(
        std::uint64_t plaintext,
        std::uint64_t max_elastic = memory::NO_SPILL_BUDGET) noexcept {
        namespace fa = constants::file_admission;
        const std::uint64_t piece =
            ParseGate::piece_bytes(std::min<std::uint64_t>(
                plaintext, IndexFoldDriver::PARSE_PIECE_BYTES));
        const std::uint64_t elastic = std::max(
            std::min(plaintext * fa::ELASTIC_PERCENT / 100, max_elastic),
            fa::MIN_ELASTIC_BYTES);
        return {fa::FIXED_BYTES + piece + elastic, elastic};
    }

    /// Bytes a file inflates to, as far as admission can tell before it
    /// parses the file: @p known_plaintext when the index recorded it, else
    /// the @p compressed size scaled by what a trace file usually inflates to.
    static std::uint64_t plaintext_estimate(std::uint64_t known_plaintext,
                                            std::uint64_t compressed) noexcept {
        return known_plaintext != 0
                   ? known_plaintext
                   : compressed * constants::file_admission::
                                      PLAINTEXT_PER_COMPRESSED_BYTE;
    }

    /// Waits for room for a file of @p plaintext bytes, then returns its
    /// ticket. Throws a cancelled error when the executor shuts down first.
    coro::CoroTask<Ticket> admit(std::uint64_t plaintext) {
        Ticket ticket;
        if (!pool_) {
            enter();
            ticket.owner_ = this;
            co_return ticket;
        }
        const Estimate est = estimate(
            plaintext,
            pool_cap_ / constants::file_admission::ELASTIC_POOL_DIVISOR);
        const std::uint64_t want = std::min(est.total, hold_cap_);
        auto got = co_await coro::reserve_async(
            *pool_, want, std::chrono::steady_clock::time_point::max());
        if (!got) {
            if (got.error() == memory::Status::Cancelled)
                coro::throw_cancelled("file admission cancelled");
            throw DFTUtilsException(ErrorCode::MemoryBudgetExceeded,
                                    "a file does not fit the memory budget");
        }
        ticket.res_ = std::move(*got);
        const std::uint64_t base = est.total - est.elastic;
        ticket.elastic_ =
            est.total <= want ? est.elastic : want - std::min(base, want / 2);
        ticket.owner_ = this;
        enter();
        co_return ticket;
    }

    ParseGate& gate() noexcept { return gate_; }

    /// Inflate workers for a file that starts now: one per
    /// INFLATE_PARSERS_PER_WORKER workers that no file in flight and no file
    /// still to start can use, and one while every worker has a file to take.
    std::size_t inflate_ways() const noexcept {
        const std::size_t started = started_.load(std::memory_order_relaxed);
        const std::size_t claimed =
            std::max<std::size_t>(in_flight_.load(std::memory_order_relaxed),
                                  1) +
            (files_ > started ? files_ - started : 0);
        const std::size_t idle = workers_ > claimed ? workers_ - claimed : 0;
        return std::clamp<std::size_t>(
            1 + idle / constants::file_admission::INFLATE_PARSERS_PER_WORKER, 1,
            MAX_INFLATE_WAYS);
    }

    std::size_t max_in_flight() const noexcept {
        return max_in_flight_.load(std::memory_order_relaxed);
    }

   private:
    static constexpr std::size_t MAX_INFLATE_WAYS = 16;

    memory::Pool* open(std::uint64_t budget, std::uint64_t sink_bytes) {
        if (budget == memory::NO_SPILL_BUDGET) return nullptr;
        const std::uint64_t kept =
            sink_bytes + budget / constants::file_admission::UNTRACKED_DIVISOR;
        const std::uint64_t pool_bytes =
            std::min<std::uint64_t>(budget > kept ? budget - kept : budget,
                                    memory::Manager::get().budget());
        pool_cap_ = pool_bytes;
        hold_cap_ = std::max<std::uint64_t>(
            pool_bytes -
                pool_bytes / constants::file_admission::PIECE_ROOM_DIVISOR,
            1);
        handle_ = memory::current_pool().child(
            "index files", memory::PoolKind::Scratch,
            memory::Limits{static_cast<std::size_t>(pool_bytes)});
        if (!handle_)
            throw DFTUtilsException(ErrorCode::MemoryBudgetExceeded,
                                    "no pool for the files in flight");
        pool_ = handle_.get();
        return pool_;
    }

    void enter() noexcept {
        started_.fetch_add(1, std::memory_order_relaxed);
        gate_.file_in();
        const std::size_t n =
            in_flight_.fetch_add(1, std::memory_order_relaxed) + 1;
        std::size_t seen = max_in_flight_.load(std::memory_order_relaxed);
        while (n > seen && !max_in_flight_.compare_exchange_weak(
                               seen, n, std::memory_order_relaxed)) {
        }
    }
    void left() noexcept {
        gate_.file_out();
        in_flight_.fetch_sub(1, std::memory_order_relaxed);
    }

    memory::PoolHandle handle_;
    memory::Pool* pool_ = nullptr;
    std::uint64_t pool_cap_ = 0;
    std::uint64_t hold_cap_ = 0;
    std::size_t workers_;
    std::size_t files_;
    ParseGate gate_;
    std::atomic<std::size_t> started_{0};
    std::atomic<std::size_t> in_flight_{0};
    std::atomic<std::size_t> max_in_flight_{0};
};

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_FILE_ADMISSION_H
