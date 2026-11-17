// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/bulk.h>
#include <dftracer/utils/comm/channel.h>
#include <dftracer/utils/comm/detail/group_state.h>
#include <dftracer/utils/comm/error.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/dstd/memory.h>

#include <mutex>
#include <utility>

namespace dftracer::utils::comm {

using dstd::Condition;
using dstd::Error;

Error unsupported_kind_error(MemoryKind kind) {
    return Error::from_format<
        "memory kind {} is not supported, only host memory is">(
        Condition::Unsupported, static_cast<unsigned>(kind));
}

dstd::Optional<Error> check_transfer(const BufferRef& ref, bool writes,
                                     std::uint64_t offset,
                                     std::uint64_t buffer_size) {
    if (ref.kind != MemoryKind::Host) return unsupported_kind_error(ref.kind);
    if (writes && ref.access == Access::ReadOnly)
        return Error::from_format<
            "buffer {} of rank {} is read only, so a push is refused">(
            Condition::InvalidArgument, ref.id, ref.owner);
    if (!writes && ref.access == Access::WriteOnly)
        return Error::from_format<
            "buffer {} of rank {} is write only, so a pull is refused">(
            Condition::InvalidArgument, ref.id, ref.owner);
    if (offset > ref.size)
        return Error::from_format<
            "offset {} is past the end of buffer {} of rank {}, which has {} "
            "bytes">(ChannelError::TooBig, offset, ref.id, ref.owner, ref.size);
    const std::uint64_t room = ref.size - offset;
    if (writes) {
        if (buffer_size > room)
            return Error::from_format<
                "a push of {} bytes at offset {} does not fit buffer {} of "
                "rank {}, which has {} bytes">(ChannelError::TooBig,
                                               buffer_size, offset, ref.id,
                                               ref.owner, ref.size);
    } else if (buffer_size < room) {
        return Error::from_format<
            "a pull of {} bytes from offset {} of buffer {} of rank {} does "
            "not fit a buffer of {} bytes">(ChannelError::TooBig, room, offset,
                                            ref.id, ref.owner, buffer_size);
    }
    return {};
}

namespace detail {

namespace {

/// A pull or a push in flight. It stands alone: a cancel or a passed deadline
/// ends it and nothing else, and the provider decides the outcome of a cancel,
/// so a transfer it already finished keeps its result.
class BulkCall final : public OpState<std::uint64_t> {
   public:
    BulkCall(dstd::SharedPtr<Backend> backend, std::uint64_t group) noexcept
        : backend_(std::move(backend)), group_(group) {}

    /// Keeps the counters of the group, which the call writes when it ends.
    void hold(dstd::SharedPtr<Tally> tally) noexcept {
        tally_ = std::move(tally);
    }

    /// Notes the provider's handle for the transfer, so a cancel reaches it. A
    /// cancel that came first reaches it here.
    void note(backend::OpId op) noexcept {
        dstd::Optional<Error> why;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            op_ = op;
            why = requested_;
        }
        if (why && op.value != 0)
            (void)backend_->bulk->cancel(group_, op, std::move(*why));
    }

    void cancel_alone(const Error& why) noexcept override {
        if (is_done()) return;
        backend::OpId op;
        Error reason;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!requested_) requested_ = why;
            reason = *requested_;
            op = op_;
        }
        if (op.value != 0)
            (void)backend_->bulk->cancel(group_, op, std::move(reason));
    }

   private:
    dstd::SharedPtr<Backend> backend_;
    dstd::SharedPtr<Tally> tally_;
    std::uint64_t group_ = 0;
    std::mutex mutex_;
    backend::OpId op_;
    dstd::Optional<Error> requested_;
};

Error group_mismatch_error(const BufferRef& ref, std::uint64_t group) {
    return Error::from_format<
        "buffer {} of rank {} was exposed in group {}, not in group {}">(
        Condition::InvalidArgument, ref.id, ref.owner, ref.group, group);
}

Error empty_group_error() {
    return Error::from_format<"an empty group has no ranks to expose to">(
        Condition::InvalidArgument);
}

Op<std::uint64_t> failed_transfer(std::uint32_t rank, Error why) {
    auto call = dstd::make_shared<OpState<std::uint64_t>>();
    call->fail(std::move(why));
    return Op<std::uint64_t>(std::move(call), dstd::SharedPtr<GroupState>{},
                             rank);
}

