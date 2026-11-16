// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_GROUP_H
#define DFTRACER_UTILS_COMM_GROUP_H

#include <dftracer/utils/comm/backend/loader.h>
#include <dftracer/utils/comm/bulk.h>
#include <dftracer/utils/comm/codec.h>
#include <dftracer/utils/comm/collective.h>
#include <dftracer/utils/comm/error.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/stop_token.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

namespace dftracer::utils::comm {

namespace backend {
class Bulk;
class Collectives;
class Messaging;
}  // namespace backend

namespace detail {
struct Backend;
struct GroupState;
struct Hooks;
}  // namespace detail

template <class T>
class Channel;

class Neighborhood;

/// How a channel opens: the depth of the stream of one sender and one receiver,
/// and what a send does when that stream is full. A depth below 1 holds one
/// message, so no channel is ever unbounded.
struct ChannelOpts {
    std::size_t depth = 16;
    Overflow overflow = Overflow::Block;
};

/// What one call may add to the defaults of its group.
struct CallOpts {
    /// A deadline on the steady clock, in nanoseconds since its epoch, or 0 for
    /// the default timeout of the group, counted from the moment of this call.
    /// A deadline that passes on one rank aborts the group of a collective; it
    /// ends a call of a channel alone, with @ref dstd::Condition::Timeout.
    std::uint64_t deadline_ns = 0;
    /// A token already stopped when the call is made cancels the call, which
    /// aborts the group of a collective and ends a call of a channel alone. A
    /// stop that arrives while the call is in flight is not observed; use
    /// @ref Op::cancel or a deadline for that.
    coro::StopToken stop{};
    /// How a reduction orders its merges.
    ReduceOrder reduce_order = ReduceOrder::Rank;
};

/**
 * @brief One set of ranks that make the same collectives in the same order.
 *
 * A handle: copies name the same membership and the same call stream. The
 * collectives of one rank match in the order they are called, so every member
 * calls them in the same order; a rank that calls another collective than its
 * peers is an error, never a wait. A group with no members is empty
 * (@ref size is 0, @ref rank and @ref id are 0) and its collectives end at once
 * with a value.
 *
 * Every call of a member belongs to the group and to a sequence number of that
 * member, and peers match on both. Aborting a group, canceling a call and a
 * passed deadline each fail every pending and later call of the group on every
 * rank, with one error that names the rank that caused it.
 */
class Group {
   public:
    /// The color a rank passes to @ref split to stay out of every new group.
    static constexpr std::uint32_t NO_COLOR = 0xFFFFFFFFu;

    /// An empty group.
    Group() noexcept = default;

    /// Made by @ref Communicator and @ref split; a user never makes one.
    Group(dstd::SharedPtr<detail::Backend> backend,
          dstd::SharedPtr<detail::GroupState> state,
          std::uint32_t rank) noexcept
        : backend_(std::move(backend)), state_(std::move(state)), rank_(rank) {}

    /// This rank's number in the group, from 0 to @ref size minus 1.
    std::uint32_t rank() const noexcept { return rank_; }

    /// How many ranks the group has.
    std::uint32_t size() const noexcept;

    /// The id the group was made with, unique among the groups of one
    /// @ref Communicator.
    std::uint64_t id() const noexcept;

    /// Opens the channel @p name of this group, which carries values of @p T
    /// between the ranks. Opening is collective-free: both ends check their
    /// name and value size at the first message.
    template <class T>
    Channel<T> channel(dstd::StringView name);

    /// @ref channel with the depth and overflow of @p opts.
    template <class T>
    Channel<T> channel(dstd::StringView name, ChannelOpts opts);

    /// Opens the channel the tag type @p Tag names, and declares its
    /// @c value_type.
    template <class Tag>
    Channel<typename Tag::value_type> channel();

    /// @ref channel for a tag type, with the depth and overflow of @p opts.
    template <class Tag>
    Channel<typename Tag::value_type> channel(ChannelOpts opts);

    /**
     * @brief Exposes @p memory to the ranks of this group and gives the
     * descriptor they use to pull from it and push into it.
     *
     * Not collective and not a call of the group's sequence: it makes no
     * handshake and matches nothing, so a rank exposes when it wants. The owner
     * sends the descriptor to a peer with any channel or collective, and the
     * peer passes it to @ref pull or @ref push of this group.
     *
     * The exposing rank keeps @p memory valid, and does not move or free it,
     * until it has called @ref unexpose and the Op that call gave has
     * completed: that is when no pull or push on the exposure is left. The
     * caller orders overlapping transfers on one exposure, which the group does
     * not, and orders its own reads and writes of the memory against the
     * transfers that run on it. The
     * descriptor itself keeps nothing alive. A group that is released or
     * aborted ends its transfers, but the memory is still the owner's to keep
     * until its transfers ended. The memory kind must be host; a device kind
     * fails with @ref dstd::Condition::Unsupported.
     */
    dstd::Result<BufferRef> expose(dstd::Span<std::byte> memory,
                                   Access access = Access::ReadWrite,
                                   MemoryKind kind = MemoryKind::Host);

    /// Exposes memory that nothing may write: @p access must be
    /// @ref Access::ReadOnly.
    dstd::Result<BufferRef> expose(dstd::Span<const std::byte> memory,
                                   Access access,
                                   MemoryKind kind = MemoryKind::Host);

    /**
     * @brief Closes the exposure @p ref, which this rank made.
     *
     * No new pull or push on it starts: each fails with
     * @ref dstd::Condition::NotFound. The pulls and pushes that already started
     * run to their end, and the Op completes when the last one ended, so the
     * owner may free the memory after it. It completes at once for a ref that
     * is not open. A second call on a ref that is still draining completes
     * when the first does. It cannot be cancelled.
     *
     * A transfer between processes holds the exposure one range at a time, so
     * a pull of several ranges that is running when this is called may fail
     * with @ref dstd::Condition::NotFound at its next range, while a transfer
     * in one process runs to its end.
     */
    Op<void> unexpose(const BufferRef& ref);

    /**
     * @brief Copies the exposure @p ref, from @p offset to its end, into
     * @p dst, and gives the byte count.
     *
     * A @p dst shorter than that range fails with @ref ChannelError::TooBig and
     * copies nothing: the call never cuts a range silently. A @p ref past its
     * end, a write only exposure, a device memory kind, an unknown or closed
     * exposure and a ref of another group fail with an error that names the
     * ref. @p dst must stay valid until the Op ends, also after
     * @ref Op::cancel.
     *
     * A cancel, a deadline and a stopped token end this call alone, never the
     * group. An abort of the group, or of the owner's process, ends it with the
     * error of the group. Between processes a range up to a threshold moves in
     * one message, and a larger one is asked for in ranges that the pulling
     * rank paces by the credit it grants its peer from its memory budget, so a
     * pull of any size streams and is never refused for its size.
     */
    Op<std::uint64_t> pull(const BufferRef& ref, dstd::Span<std::byte> dst,
                           std::uint64_t offset = 0, CallOpts opts = {});

