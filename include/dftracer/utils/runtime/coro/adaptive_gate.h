#ifndef DFTRACER_UTILS_RUNTIME_CORO_ADAPTIVE_GATE_H
#define DFTRACER_UTILS_RUNTIME_CORO_ADAPTIVE_GATE_H

#include <dftracer/utils/runtime/coro/async_semaphore.h>
#include <dftracer/utils/runtime/coro/task.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::coro {

/// Limit a gate starts at. One, so the first unit measures the time per
/// operation without contention from this gate.
inline constexpr std::size_t ADAPTIVE_INITIAL_LIMIT = 1;
/// A unit whose time per operation is above this many times the fastest seen
/// counts as slowed by contention.
inline constexpr std::uint64_t ADAPTIVE_LATENCY_TOLERANCE = 2;

/**
 * @brief Concurrency limit that follows the latency of the units it admits.
 *
 * For blocking calls whose cost grows with how many run at once, such as stat
 * on a parallel file system's metadata server. The limit grows by one when a
 * unit that started with the gate full ran within ADAPTIVE_LATENCY_TOLERANCE
 * times the fastest time per operation seen, and drops by a quarter when a
 * unit ran slower. It drops at most once per window of `limit` units, so the
 * slow units that were already running do not each cut it again. A slow unit
 * at a limit of one is not slowed by this gate's own contention, so it sets
 * the fastest time instead: a file system that is loaded by other clients for
 * the whole run is not held at one.
 *
 * Not thread safe; AdaptiveGate serializes it.
 */
class AdaptiveLimit {
   public:
    explicit AdaptiveLimit(std::size_t cap) noexcept
        : cap_(std::max<std::size_t>(cap, 1)),
          limit_(std::min(ADAPTIVE_INITIAL_LIMIT, cap_)) {}

    std::size_t limit() const noexcept { return limit_; }
    std::size_t cap() const noexcept { return cap_; }

    /// One finished unit: its time per operation in nanoseconds, and whether
    /// the gate was full when it started.
    void record(std::uint64_t ns_per_op, bool saturated) noexcept {
        if (since_drop_ < limit_) ++since_drop_;
        ns_per_op = std::max<std::uint64_t>(ns_per_op, 1);
        if (best_ == 0 || ns_per_op < best_) best_ = ns_per_op;
        if (ns_per_op > best_ * ADAPTIVE_LATENCY_TOLERANCE) {
            if (since_drop_ < limit_) return;
            if (limit_ == 1) {
                best_ = ns_per_op;
                return;
            }
            const std::size_t drop = std::max<std::size_t>(limit_ / 4, 1);
            limit_ = limit_ > drop ? limit_ - drop : 1;
            since_drop_ = 0;
            return;
        }
        if (saturated && limit_ < cap_) ++limit_;
    }

   private:
    std::size_t cap_;
    std::size_t limit_;
    std::size_t since_drop_ = 0;
    std::uint64_t best_ = 0;
};

/**
 * @brief Admits blocking units of work at the concurrency an AdaptiveLimit
 * sets, up to `cap` at once.
 *
 * A waiting unit suspends its coroutine and does not hold a worker, so an
 * elastic pool does not grow for it. Thread safe.
 */
class AdaptiveGate {
   public:
    explicit AdaptiveGate(std::size_t cap) : limit_(cap), sem_(limit_.cap()) {
        const bool held = sem_.try_acquire(limit_.cap() - limit_.limit());
        (void)held;
    }

    AdaptiveGate(const AdaptiveGate&) = delete;
    AdaptiveGate& operator=(const AdaptiveGate&) = delete;

    std::size_t limit() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return limit_.limit();
    }

    /// One admitted unit. It times the unit from its admission and reports
    /// the time per operation to the gate when it is destroyed.
    class Ticket {
       public:
        Ticket() = default;
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        Ticket(Ticket&& other) noexcept { take(other); }
        Ticket& operator=(Ticket&& other) noexcept {
            if (this != &other) {
                finish();
                take(other);
            }
            return *this;
        }
        ~Ticket() { finish(); }

       private:
        friend class AdaptiveGate;
        Ticket(AdaptiveGate& gate, bool saturated, std::size_t ops) noexcept
            : gate_(&gate),
              start_(std::chrono::steady_clock::now()),
              ops_(std::max<std::size_t>(ops, 1)),
              saturated_(saturated) {}

        void take(Ticket& other) noexcept {
            gate_ = std::exchange(other.gate_, nullptr);
            start_ = other.start_;
            ops_ = other.ops_;
            saturated_ = other.saturated_;
        }

        void finish() noexcept {
            if (!gate_) return;
            const auto ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start_)
                    .count();
            std::exchange(gate_, nullptr)
                ->end(static_cast<std::uint64_t>(ns) / ops_, saturated_);
        }

        AdaptiveGate* gate_ = nullptr;
        std::chrono::steady_clock::time_point start_{};
        std::size_t ops_ = 1;
        bool saturated_ = false;
    };

    /// Waits until the gate admits one unit of `ops` operations. Run the
    /// unit's blocking call right after and drop the ticket when it returns.
    CoroTask<Ticket> enter(std::size_t ops) {
        co_await sem_.acquire(1);
        co_return Ticket(*this, begin(), ops);
    }

   private:
    bool begin() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ++in_flight_ >= limit_.limit();
    }

    void end(std::uint64_t ns_per_op, bool saturated) {
        std::size_t give = 1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --in_flight_;
            const std::size_t before = limit_.limit();
            limit_.record(ns_per_op, saturated);
            const std::size_t after = limit_.limit();
            if (after > before) {
                const std::size_t grow = after - before;
                const std::size_t cancel = std::min(owed_, grow);
                owed_ -= cancel;
                give += grow - cancel;
            } else {
                owed_ += before - after;
            }
            if (owed_ > 0) {
                --owed_;
                --give;
            }
        }
        if (give > 0) sem_.release(give);
    }

    mutable std::mutex mutex_;
    AdaptiveLimit limit_;
    CoroSemaphore sem_;
    std::size_t in_flight_ = 0;
    std::size_t owed_ = 0;
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_RUNTIME_CORO_ADAPTIVE_GATE_H
