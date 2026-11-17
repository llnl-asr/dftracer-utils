// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/bulk.h>
#include <dftracer/utils/comm/backend/bulk_support.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm::backend {

namespace {

using bulk_support::CreditGate;
using bulk_support::ExposureTable;
using bulk_support::Use;
using dstd::Condition;
using dstd::Error;

class LocalBulk final : public Bulk,
                        public std::enable_shared_from_this<LocalBulk> {
   public:
    dstd::Result<BufferRef> expose(std::uint64_t group, std::uint32_t rank,
                                   dstd::Span<std::byte> memory, Access access,
                                   MemoryKind kind) override {
        return table_.add(group, rank, memory, access, kind);
    }

    void unexpose(const BufferRef& ref, Drained done) override {
        table_.close(ref, std::move(done));
    }

    OpId pull(std::uint32_t rank, const BufferRef& ref, std::uint64_t offset,
              dstd::Span<std::byte> dst, Done done) override {
        return start(rank, ref, offset, dst.data(), nullptr,
                     ref.size - std::min(offset, ref.size), std::move(done),
                     false);
    }

    OpId push(std::uint32_t rank, const BufferRef& ref, std::uint64_t offset,
              dstd::Span<const std::byte> src, Done done) override {
        return start(rank, ref, offset, nullptr, src.data(), src.size(),
                     std::move(done), true);
    }

    bool cancel(std::uint64_t group, OpId op, Error why) override {
        dstd::SharedPtr<Transfer> transfer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto at = active_.find(op.value);
            if (at == active_.end() || at->second->group != group) return false;
            transfer = at->second;
        }
        transfer->stop(std::move(why));
        return true;
    }