    /**
     * @brief Copies @p src into the exposure @p ref at @p offset, and gives the
     * byte count.
     *
     * Fails with @ref ChannelError::TooBig, and writes nothing, when @p src
     * does not fit between @p offset and the end of the exposure. A read only
     * exposure refuses it. The other failures and the cancel rules are those
     * of @ref pull; @p src must stay valid until the Op ends. The owner's
     * memory is written in ranges, so a call that fails midway may have
     * written some of them.
     */
    Op<std::uint64_t> push(const BufferRef& ref,
                           dstd::Span<const std::byte> src,
                           std::uint64_t offset = 0, CallOpts opts = {});

    /**
     * @brief Collective: each member picks a color and a key, and the members
     * of one color form a new group.
     *
     * The new ranks follow @p key and, for equal keys, the rank in this group.
     * A member that passes @ref NO_COLOR gets an empty group. Every member must
     * call it, and every member also gets a group of its own: a rank that no
     * longer participates stays out of later calls of the new group.
     */
    Op<Group> split(std::uint32_t color, std::uint32_t key, CallOpts opts = {});

    /// Collective: ends once every member called it.
    Op<void> barrier(CallOpts opts = {});

    /// @ref barrier whose error names the members that did not arrive when its
    /// deadline passes.
    Op<void> monitored_barrier(CallOpts opts = {});

    /**
     * @brief Collective: every rank gets the value of rank @p root.
     *
     * Every rank passes the same root, and for the typed forms the same number
     * of values; only the root's values are read. The typed forms copy the
     * values into the call, because the call may end while the provider still
     * reads them. The @ref Bytes form takes them as they are: its owner stays
     * alive until the call ends, and the result is a view of one buffer that
     * all ranks of a local provider share. The @c Span<const std::byte> form
     * copies the bytes and has no count to match.
     */
    template <detail::Scalar T>
    Op<T> broadcast(std::uint32_t root, const T& value, CallOpts opts = {}) {
        detail::DataCall call = detail::make_data_call(
            detail::CallKind::Broadcast, root, sizeof(T), 1,
            backend::RECORD_WANTS,
            rank_ == root ? detail::own_value(value) : backend::Contribution{});
        return data_op<T>(
            std::move(call),
            [root](const detail::Outcome& out) -> dstd::Result<T> {
                return detail::scalar_of<T>(out.table.at(root));
            },
            opts);
    }

    /// @ref broadcast of @p values, which every rank passes with one count.
    template <class T>
    Op<dstd::Vector<T>> broadcast(std::uint32_t root,
                                  dstd::Span<const T> values,
                                  CallOpts opts = {}) {
        static_assert(std::is_trivially_copyable_v<T>);
        detail::DataCall call = detail::make_data_call(
            detail::CallKind::Broadcast, root, sizeof(T), values.size(),
            backend::RECORD_WANTS,
            rank_ == root ? detail::own_bytes(std::as_bytes(values))
                          : backend::Contribution{});
        return data_op<dstd::Vector<T>>(
            std::move(call),
            [root](
                const detail::Outcome& out) -> dstd::Result<dstd::Vector<T>> {
                return detail::vector_of<T>(out.table.at(root));
            },
            opts);
    }

    /// @ref broadcast of bytes of any size; a rank other than @p root passes
    /// an empty one.
    Op<Bytes> broadcast(std::uint32_t root, Bytes value, CallOpts opts = {});

    /// @ref broadcast of a copy of @p value.
    Op<Bytes> broadcast(std::uint32_t root, dstd::Span<const std::byte> value,
                        CallOpts opts = {});

    /**
     * @brief Collective: the root gets the values of every rank in one buffer,
     * and the other ranks an empty @ref Gathered.
     *
     * Every rank passes the same number of values; the byte forms let the
     * sizes differ. The result is one owned buffer with a view per rank, and a
     * copy of it keeps every view alive.
     */
    template <class T>
    Op<Gathered> gather(std::uint32_t root, dstd::Span<const T> values,
                        CallOpts opts = {}) {
        static_assert(std::is_trivially_copyable_v<T>);
        return gather_call(detail::CallKind::Gather, root, sizeof(T),
                           values.size(),
                           detail::own_bytes(std::as_bytes(values)), opts);
    }

    /// @ref gather of bytes of any size.
    Op<Gathered> gather(std::uint32_t root, Bytes value, CallOpts opts = {});

    /// @ref gather of a copy of @p value.
    Op<Gathered> gather(std::uint32_t root, dstd::Span<const std::byte> value,
                        CallOpts opts = {});

    /// @ref gather whose result every rank gets.
    template <class T>
    Op<Gathered> all_gather(dstd::Span<const T> values, CallOpts opts = {}) {
        static_assert(std::is_trivially_copyable_v<T>);
        return gather_call(detail::CallKind::AllGather, 0, sizeof(T),
                           values.size(),
                           detail::own_bytes(std::as_bytes(values)), opts);
    }

    /// @ref all_gather of bytes of any size.
    Op<Gathered> all_gather(Bytes value, CallOpts opts = {});

    /// @ref all_gather of a copy of @p value.
    Op<Gathered> all_gather(dstd::Span<const std::byte> value,
                            CallOpts opts = {});

    /**
     * @brief Collective: rank `r` gets piece `r` of the values of @p root.
     *
     * The root passes `size() * k` values and every rank gets `k` of them; the
     * other ranks pass an empty span. A count that `size()` does not divide
     * ends the call with an error on every rank.
     */
    template <class T>
    Op<dstd::Vector<T>> scatter(std::uint32_t root, dstd::Span<const T> values,
                                CallOpts opts = {}) {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::uint32_t rank = rank_;
        detail::DataCall call = detail::make_data_call(
            detail::CallKind::Scatter, root, sizeof(T),
            rank_ == root ? values.size() : 0, backend::RECORD_WANTS,
            rank_ == root ? detail::own_bytes(std::as_bytes(values))
                          : backend::Contribution{});
        return data_op<dstd::Vector<T>>(
            std::move(call),
            [root, rank](
                const detail::Outcome& out) -> dstd::Result<dstd::Vector<T>> {
                const dstd::Span<const std::byte> all = out.table.at(root);
                const std::size_t piece = all.size() / out.table.size();
                return detail::vector_of<T>(all.subspan(rank * piece, piece));
            },
            opts);
    }

    /**
     * @brief @ref scatter of pieces of any size: rank `r` gets the bytes of
     * `pieces[r]`.
     *
     * The root passes one piece per rank and packs them into one block, which
     * is a copy; the other ranks pass an empty span. The result is a view of
     * the root's block.
     */
    Op<Bytes> scatter(std::uint32_t root,
                      dstd::Span<const dstd::Span<const std::byte>> pieces,
                      CallOpts opts = {});

    /**
     * @brief Collective: the root gets the element-wise merge of the values of
     * every rank, and the other ranks an empty result.
     *
     * Every rank passes the same number of values and the same @p op. The merge
     * folds the ranks in rank order unless @ref CallOpts::reduce_order says
     * otherwise. A built-in @p op needs an arithmetic @p T; the forms with a
     * function take any trivially copyable one. Every rank passes the same
     * function: the merge runs the function of @p root for @ref reduce and of
     * rank 0 for @ref all_reduce, in both orders. A provider that collects at
     * rank 0 and cannot see the function of another rank, such as the linear
     * one, runs the function of rank 0.
     */
    template <class T>
    Op<dstd::Vector<T>> reduce(std::uint32_t root, dstd::Span<const T> values,
                               ReduceOp op, CallOpts opts = {}) {
        return reduce_call<T>(detail::CallKind::Reduce, root, values,
                              detail::reduce_code(op),
                              detail::builtin_combine<T>(op), opts);
    }