/// Starts the pull or push of @p ref for @p rank of @p state.
Op<std::uint64_t> bulk_transfer(const dstd::SharedPtr<Backend>& backend,
                                const dstd::SharedPtr<GroupState>& state,
                                std::uint32_t rank, const BufferRef& ref,
                                bool writes, std::uint64_t offset,
                                std::byte* dst, const std::byte* src,
                                std::uint64_t bytes, const CallOpts& opts) {
    if (!state) return failed_transfer(rank, empty_group_error());
    if (ref.group != state->id)
        return failed_transfer(rank, group_mismatch_error(ref, state->id));
    if (ref.owner >= state->size())
        return failed_transfer(
            rank, Error::from_format<"rank {} is not in group {}">(
                      Condition::InvalidArgument, ref.owner, state->id));
    if (const auto why = check_transfer(ref, writes, offset, bytes))
        return failed_transfer(rank, *why);
    if (opts.stop.stop_requested())
        return failed_transfer(
            rank, Error{Condition::Cancelled,
                        "the call was cancelled before it started"});
    std::uint64_t timeout = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->aborted) return failed_transfer(rank, *state->aborted);
        timeout = state->default_timeout_ns[rank];
    }
    auto call = dstd::make_shared<BulkCall>(backend, state->id);
    call->hold(state->tally);
    call->observe(begin_call(state->tally.get(),
                             writes ? CallKind::Push : CallKind::Pull,
                             state->members[rank]));
    auto done = [call, writes](std::uint64_t moved, const Error* why) {
        if (why != nullptr) {
            call->fail(*why);
            return;
        }
        if (writes)
            call->set_sent(moved);
        else
            call->set_received(moved);
        call->succeed(moved);
    };
    const backend::OpId op =
        writes ? backend->bulk->push(rank, ref, offset,
                                     dstd::Span<const std::byte>(src, bytes),
                                     std::move(done))
               : backend->bulk->pull(rank, ref, offset,
                                     dstd::Span<std::byte>(dst, bytes),
                                     std::move(done));
    call->note(op);
    watch_deadline(backend, opts.deadline_ns == 0 ? timeout : 0, call, opts);
    return Op<std::uint64_t>(std::move(call), dstd::SharedPtr<GroupState>{},
                             rank);
}

}  // namespace

}  // namespace detail

dstd::Result<BufferRef> Group::expose(dstd::Span<std::byte> memory,
                                      Access access, MemoryKind kind) {
    if (!state_) return dstd::Unexpected(detail::empty_group_error());
    if (kind != MemoryKind::Host)
        return dstd::Unexpected(unsupported_kind_error(kind));
    return backend_->bulk->expose(state_->id, rank_, memory, access, kind);
}

dstd::Result<BufferRef> Group::expose(dstd::Span<const std::byte> memory,
                                      Access access, MemoryKind kind) {
    if (access != Access::ReadOnly)
        return dstd::Unexpected(
            Error::from_format<
                "memory that is read only cannot be exposed for writes">(
                Condition::InvalidArgument));
    return expose(dstd::Span<std::byte>(const_cast<std::byte*>(memory.data()),
                                        memory.size()),
                  access, kind);
}

Op<void> Group::unexpose(const BufferRef& ref) {
    auto call = dstd::make_shared<OpState<void>>();
    if (!state_) {
        call->fail(detail::empty_group_error());
    } else if (ref.group != state_->id) {
        call->fail(detail::group_mismatch_error(ref, state_->id));
    } else if (ref.owner != rank_) {
        call->fail(Error::from_format<
                   "buffer {} belongs to rank {}, so rank {} cannot close "
                   "it">(Condition::InvalidArgument, ref.id, ref.owner, rank_));
    } else {
        backend_->bulk->unexpose(ref, [call] { call->succeed(); });
    }
    return Op<void>(std::move(call), dstd::SharedPtr<detail::GroupState>{},
                    rank_);
}

Op<std::uint64_t> Group::pull(const BufferRef& ref, dstd::Span<std::byte> dst,
                              std::uint64_t offset, CallOpts opts) {
    return detail::bulk_transfer(backend_, state_, rank_, ref, false, offset,
                                 dst.data(), nullptr, dst.size(), opts);
}

Op<std::uint64_t> Group::push(const BufferRef& ref,
                              dstd::Span<const std::byte> src,
                              std::uint64_t offset, CallOpts opts) {
    return detail::bulk_transfer(backend_, state_, rank_, ref, true, offset,
                                 nullptr, src.data(), src.size(), opts);
}

}  // namespace dftracer::utils::comm
