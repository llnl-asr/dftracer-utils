// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_BULK_SUPPORT_H
#define DFTRACER_UTILS_COMM_BACKEND_BULK_SUPPORT_H

#include <dftracer/utils/comm/backend/bulk.h>
#include <dftracer/utils/comm/bulk.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::comm::backend::bulk_support {

/**
 * @brief One exposure: memory a rank exposed, and the transfers that use it.
 *
 * Closing it stops new transfers; the memory stays valid for the owner until
 * every @ref Use is gone, which runs the callbacks of @ref close.
 */
class Exposure {
   public:
    Exposure(dstd::Span<std::byte> memory, Access access, MemoryKind kind,
             std::uint64_t group, std::uint32_t owner, std::uint64_t id)
        : memory_(memory),
          access_(access),
          kind_(kind),
          group_(group),
          owner_(owner),
          id_(id) {}

    dstd::Span<std::byte> memory() const noexcept { return memory_; }
    Access access() const noexcept { return access_; }
    std::uint64_t group() const noexcept { return group_; }
    std::uint32_t owner() const noexcept { return owner_; }
    std::uint64_t id() const noexcept { return id_; }
    MemoryKind kind() const noexcept { return kind_; }

    /// Takes a use, and gives false when the exposure is closed.
    bool enter() noexcept;
    void leave() noexcept;

    /// Closes the exposure and runs @p done, now when no use is left and when
    /// the last one ends otherwise.
    void close(Bulk::Drained done);

    bool is_closed() const noexcept;

    /// Closed and with no use left.
    bool is_drained() const noexcept;

   private:
    const dstd::Span<std::byte> memory_;
    const Access access_;
    const MemoryKind kind_;
    const std::uint64_t group_;
    const std::uint32_t owner_;
    const std::uint64_t id_;
    mutable std::mutex mutex_;
    std::size_t uses_ = 0;
    bool closed_ = false;
    dstd::Vector<Bulk::Drained> waiters_;
};

/// One transfer's hold on an exposure, which keeps the memory valid for the
/// owner. Passed as a keep-alive, it holds the exposure until the last copy
/// goes.
class Use {
   public:
    explicit Use(dstd::SharedPtr<Exposure> exposure) noexcept
        : exposure_(std::move(exposure)) {}
    Use(const Use&) = delete;
    Use& operator=(const Use&) = delete;
    ~Use() { exposure_->leave(); }

    dstd::Span<std::byte> memory() const noexcept {
        return exposure_->memory();
    }

   private:
    dstd::SharedPtr<Exposure> exposure_;
};

/// The exposures of one provider by group, owner and id, and the groups that
/// ended.
class ExposureTable {
   public:
    dstd::Result<BufferRef> add(std::uint64_t group, std::uint32_t rank,
                                dstd::Span<std::byte> memory, Access access,
                                MemoryKind kind);

    /// Takes a use of exposure @p id of @p owner in @p group for a transfer
    /// that writes or reads it. Gives the error that names why it cannot: the
    /// group ended, the exposure is unknown or closed, or its access does not
    /// allow it.
    dstd::Result<dstd::SharedPtr<Use>> open(std::uint64_t group,
                                            std::uint32_t owner,
                                            std::uint64_t id, bool writes);

    void close(const BufferRef& ref, Bulk::Drained done);

    /// Notes that @p group ended with @p why; the first end keeps its error.
    void end(std::uint64_t group, const dstd::Error& why);

    dstd::Optional<dstd::Error> ended(std::uint64_t group) const;

    /// Closes every exposure of @p group and forgets how it ended.
    void release(std::uint64_t group);

   private:
    struct Key {
        std::uint64_t group = 0;
        std::uint32_t owner = 0;
        std::uint64_t id = 0;
        friend bool operator<(const Key& a, const Key& b) noexcept {
            if (a.group != b.group) return a.group < b.group;
            if (a.owner != b.owner) return a.owner < b.owner;
            return a.id < b.id;
        }
    };

    /// Shared with the callbacks that drop a drained exposure, which may run
    /// after the table is gone.
    struct State {
        std::mutex mutex;
        std::uint64_t next_id = 1;
        dstd::Map<Key, dstd::SharedPtr<Exposure>> exposures;
        dstd::Map<std::uint64_t, dstd::Error> ended;
    };

    dstd::SharedPtr<State> state_ = dstd::make_shared<State>();
};

/**
 * @brief The bytes each peer may have in flight toward a receiver.
 *
 * A transfer asks for credit before a range moves and gives it back when the
 * range landed, so the bytes in flight toward one receiver from one peer never
 * pass its credit, however many transfers run. A request that waits is granted
 * in order as credit comes back. A request larger than the whole credit is
 * granted alone, so it never starves.
 */
class CreditGate {
   public:
    struct Key {
        std::uint64_t group = 0;
        std::uint32_t receiver = 0;
        std::uint32_t peer = 0;
        friend bool operator<(const Key& a, const Key& b) noexcept {
            if (a.group != b.group) return a.group < b.group;
            if (a.receiver != b.receiver) return a.receiver < b.receiver;
            return a.peer < b.peer;
        }
    };

    /// Runs when queued credit is granted, on the thread that gave back the
    /// credit and with no lock held.
    using Granted = dstd::Function<void()>;

    /// Takes @p bytes now and gives 0, or queues @p granted and gives the
    /// ticket that @ref withdraw takes.
    std::uint64_t acquire(const Key& key, std::uint64_t bytes, Granted granted,
                          std::uint64_t credit);

    /// Removes a queued request and gives true; false when it was granted or
    /// is being granted, in which case its callback runs.
    bool withdraw(const Key& key, std::uint64_t ticket);

    /// Gives @p bytes back, and grants what now fits.
    void release(const Key& key, std::uint64_t bytes);

    /// The most bytes any one key had in flight at once.
    std::uint64_t peak() const noexcept {
        return peak_.load(std::memory_order_relaxed);
    }

    /// The bytes held now, over every key.
    std::uint64_t in_flight() const;

    /// Takes @p bytes of @p key, held until the token goes.
    dstd::SharedPtr<void> hold(const Key& key, std::uint64_t bytes,
                               std::uint64_t credit);

   private:
    struct Waiter {
        std::uint64_t ticket = 0;
        std::uint64_t bytes = 0;
        Granted granted;
    };
    struct Account {
        std::uint64_t in_flight = 0;
        std::uint64_t credit = 0;
        dstd::Vector<Waiter> queue;
    };

    mutable std::mutex mutex_;
    std::uint64_t next_ticket_ = 1;
    dstd::Map<Key, Account> accounts_;
    std::atomic<std::uint64_t> peak_{0};
};

/// The credit one peer gets toward a receiver whose group has @p members
/// ranks: a share of the memory budget of the process, divided by the peers.
std::uint64_t credit_from_budget(std::uint32_t members) noexcept;

/// The error of a call that names an exposure the provider does not hold.
dstd::Error unknown_exposure_error(std::uint32_t owner, std::uint64_t id);

}  // namespace dftracer::utils::comm::backend::bulk_support

#endif  // DFTRACER_UTILS_COMM_BACKEND_BULK_SUPPORT_H