    /// @ref reduce with the merge @p fn.
    template <class T>
    Op<dstd::Vector<T>> reduce(std::uint32_t root, dstd::Span<const T> values,
                               std::type_identity_t<ReduceFn<T>> fn,
                               CallOpts opts = {}) {
        return reduce_call<T>(detail::CallKind::Reduce, root, values,
                              detail::REDUCE_CUSTOM,
                              detail::make_combine<T>(std::move(fn)), opts);
    }

    /// @ref reduce of one value.
    template <detail::Scalar T>
    Op<T> reduce(std::uint32_t root, const T& value, ReduceOp op,
                 CallOpts opts = {}) {
        return reduce_one<T>(detail::CallKind::Reduce, root, value,
                             detail::reduce_code(op),
                             detail::builtin_combine<T>(op), opts);
    }

    /// @ref reduce of one value with the merge @p fn.
    template <detail::Scalar T>
    Op<T> reduce(std::uint32_t root, const T& value,
                 std::type_identity_t<ReduceFn<T>> fn, CallOpts opts = {}) {
        return reduce_one<T>(detail::CallKind::Reduce, root, value,
                             detail::REDUCE_CUSTOM,
                             detail::make_combine<T>(std::move(fn)), opts);
    }

    /// @ref reduce whose result every rank gets.
    template <class T>
    Op<dstd::Vector<T>> all_reduce(dstd::Span<const T> values, ReduceOp op,
                                   CallOpts opts = {}) {
        return reduce_call<T>(detail::CallKind::AllReduce, 0, values,
                              detail::reduce_code(op),
                              detail::builtin_combine<T>(op), opts);
    }

    /// @ref all_reduce with the merge @p fn.
    template <class T>
    Op<dstd::Vector<T>> all_reduce(dstd::Span<const T> values,
                                   std::type_identity_t<ReduceFn<T>> fn,
                                   CallOpts opts = {}) {
        return reduce_call<T>(detail::CallKind::AllReduce, 0, values,
                              detail::REDUCE_CUSTOM,
                              detail::make_combine<T>(std::move(fn)), opts);
    }

    /// @ref all_reduce of one value.
    template <detail::Scalar T>
    Op<T> all_reduce(const T& value, ReduceOp op, CallOpts opts = {}) {
        return reduce_one<T>(detail::CallKind::AllReduce, 0, value,
                             detail::reduce_code(op),
                             detail::builtin_combine<T>(op), opts);
    }

    /// @ref all_reduce of one value with the merge @p fn.
    template <detail::Scalar T>
    Op<T> all_reduce(const T& value, std::type_identity_t<ReduceFn<T>> fn,
                     CallOpts opts = {}) {
        return reduce_one<T>(detail::CallKind::AllReduce, 0, value,
                             detail::REDUCE_CUSTOM,
                             detail::make_combine<T>(std::move(fn)), opts);
    }

    /**
     * @brief Collective: every rank sends block `d` of its values to rank `d`
     * and gets one block from every rank.
     *
     * Every rank passes `size() * k` values for the same `k`, so the blocks
     * have equal size; a count that `size()` does not divide ends the call with
     * an error on every rank. The result is one owned buffer with a view per
     * source rank: view `s` holds the `k` values rank `s` sent to this rank.
     */
    template <class T>
    Op<Gathered> all_to_all(dstd::Span<const T> values, CallOpts opts = {}) {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::uint32_t members = size();
        const std::size_t block = members == 0 ? 0 : values.size() / members;
        detail::DataCall call = detail::make_data_call(
            detail::CallKind::AllToAll, 0, sizeof(T), values.size(),
            backend::RECORD_WANTS | backend::RECORD_PERSONAL,
            detail::own_personal(members, [&](std::uint32_t dest) {
                return std::as_bytes(values.subspan(dest * block, block));
            }));
        call.record.color = detail::ALL_TO_ALL_EQUAL;
        return table_call(std::move(call), opts);
    }

    /**
     * @brief @ref all_to_all of blocks of any size: rank `d` gets the bytes of
     * `blocks[d]`.
     *
     * Every rank passes one block per rank, which may be empty, and the sizes
     * may differ by block and by rank. The blocks are copied into one block of
     * the call. A count other than `size()` ends the call with an error on
     * every rank.
     */
    Op<Gathered> all_to_all(
        dstd::Span<const dstd::Span<const std::byte>> blocks,
        CallOpts opts = {});

    /**
     * @brief Collective: every rank sends each of its rows to the rank that
     * @p key_fn names, and gets the rows sent to it.
     *
     * @p key_fn maps a row to a rank below `size()` and runs once per row on
     * the calling thread. A row that maps to another rank ends the call with an
     * error on every rank. The result is one owned buffer with a view per
     * source rank, in rank order, and the rows of one source keep their order;
     * read a view with @ref Gathered::as. Every rank passes the same @p T.
     */
    template <class T, class KeyFn>
    Op<Gathered> exchange_by(dstd::Span<const T> rows, KeyFn key_fn,
                             CallOpts opts = {}) {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::uint32_t members = size();
        dstd::Vector<std::uint32_t> dest(rows.size());
        dstd::Vector<std::size_t> start(std::size_t{members} + 1, 0);
        bool has_bad = false;
        std::uint32_t bad = 0;
        for (std::size_t at = 0; at < rows.size(); ++at) {
            const auto key = key_fn(rows[at]);
            static_assert(
                std::is_integral_v<std::remove_cvref_t<decltype(key)>>,
                "the key function gives an integer rank");
            if (std::cmp_less(key, members) && std::cmp_greater_equal(key, 0)) {
                dest[at] = static_cast<std::uint32_t>(key);
                ++start[dest[at] + 1];
                continue;
            }
            dest[at] = members;
            if (has_bad) continue;
            has_bad = true;
            bad = std::in_range<std::uint32_t>(key)
                      ? static_cast<std::uint32_t>(key)
                      : 0xFFFFFFFEu;
        }
        for (std::uint32_t at = 0; at < members; ++at)
            start[at + 1] += start[at];
        backend::Contribution contribution;
        auto made =
            detail::PersonalBlock::make(members, start[members] * sizeof(T));
        if (!made) {
            abort(made.error());
        } else {
            dstd::Vector<std::byte>& bytes = made->bytes();
            const std::size_t head =
                std::size_t{members} * sizeof(std::uint64_t);
            for (std::uint32_t at = 0; at < members; ++at) {
                const std::uint64_t size =
                    (start[at + 1] - start[at]) * sizeof(T);
                std::memcpy(bytes.data() + at * sizeof(std::uint64_t), &size,
                            sizeof(size));
            }
            for (std::size_t at = 0; at < rows.size(); ++at) {
                if (dest[at] >= members) continue;
                std::memcpy(bytes.data() + head + start[dest[at]]++ * sizeof(T),
                            &rows[at], sizeof(T));
            }
            contribution = made->finish();
        }
        detail::DataCall call = detail::make_data_call(
            detail::CallKind::AllToAll, 0, sizeof(T), backend::UNCHECKED_COUNT,
            backend::RECORD_WANTS | backend::RECORD_PERSONAL,
            std::move(contribution));
        call.record.color = detail::ALL_TO_ALL_ROWS;
        call.record.key =
            has_bad ? (bad < 0xFFFFFFFEu ? bad : 0xFFFFFFFEu) + 1 : 0;
        return table_call(std::move(call), opts);
    }