    void abort(std::uint64_t group, Error why) override {
        table_.end(group, why);
        dstd::Vector<dstd::SharedPtr<Transfer>> stopping;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : active_)
                if (entry.second->group == group)
                    stopping.push_back(entry.second);
        }
        for (const auto& transfer : stopping) transfer->stop(why);
    }

    void bind_group(std::uint64_t group,
                    dstd::Span<const std::uint32_t> members) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sizes_[group] = static_cast<std::uint32_t>(members.size());
    }

    void release(std::uint64_t group) override {
        table_.release(group);
        const Error gone = Error::from_format<"group {} was released">(
            Condition::Cancelled, group);
        dstd::Vector<dstd::SharedPtr<Transfer>> stopping;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sizes_.erase(group);
            for (const auto& entry : active_)
                if (entry.second->group == group)
                    stopping.push_back(entry.second);
        }
        for (const auto& transfer : stopping) transfer->stop(gone);
    }

    void limit_range_bytes(std::uint64_t bytes) noexcept override {
        range_.store(std::max<std::uint64_t>(bytes, 1),
                     std::memory_order_relaxed);
    }

    void limit_credit_bytes(std::uint64_t bytes) noexcept override {
        credit_.store(bytes, std::memory_order_relaxed);
    }

    std::uint64_t peak_in_flight() const noexcept override {
        return gate_.peak();
    }

    std::uint64_t credit_in_flight() const noexcept override {
        return gate_.in_flight();
    }

    dstd::SharedPtr<void> hold_credit(std::uint64_t group,
                                      std::uint32_t receiver,
                                      std::uint32_t peer,
                                      std::uint64_t bytes) override {
        return gate_.hold(CreditGate::Key{group, receiver, peer}, bytes,
                          credit_of(group));
    }

   private:
    /// One copy between two memories of this process, in ranges: each range
    /// takes credit toward the receiving rank, copies, and gives it back.
    struct Transfer : std::enable_shared_from_this<Transfer> {
        LocalBulk* owner = nullptr;
        std::uint64_t id = 0;
        std::uint64_t group = 0;
        CreditGate::Key key;
        dstd::SharedPtr<Use> use;
        /// Where the range copies from and to, at the start of the transfer.
        const std::byte* from = nullptr;
        std::byte* to = nullptr;
        std::uint64_t total = 0;
        std::uint64_t copied = 0;
        std::uint64_t held = 0;
        Done done;

        std::mutex mutex;
        bool finished = false;
        bool waiting = false;
        std::uint64_t ticket = 0;
        dstd::Optional<Error> stopped;

        /// Copies ranges until the transfer ends or must wait for credit.
        void run() {
            while (copied < total) {
                if (const auto why = stop_reason()) {
                    finish(*why);
                    return;
                }
                const std::uint64_t credit = owner->credit_of(group);
                held = std::min({total - copied, owner->range(), credit});
                std::unique_lock<std::mutex> lock(mutex);
                // Held across the call, so a grant that a release gives at
                // once waits here until the ticket is noted.
                const std::uint64_t at = owner->gate_.acquire(
                    key, held, [self = shared_from_this()] { self->resume(); },
                    credit);
                if (at != 0) {
                    waiting = true;
                    ticket = at;
                    const bool cancelled = stopped.has_value();
                    lock.unlock();
                    if (cancelled) stop(*stop_reason());
                    return;
                }
                lock.unlock();
                copy_range();
            }
            finish({});
        }

        /// The queued credit arrived: copy the range it was for and go on.
        void resume() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                waiting = false;
            }
            copy_range();
            run();
        }

        void copy_range() {
            std::memcpy(to + copied, from + copied, held);
            copied += held;
            owner->gate_.release(key, held);
            held = 0;
        }

        dstd::Optional<Error> stop_reason() {
            std::lock_guard<std::mutex> lock(mutex);
            return stopped;
        }

        /// Asks the transfer to end with @p why at its next range, and ends it
        /// now when it waits for credit.
        void stop(Error why) {
            std::uint64_t at = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (finished) return;
                if (!stopped) stopped = why;
                if (!waiting) return;
                at = ticket;
                waiting = false;
            }
            if (owner->gate_.withdraw(key, at)) finish(why);
        }

        /// Ends the transfer once: with the error @p why, or well for none.
        void finish(const dstd::Optional<Error>& why) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (finished) return;
                finished = true;
            }
            owner->forget(id);
            if (why)
                done(0, &*why);
            else
                done(total, nullptr);
            use.reset();
        }
    };

    OpId start(std::uint32_t rank, const BufferRef& ref, std::uint64_t offset,
               std::byte* dst, const std::byte* src, std::uint64_t bytes,
               Done done, bool writes) {
        auto opened = table_.open(ref.group, ref.owner, ref.id, writes);
        if (!opened) {
            done(0, &opened.error());
            return NO_OP;
        }
        const dstd::Span<std::byte> memory = (*opened)->memory();
        if (offset > memory.size() || bytes > memory.size() - offset) {
            const Error why = Error::from_format<
                "a transfer of {} bytes at offset {} does not fit buffer {} of "
                "rank {}, which has {} bytes">(ChannelError::TooBig, bytes,
                                               offset, ref.id, ref.owner,
                                               memory.size());
            done(0, &why);
            return NO_OP;
        }
        auto transfer = dstd::make_shared<Transfer>();
        transfer->owner = this;
        transfer->group = ref.group;
        transfer->use = std::move(*opened);
        transfer->total = bytes;
        if (writes) {
            transfer->from = src;
            transfer->to = memory.data() + offset;
            transfer->key = CreditGate::Key{ref.group, ref.owner, rank};
        } else {
            transfer->from = memory.data() + offset;
            transfer->to = dst;
            transfer->key = CreditGate::Key{ref.group, rank, ref.owner};
        }
        transfer->done = std::move(done);
        if (const auto ended = table_.ended(ref.group)) {
            transfer->finish(*ended);
            return NO_OP;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            transfer->id = next_op_++;
            active_[transfer->id] = transfer;
        }
        const OpId id{transfer->id};
        transfer->run();
        return id;
    }

    void forget(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.erase(id);
    }

    std::uint64_t range() const noexcept {
        return std::min<std::uint64_t>(
            range_.load(std::memory_order_relaxed),
            common::constants::comm_settings::BULK_RANGE_BYTES);
    }

    std::uint64_t credit_of(std::uint64_t group) {
        const std::uint64_t forced = credit_.load(std::memory_order_relaxed);
        if (forced != 0) return forced;
        std::uint32_t members = 2;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto at = sizes_.find(group);
            if (at != sizes_.end()) members = at->second;
        }
        return bulk_support::credit_from_budget(members);
    }

    ExposureTable table_;
    CreditGate gate_;
    std::mutex mutex_;
    std::uint64_t next_op_ = 1;
    dstd::Map<std::uint64_t, dstd::SharedPtr<Transfer>> active_;
    dstd::Map<std::uint64_t, std::uint32_t> sizes_;
    std::atomic<std::uint64_t> range_{
        common::constants::comm_settings::BULK_RANGE_BYTES};
    std::atomic<std::uint64_t> credit_{0};
};

}  // namespace

dstd::SharedPtr<Bulk> make_local_bulk() {
    return dstd::make_shared<LocalBulk>();
}

}  // namespace dftracer::utils::comm::backend