    /**
     * @brief Collective: the element-wise merge of the values of every rank, of
     * which rank `r` gets block `r`.
     *
     * Every rank passes the same number of values and the same @p op. This form
     * cuts equal blocks, so the count must be a multiple of `size()`; the form
     * with @p counts cuts blocks of those sizes, which must add up to the count
     * and be the same on every rank. The ranks compare the counts exactly, so
     * counts that differ are an error on every rank. The merge follows the
     * rules of @ref all_reduce: rank order unless @ref CallOpts::reduce_order
     * says otherwise, and the function of rank 0. A provider that collects at
     * rank 0 sends each rank the whole merge, of which the rank keeps its
     * block.
     */
    template <class T>
    Op<dstd::Vector<T>> reduce_scatter(dstd::Span<const T> values, ReduceOp op,
                                       CallOpts opts = {}) {
        return reduce_scatter_call<T>(values, {}, false,
                                      detail::reduce_code(op),
                                      detail::builtin_combine<T>(op), opts);
    }

    /// @ref reduce_scatter with the merge @p fn.
    template <class T>
    Op<dstd::Vector<T>> reduce_scatter(dstd::Span<const T> values,
                                       std::type_identity_t<ReduceFn<T>> fn,
                                       CallOpts opts = {}) {
        return reduce_scatter_call<T>(values, {}, false, detail::REDUCE_CUSTOM,
                                      detail::make_combine<T>(std::move(fn)),
                                      opts);
    }

    /// @ref reduce_scatter into blocks of the sizes @p counts, one per rank.
    template <class T>
    Op<dstd::Vector<T>> reduce_scatter(dstd::Span<const T> values,
                                       dstd::Span<const std::size_t> counts,
                                       ReduceOp op, CallOpts opts = {}) {
        return reduce_scatter_call<T>(values, counts, true,
                                      detail::reduce_code(op),
                                      detail::builtin_combine<T>(op), opts);
    }

    /// @ref reduce_scatter into blocks of the sizes @p counts with the merge
    /// @p fn.
    template <class T>
    Op<dstd::Vector<T>> reduce_scatter(dstd::Span<const T> values,
                                       dstd::Span<const std::size_t> counts,
                                       std::type_identity_t<ReduceFn<T>> fn,
                                       CallOpts opts = {}) {
        return reduce_scatter_call<T>(
            values, counts, true, detail::REDUCE_CUSTOM,
            detail::make_combine<T>(std::move(fn)), opts);
    }

    /**
     * @brief Collective: rank `r` gets the merge of the values of ranks 0 to
     * `r`.
     *
     * Every rank passes the same number of values, the same @p op and, for the
     * forms with a function, the same function. The merge always folds in rank
     * order, so a float result is the same on every run and @ref
     * CallOpts::reduce_order has no effect. Every rank receives every
     * contribution and merges its own prefix, which costs `r` merges on rank
     * `r`.
     */
    template <class T>
    Op<dstd::Vector<T>> scan(dstd::Span<const T> values, ReduceOp op,
                             CallOpts opts = {}) {
        return scan_call<T>(detail::CallKind::Scan, values,
                            detail::reduce_code(op),
                            detail::builtin_combine<T>(op), opts);
    }

    /// @ref scan with the merge @p fn.
    template <class T>
    Op<dstd::Vector<T>> scan(dstd::Span<const T> values,
                             std::type_identity_t<ReduceFn<T>> fn,
                             CallOpts opts = {}) {
        return scan_call<T>(detail::CallKind::Scan, values,
                            detail::REDUCE_CUSTOM,
                            detail::make_combine<T>(std::move(fn)), opts);
    }

    /// @ref scan of one value.
    template <detail::Scalar T>
    Op<T> scan(const T& value, ReduceOp op, CallOpts opts = {}) {
        return scan_one<T>(detail::CallKind::Scan, value,
                           detail::reduce_code(op),
                           detail::builtin_combine<T>(op), opts);
    }

    /// @ref scan of one value with the merge @p fn.
    template <detail::Scalar T>
    Op<T> scan(const T& value, std::type_identity_t<ReduceFn<T>> fn,
               CallOpts opts = {}) {
        return scan_one<T>(detail::CallKind::Scan, value, detail::REDUCE_CUSTOM,
                           detail::make_combine<T>(std::move(fn)), opts);
    }

    /**
     * @brief @ref scan without the own values: rank `r` gets the merge of ranks
     * 0 to `r - 1`.
     *
     * Rank 0 has nothing to merge and gets an empty vector, or no value for the
     * form of one value.
     */
    template <class T>
    Op<dstd::Vector<T>> exclusive_scan(dstd::Span<const T> values, ReduceOp op,
                                       CallOpts opts = {}) {
        return scan_call<T>(detail::CallKind::ExclusiveScan, values,
                            detail::reduce_code(op),
                            detail::builtin_combine<T>(op), opts);
    }

    /// @ref exclusive_scan with the merge @p fn.
    template <class T>
    Op<dstd::Vector<T>> exclusive_scan(dstd::Span<const T> values,
                                       std::type_identity_t<ReduceFn<T>> fn,
                                       CallOpts opts = {}) {
        return scan_call<T>(detail::CallKind::ExclusiveScan, values,
                            detail::REDUCE_CUSTOM,
                            detail::make_combine<T>(std::move(fn)), opts);
    }

    /// @ref exclusive_scan of one value, which is empty on rank 0.
    template <detail::Scalar T>
    Op<dstd::Optional<T>> exclusive_scan(const T& value, ReduceOp op,
                                         CallOpts opts = {}) {
        return exclusive_one<T>(value, detail::reduce_code(op),
                                detail::builtin_combine<T>(op), opts);
    }

    /// @ref exclusive_scan of one value with the merge @p fn.
    template <detail::Scalar T>
    Op<dstd::Optional<T>> exclusive_scan(const T& value,
                                         std::type_identity_t<ReduceFn<T>> fn,
                                         CallOpts opts = {}) {
        return exclusive_one<T>(value, detail::REDUCE_CUSTOM,
                                detail::make_combine<T>(std::move(fn)), opts);
    }

    /**
     * @brief Collective: fixes a graph of the ranks, where each rank names the
     * ranks it sends to.
     *
     * The in-neighbors of a rank are the ranks that name it, which the call
     * works out and checks once: a rank beyond the group or named twice by one
     * rank ends the call with an error on every rank and aborts the group. The
     * order of @p out_neighbors is the order of the blocks of
     * @ref Neighborhood::neighbor_exchange. A rank may name itself.
     */
    Op<Neighborhood> neighbors(dstd::Span<const std::uint32_t> out_neighbors,
                               CallOpts opts = {});

    /**
     * @brief The `_object` forms move a value of @p T that is not trivially
     * copyable. Each result holds its objects as `SharedPtr<const T>`: shared
     * and read-only, so copy an object with `T mine = *object` to change it.
     *
     * Whether a value is serialized is decided for each pair of ranks, from
     * the process of each rank, which every rank knows. A rank in the process
     * of the sender holds the object the sender gave, with no copy and no
     * codec call. A value is serialized once, into the one block the call owns,
     * and only when a rank in another process reads it; a rank deserializes it
     * on the thread that ends the call, never under a lock of a provider, into
     * one object that all ranks of its process share. A call whose ranks are
     * all in one process never runs the codec. A codec that fails to serialize
     * aborts the group. A value that does not deserialize aborts the group too,
     * since the ranks no longer agree on their data, and ends the call of every
     * rank with the error of the group, which names the rank and the error of
     * the codec.
     * @ref all_gather_object gives every rank one object per rank. The types
     * are default constructible.
     */
    template <class T, Codec<T> C>
    Op<dstd::Vector<dstd::SharedPtr<const T>>> all_gather_object(
        T value, C codec, CallOpts opts = {}) {
        return object_gather<T, C>(detail::CallKind::AllGather, 0, true,
                                   std::move(value), std::move(codec), opts);
    }

    /// @ref gather of one object per rank; only @p root gets objects. A value
    /// that cannot be decoded fails @p root and the rank that owns it; the
    /// other ranks receive nothing and may end with success.
    template <class T, Codec<T> C>
    Op<dstd::Vector<dstd::SharedPtr<const T>>> gather_object(
        std::uint32_t root, T value, C codec, CallOpts opts = {}) {
        return object_gather<T, C>(detail::CallKind::Gather, root,
                                   rank_ == root, std::move(value),
                                   std::move(codec), opts);
    }

    /// @ref broadcast of the object of @p root; the other ranks pass any value.
    template <class T, Codec<T> C>
    Op<dstd::SharedPtr<const T>> broadcast_object(std::uint32_t root, T value,
                                                  C codec, CallOpts opts = {}) {
        using Out = dstd::SharedPtr<const T>;
        const bool one = in_one_process();
        dstd::SharedPtr<const T> holder;
        backend::Contribution contribution;
        dstd::Optional<dstd::Error> unreadable;
        if (rank_ == root) {
            holder = dstd::make_shared<const T>(std::move(value));
            if (!one) {
                auto made = detail::serialize_owned<T>(codec, *holder);
                if (made) {
                    contribution = std::move(*made);
                    unreadable = detail::check_readable<T>(
                        codec, contribution.bytes, rank_);
                } else {
                    abort(made.error());
                }
            }
        }
        detail::DataCall call = detail::make_data_call(
            detail::CallKind::Broadcast, root, 1, backend::UNCHECKED_COUNT,
            backend::RECORD_WANTS, std::move(contribution));
        if (one) {
            call.record.color = detail::OBJECT_SHARED;
            detail::set_type_key<T>(call.record);
        }
        call.object = std::move(holder);
        const std::uint32_t rank = rank_;
        return data_op<Out>(
            std::move(call),
            [root, rank, unreadable = std::move(unreadable),
             codec = std::move(codec)](
                const detail::Outcome& out) -> dstd::Result<Out> {
                if (unreadable) return dstd::Unexpected(*unreadable);
                return detail::object_from<T>(codec, out, rank, root);
            },
            opts);
    }

    /// @ref all_to_all of one object per rank: rank `d` gets `values[d]` from
    /// every rank, so each rank gets one object per source rank. The count must
    /// be `size()` on every rank.
    template <class T, Codec<T> C>
    Op<dstd::Vector<dstd::SharedPtr<const T>>> all_to_all_object(
        dstd::Vector<T> values, C codec, CallOpts opts = {}) {
        using Out = dstd::Vector<dstd::SharedPtr<const T>>;
        const bool one = in_one_process();
        const std::uint32_t members = size();
        const std::size_t count = values.size();
        auto holder =
            dstd::make_shared<const dstd::Vector<T>>(std::move(values));
        std::uint32_t flags = backend::RECORD_WANTS;
        backend::Contribution contribution;
        dstd::Optional<dstd::Error> unreadable;
        if (!one) {
            flags |= backend::RECORD_PERSONAL;
            dstd::Vector<std::size_t> sizes(members, 0);
            std::size_t total = 0;
            dstd::Optional<dstd::Error> failed;
            for (std::size_t dest = 0; dest < members && dest < count; ++dest) {
                if (same_process(static_cast<std::uint32_t>(dest))) continue;
                sizes[dest] = codec.serialized_size((*holder)[dest]);
                if (sizes[dest] >
                    std::numeric_limits<std::size_t>::max() - total) {
                    failed = dstd::Error::from_format<
                        "the objects of an all_to_all name more bytes than "
                        "fit">(dstd::Condition::OutOfMemory);
                    break;
                }
                total += sizes[dest];
            }
            if (!failed) {
                auto made = detail::PersonalBlock::make(members, total);
                if (!made) {
                    failed = made.error();
                } else {
                    for (std::uint32_t dest = 0; dest < members && !failed;
                         ++dest) {
                        const dstd::Span<std::byte> piece =
                            made->piece(sizes[dest]);
                        if (dest >= count || same_process(dest)) continue;
                        dstd::Result<std::size_t> done =
                            codec.serialize((*holder)[dest], piece);
                        if (!done) {
                            failed = done.error();
                        } else if (*done != sizes[dest]) {
                            failed = detail::wrong_size(*done, sizes[dest]);
                        } else {
                            if (!unreadable)
                                unreadable = detail::check_readable<T>(
                                    codec, piece, rank_);
                        }
                    }
                    if (!failed) contribution = made->finish();
                }
            }
            if (failed) abort(*failed);
        }
        detail::DataCall call =
            detail::make_data_call(detail::CallKind::AllToAll, 0, 1, count,
                                   flags, std::move(contribution));
        if (one) {
            call.record.color = detail::OBJECT_SHARED;
            detail::set_type_key<T>(call.record);
        } else {
            call.record.color = detail::ALL_TO_ALL_BLOCKS;
        }
        call.object = holder;
        const std::uint32_t rank = rank_;
        return data_op<Out>(
            std::move(call),
            [rank, unreadable = std::move(unreadable),
             codec = std::move(codec)](
                const detail::Outcome& out) -> dstd::Result<Out> {
                if (unreadable) return dstd::Unexpected(*unreadable);
                Out got;
                got.reserve(out.processes.size());
                for (std::uint32_t from = 0; from < out.processes.size();
                     ++from) {
                    if (out.processes[from] != out.processes[rank]) {
                        auto sent = detail::deserialize_from<T>(
                            codec, out.table.at(from), from);
                        if (!sent) return dstd::Unexpected(sent.error());
                        got.push_back(std::move(*sent));
                        continue;
                    }
                    auto theirs = detail::shared_object<dstd::Vector<T>>(
                        out.objects, from);
                    if (!theirs) return dstd::Unexpected(theirs.error());
                    if (rank >= (*theirs)->size()) {
                        return dstd::Unexpected(
                            dstd::Error::from_format<"rank {} gave {} objects "
                                                     "to an all_to_all">(
                                dstd::Condition::Internal, from,
                                (*theirs)->size()));
                    }
                    got.push_back(
                        dstd::SharedPtr<const T>(*theirs, &(**theirs)[rank]));
                }
                return dstd::Result<Out>(std::move(got));
            },
            opts);
    }

    /// The timeout the later calls of this rank use when @ref CallOpts names
    /// no deadline, counted from the moment each call is made. 0, the default,
    /// sets none.
    void set_default_deadline(std::uint64_t timeout_ns) noexcept;

    /**
     * @brief Fails every pending and later call of the group, on every rank,
     * with one error that names @p why and this rank.
     *
     * The first abort of a group wins and a later one is dropped.
     */
    void abort(dstd::Error why) noexcept;

   private:
    friend class Neighborhood;
    friend struct detail::Hooks;

    /// True when every rank of the group is in one process, so an object call
    /// serializes nothing.
    bool in_one_process() const noexcept;

    /// True when group rank @p other is in the process of this rank. The group
    /// must not be empty.
    bool same_process(std::uint32_t other) const noexcept;

    /// Starts @p call, which ends with the value @p convert makes of its
    /// outcome, or of the error it gives: a @c dstd::Result. The value stays
    /// staged until every rank of the slot in this process has one; an error
    /// of any rank ends the call of every rank with the abort of the group.
    template <class T, class Convert>
    Op<T> data_op(detail::DataCall call, Convert convert,
                  const CallOpts& opts) {
        auto state = dstd::make_shared<OpState<T>>();
        if (!state_) {
            state->succeed(T{});
            return Op<T>(std::move(state), nullptr, rank_);
        }
        start_data(
            std::move(call),
            [state, convert = std::move(convert)](
                const detail::Outcome& out) -> dstd::Optional<dstd::Error> {
                if constexpr (detail::IS_RESULT_V<std::invoke_result_t<
                                  Convert&, const detail::Outcome&>>) {
                    auto made = convert(out);
                    if (!made) return made.error();
                    state->stage(std::move(*made));
                } else {
                    state->stage(convert(out));
                }
                return {};
            },
            state, opts);
        return Op<T>(std::move(state), state_, rank_);
    }

    /// A call whose result is the table of this rank.
    Op<Gathered> table_call(detail::DataCall call, const CallOpts& opts);

    template <class T, class C>
    Op<dstd::Vector<dstd::SharedPtr<const T>>> object_gather(
        detail::CallKind kind, std::uint32_t root, bool takes, T value, C codec,
        const CallOpts& opts) {
        using Out = dstd::Vector<dstd::SharedPtr<const T>>;
        const std::uint32_t flags = takes ? backend::RECORD_WANTS : 0;
        const bool one = in_one_process();
        auto holder = dstd::make_shared<const T>(std::move(value));
        backend::Contribution contribution;
        dstd::Optional<dstd::Error> unreadable;
        if (!one &&
            (kind == detail::CallKind::AllGather || !same_process(root))) {
            auto made = detail::serialize_owned<T>(codec, *holder);
            if (made) {
                contribution = std::move(*made);
                unreadable =
                    detail::check_readable<T>(codec, contribution.bytes, rank_);
            } else {
                abort(made.error());
            }
        }
        detail::DataCall call =
            detail::make_data_call(kind, root, 1, backend::UNCHECKED_COUNT,
                                   flags, std::move(contribution));
        if (one) {
            call.record.color = detail::OBJECT_SHARED;
            detail::set_type_key<T>(call.record);
        }
        call.object = std::move(holder);
        const std::uint32_t rank = rank_;
        return data_op<Out>(
            std::move(call),
            [takes, rank, unreadable = std::move(unreadable),
             codec = std::move(codec)](
                const detail::Outcome& out) -> dstd::Result<Out> {
                if (unreadable) return dstd::Unexpected(*unreadable);
                if (!takes) return Out{};
                return detail::objects_from_all<T>(codec, out, rank);
            },
            opts);
    }

    template <class T>
    Op<dstd::Vector<T>> reduce_scatter_call(
        dstd::Span<const T> values, dstd::Span<const std::size_t> counts,
        bool by_counts, std::uint32_t code, backend::Combine fold,
        const CallOpts& opts) {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::uint32_t members = size();
        bool fits = !by_counts || counts.size() == members;
        std::size_t total = 0;
        for (std::size_t at = 0; fits && at < counts.size(); ++at) {
            fits = counts[at] <= values.size() &&
                   total <= values.size() - counts[at];
            if (fits) total += counts[at];
        }
        fits = fits && (!by_counts || total == values.size());
        detail::DataCall call = detail::make_data_call(
            detail::CallKind::ReduceScatter, 0, sizeof(T), values.size(),
            reduce_flags(true, opts),
            detail::own_split(
                std::as_bytes(values),
                by_counts && fits ? counts : dstd::Span<const std::size_t>(),
                members));
        call.record.reduce = code;
        call.record.key = !by_counts ? detail::SPLIT_EQUAL
                          : fits     ? detail::SPLIT_COUNTS
                                     : detail::SPLIT_BAD;
        call.fold = detail::make_split_combine(std::move(fold),
                                               detail::split_head(members));
        dstd::Vector<std::size_t> kept;
        if (by_counts && fits) kept.assign(counts.begin(), counts.end());
        const std::uint32_t rank = rank_;
        return data_op<dstd::Vector<T>>(
            std::move(call),
            [rank, members, by_counts, kept = std::move(kept)](
                const detail::Outcome& out) -> dstd::Result<dstd::Vector<T>> {
                const dstd::Span<const std::byte> entry = out.table.at(0);
                const std::size_t head = detail::split_head(members);
                if (entry.size() < head ||
                    (entry.size() - head) % sizeof(T) != 0) {
                    return dstd::Unexpected(
                        dstd::Error::from_format<"a reduce_scatter merge of {} "
                                                 "bytes does not fit its head">(
                            dstd::Condition::InvalidArgument, entry.size()));
                }
                std::uint64_t differ = 0;
                std::memcpy(&differ, entry.data(), sizeof(differ));
                if (differ != 0) {
                    return dstd::Unexpected(dstd::Error::from_format<
                                            "the ranks gave different counts "
                                            "to a reduce_scatter">(
                        dstd::Condition::InvalidArgument));
                }
                const dstd::Span<const std::byte> whole = entry.subspan(head);
                const std::size_t all = whole.size() / sizeof(T);
                std::size_t begin = 0;
                std::size_t count = 0;
                if (!by_counts) {
                    count = all / members;
                    begin = rank * count;
                } else {
                    for (std::uint32_t at = 0; at < rank; ++at)
                        begin += kept[at];
                    count = kept[rank];
                }
                if (begin > all || count > all - begin) {
                    return dstd::Unexpected(
                        dstd::Error::from_format<"the merge has {} values but "
                                                 "block {} starts at {} and "
                                                 "holds {}">(
                            dstd::Condition::Internal, all, rank, begin,
                            count));
                }
                return detail::vector_of<T>(
                    whole.subspan(begin * sizeof(T), count * sizeof(T)));
            },
            opts);
    }

    template <class T>
    Op<dstd::Vector<T>> scan_call(detail::CallKind kind,
                                  dstd::Span<const T> values,
                                  std::uint32_t code, backend::Combine fold,
                                  const CallOpts& opts) {
        static_assert(std::is_trivially_copyable_v<T>);
        detail::DataCall call = detail::make_data_call(
            kind, 0, sizeof(T), values.size(), backend::RECORD_WANTS,
            detail::own_bytes(std::as_bytes(values)));
        call.record.reduce = code;
        const std::uint32_t rank = rank_;
        const bool inclusive = kind == detail::CallKind::Scan;
        const std::size_t count = values.size();
        return data_op<dstd::Vector<T>>(
            std::move(call),
            [rank, inclusive, count, fold = std::move(fold)](
                const detail::Outcome& out) -> dstd::Result<dstd::Vector<T>> {
                if (!inclusive && rank == 0) return dstd::Vector<T>{};
                return detail::prefix_values<T>(
                    out.table, inclusive ? rank : rank - 1, fold, count);
            },
            opts);
    }

    template <class T>
    Op<T> scan_one(detail::CallKind kind, const T& value, std::uint32_t code,
                   backend::Combine fold, const CallOpts& opts) {
        detail::DataCall call =
            detail::make_data_call(kind, 0, sizeof(T), 1, backend::RECORD_WANTS,
                                   detail::own_value(value));
        call.record.reduce = code;
        const std::uint32_t rank = rank_;
        return data_op<T>(
            std::move(call),
            [rank, fold = std::move(fold)](
                const detail::Outcome& out) -> dstd::Result<T> {
                auto merged =
                    detail::prefix_values<T>(out.table, rank, fold, 1);
                if (!merged) return dstd::Unexpected(merged.error());
                if (merged->empty()) {
                    return dstd::Unexpected(dstd::Error::from_format<
                                            "a scan of one value merged none">(
                        dstd::Condition::Internal));
                }
                return dstd::Result<T>((*merged)[0]);
            },
            opts);
    }

    template <class T>
    Op<dstd::Optional<T>> exclusive_one(const T& value, std::uint32_t code,
                                        backend::Combine fold,
                                        const CallOpts& opts) {
        detail::DataCall call = detail::make_data_call(
            detail::CallKind::ExclusiveScan, 0, sizeof(T), 1,
            backend::RECORD_WANTS, detail::own_value(value));
        call.record.reduce = code;
        const std::uint32_t rank = rank_;
        return data_op<dstd::Optional<T>>(
            std::move(call),
            [rank, fold = std::move(fold)](
                const detail::Outcome& out) -> dstd::Result<dstd::Optional<T>> {
                if (rank == 0) return dstd::Optional<T>{};
                auto merged =
                    detail::prefix_values<T>(out.table, rank - 1, fold, 1);
                if (!merged) return dstd::Unexpected(merged.error());
                if (merged->empty()) {
                    return dstd::Unexpected(dstd::Error::from_format<
                                            "a scan of one value merged none">(
                        dstd::Condition::Internal));
                }
                return dstd::Result<dstd::Optional<T>>(
                    dstd::Optional<T>((*merged)[0]));
            },
            opts);
    }

    template <class T>
    Op<dstd::Vector<T>> reduce_call(detail::CallKind kind, std::uint32_t root,
                                    dstd::Span<const T> values,
                                    std::uint32_t code, backend::Combine fold,
                                    const CallOpts& opts) {
        static_assert(std::is_trivially_copyable_v<T>);
        const bool takes = kind == detail::CallKind::AllReduce || rank_ == root;
        detail::DataCall call = detail::make_data_call(
            kind, root, sizeof(T), values.size(), reduce_flags(takes, opts),
            detail::own_bytes(std::as_bytes(values)));
        call.record.reduce = code;
        if (code != detail::REDUCE_CUSTOM)
            call.record.color = detail::elem_code<T>();
        call.fold = std::move(fold);
        return data_op<dstd::Vector<T>>(
            std::move(call),
            [takes](
                const detail::Outcome& out) -> dstd::Result<dstd::Vector<T>> {
                if (!takes) return dstd::Vector<T>{};
                return detail::vector_of<T>(out.table.at(0));
            },
            opts);
    }

    template <class T>
    Op<T> reduce_one(detail::CallKind kind, std::uint32_t root, const T& value,
                     std::uint32_t code, backend::Combine fold,
                     const CallOpts& opts) {
        const bool takes = kind == detail::CallKind::AllReduce || rank_ == root;
        detail::DataCall call = detail::make_data_call(
            kind, root, sizeof(T), 1, reduce_flags(takes, opts),
            detail::own_value(value));
        call.record.reduce = code;
        if (code != detail::REDUCE_CUSTOM)
            call.record.color = detail::elem_code<T>();
        call.fold = std::move(fold);
        return data_op<T>(
            std::move(call),
            [takes](const detail::Outcome& out) -> dstd::Result<T> {
                if (!takes) return T{};
                return detail::scalar_of<T>(out.table.at(0));
            },
            opts);
    }

    static std::uint32_t reduce_flags(bool takes, const CallOpts& opts) {
        std::uint32_t flags = takes ? backend::RECORD_WANTS : 0;
        if (opts.reduce_order == ReduceOrder::Any)
            flags |= backend::RECORD_COMBINE;
        return flags;
    }

    Op<Gathered> gather_call(detail::CallKind kind, std::uint32_t root,
                             std::uint32_t elem_size, std::uint64_t count,
                             backend::Contribution contribution,
                             const CallOpts& opts);

    void start_data(detail::DataCall call, detail::Finisher finisher,
                    const dstd::SharedPtr<PendingOp>& state,
                    const CallOpts& opts);

    dstd::SharedPtr<detail::Backend> backend_;
    dstd::SharedPtr<detail::GroupState> state_;
    std::uint32_t rank_ = 0;
};

/**
 * @brief A fixed graph of the ranks of a group, made by @ref Group::neighbors,
 * and the exchanges along its edges.
 *
 * A handle with value semantics. Every rank of the group makes the same calls
 * on its own copy in the same order, as with the collectives of the group.
 */
class Neighborhood {
   public:
    /// An empty graph on an empty group.
    Neighborhood() noexcept = default;

    /// The ranks this rank sends to, in the order the blocks of
    /// @ref neighbor_exchange follow.
    dstd::Span<const std::uint32_t> out_neighbors() const noexcept {
        return dstd::Span<const std::uint32_t>(out_.data(), out_.size());
    }

    /// The ranks that name this rank, in rank order, which is the order of the
    /// blocks @ref neighbor_exchange gives back.
    dstd::Span<const std::uint32_t> in_neighbors() const noexcept {
        return in_ ? dstd::Span<const std::uint32_t>(in_->data(), in_->size())
                   : dstd::Span<const std::uint32_t>();
    }

    /**
     * @brief Collective: sends @p blocks, one to each out-neighbor, and gets
     * one block from each in-neighbor.
     *
     * `blocks[i]` goes to out-neighbor `i`; a block may be empty and the sizes
     * may differ. The blocks are copied into one block of the call. A rank that
     * passes another count than its out-degree ends the call with an error on
     * every rank. The result is one owned buffer with a view per in-neighbor.
     */
    Op<NeighborBlocks> neighbor_exchange(
        dstd::Span<const dstd::Span<const std::byte>> blocks,
        CallOpts opts = {});

    /// @ref neighbor_exchange of one value of @p T per out-neighbor; read a
    /// block with @ref NeighborBlocks::as.
    template <class T>
    Op<NeighborBlocks> neighbor_exchange(dstd::Span<const T> values,
                                         CallOpts opts = {}) {
        static_assert(std::is_trivially_copyable_v<T>);
        dstd::Vector<dstd::Span<const std::byte>> blocks;
        blocks.reserve(values.size());
        for (const T& value : values)
            blocks.push_back(std::as_bytes(dstd::Span<const T>(&value, 1)));
        return neighbor_exchange(dstd::Span<const dstd::Span<const std::byte>>(
                                     blocks.data(), blocks.size()),
                                 opts);
    }

   private:
    friend class Group;

    Neighborhood(Group group, dstd::Vector<std::uint32_t> out,
                 dstd::SharedPtr<const dstd::Vector<std::uint32_t>> in) noexcept
        : group_(std::move(group)), out_(std::move(out)), in_(std::move(in)) {}

    Group group_;
    dstd::Vector<std::uint32_t> out_;
    dstd::SharedPtr<const dstd::Vector<std::uint32_t>> in_;
};

/**
 * @brief The members of one run. Makes the groups.
 *
 * The local-threads backend runs every rank as a coroutine in this process, on
 * the executor given at start, so a program written against it runs unchanged
 * on a backend that spreads the ranks over nodes. Every rank of the run is
 * reachable from every thread of the process.
 */
class Communicator {
   public:
    /// Starts the local-threads backend with @p ranks ranks on @p executor,
    /// which must outlive the communicator and every group it made. Binds the
    /// bootstrap, the messaging and the collectives of the local backend.
    static dstd::Result<Communicator> start_local(std::uint32_t ranks,
                                                  coro::Executor& executor);

    /**
     * @brief Starts the communicator of this process on @p kind.
     *
     * Local is one rank in this process. Mpi loads the MPI backend library at
     * run time and gives one rank per process; it fails with an error that
     * names every path tried when the library is missing or built for another
     * comm. Auto is Mpi when a launcher started this process and Local
     * otherwise, and it fails, rather than run copies that do not talk to each
     * other, when a launcher started the process and the library is missing.
     */
    static dstd::Result<Communicator> start(BackendKind kind,
                                            const StartOptions& options = {});

    /// Starts a communicator on providers a backend library made.
    static dstd::Result<Communicator> start(Providers providers);

    /// Made by @ref start_local.
    explicit Communicator(dstd::SharedPtr<detail::Backend> backend) noexcept
        : backend_(std::move(backend)) {}

    /// The group of every rank of the run. @p rank must be below its size.
    Group world(std::uint32_t rank) noexcept;

    /// The ranks that share this node, which is every rank for this backend.
    Group local(std::uint32_t rank) noexcept;

    /// How many ranks the run has.
    std::uint32_t size() const noexcept;

    /// The rank this process runs when the backend has one rank per process; 0
    /// for the local-threads backend, whose ranks all run here.
    std::uint32_t rank() const noexcept;

   private:
    friend struct detail::Hooks;
    dstd::SharedPtr<detail::Backend> backend_;
    dstd::SharedPtr<detail::GroupState> world_;
    /// One group per node of the run, by world rank.
    dstd::Vector<Group> node_;
};

namespace detail {

/// Test hooks that force the states a correct peer never reaches.
struct Hooks {
    /// Makes @p rank report @p version at the protocol handshake.
    static void set_protocol_version(Communicator& comm, std::uint32_t rank,
                                     std::uint32_t version) noexcept;

    /// Calls that are alive now, of every communicator.
    static std::size_t live_ops() noexcept;

    /// Groups that are alive now, of every communicator.
    static std::size_t live_groups() noexcept;

    /// Holds the timer thread in @ref expire right before it takes the group
    /// lock, which is where a completing call can land. Null clears it.
    static void set_expire_gate(void (*gate)()) noexcept;

    /// Clears what @ref expire reports to a test.
    static void reset_expire_signal() noexcept;

    /// True once @ref expire reached the group lock, and once it returned. A
    /// test waits on these instead of on time.
    static bool expire_at_lock() noexcept;
    static bool expire_returned() noexcept;

    /// The provider that moves the messages of @p comm.
    static dstd::SharedPtr<backend::Messaging> messaging(
        const Communicator& comm) noexcept;

    /// The provider that collects the calls of @p comm.
    static dstd::SharedPtr<backend::Collectives> collectives(
        const Communicator& comm) noexcept;

    /// The provider that moves the bulk transfers of @p comm.
    static dstd::SharedPtr<backend::Bulk> bulk(
        const Communicator& comm) noexcept;

    /// Runs the bulk transfers of @p comm on @p bulk from now on. A test binds
    /// another one before the first group exposes memory.
    static void set_bulk(Communicator& comm,
                         dstd::SharedPtr<backend::Bulk> bulk) noexcept;

    /// Collects the calls of @p comm with @p collectives from now on. A test
    /// binds another one before it makes the first call.
    static void set_collectives(
        Communicator& comm,
        dstd::SharedPtr<backend::Collectives> collectives) noexcept;

    /// Makes the native collectives of @p comm move at most @p bytes in one
    /// MPI call, so a test runs the rounds of a large call on small data. Every
    /// process must set the same limit before its first call.
    static void set_chunk_limit(Communicator& comm,
                                std::uint64_t bytes) noexcept;

    /// An all_to_all whose personal contribution is @p contribution as it is,
    /// well formed or not.
    static Op<Gathered> all_to_all_raw(Group& group,
                                       backend::Contribution contribution,
                                       const CallOpts& opts = {});

    /// Delivers to @p comm the abort that a peer process announced for group
    /// @p id, made from group @p parent by a split or 0, as a messaging
    /// provider does.
    static void peer_abort(Communicator& comm, std::uint64_t id,
                           std::uint64_t parent, const dstd::Error& why);

    /// The id the next group made on @p comm takes.
    static std::uint64_t next_group_id(const Communicator& comm) noexcept;

    /// How many aborts of groups not made yet @p comm keeps.
    static std::size_t early_abort_count(const Communicator& comm);

    /// Releases the groups of @p comm on their provider, as the end of a
    /// communicator does, so a test can ask what a dead group left behind.
    static void release_groups(Communicator& comm) noexcept;
};

}  // namespace detail

}  // namespace dftracer::utils::comm

#endif  // DFTRACER_UTILS_COMM_GROUP_H
