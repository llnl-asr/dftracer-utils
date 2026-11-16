// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_COLLECTIVE_H
#define DFTRACER_UTILS_COMM_COLLECTIVE_H

#include <dftracer/utils/comm/backend/collectives.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace dftracer::utils::comm {

/// The built-in element-wise merges of @ref Group::reduce and
/// @ref Group::all_reduce, for arithmetic element types.
enum class ReduceOp : std::uint8_t { Sum, Min, Max };

/// How a reduction orders its merges.
enum class ReduceOrder : std::uint8_t {
    /// Folds the contributions in group rank order, so a floating point result
    /// is the same on every run and on every rank.
    Rank,
    /// Lets the provider fold the contributions in the order they arrive, which
    /// holds one running result instead of every contribution. A floating point
    /// result may differ between runs.
    Any,
};

/// A merge of two values of @p T for a reduction with a function of the user.
template <class T>
using ReduceFn = dstd::Function<T(const T&, const T&)>;

/**
 * @brief A read-only block of bytes and the keep-alive of its owner.
 *
 * A copy shares the block, so the view stays valid while any copy lives.
 */
struct Bytes {
    dstd::Span<const std::byte> view;
    dstd::SharedPtr<const void> keep;

    /// A view of memory that @p keep owns, which a call takes without a copy.
    static Bytes borrowed(dstd::Span<const std::byte> view,
                          dstd::SharedPtr<const void> keep) noexcept {
        return Bytes{view, std::move(keep)};
    }

    /// A copy of @p view in a block of its own.
    static Bytes copy_of(dstd::Span<const std::byte> view) {
        auto block = dstd::make_shared<dstd::Vector<std::byte>>();
        block->assign(view.begin(), view.end());
        const dstd::Span<const std::byte> own(block->data(), block->size());
        return Bytes{own, std::move(block)};
    }

    std::size_t size() const noexcept { return view.size(); }
};

/**
 * @brief The contributions of every rank in one owned receive buffer, with a
 * view of each rank.
 *
 * A copy shares the buffer, so every view stays valid while any copy lives. A
 * rank that does not take the result of a call gets an empty one.
 */
class Gathered {
   public:
    Gathered() noexcept = default;
    explicit Gathered(backend::Table table) noexcept
        : table_(std::move(table)) {}

    /// How many ranks the buffer holds, or 0 when this rank got nothing.
    std::uint32_t size() const noexcept { return table_.size(); }

    /// The bytes rank @p rank brought, in the one buffer.
    dstd::Span<const std::byte> operator[](std::uint32_t rank) const noexcept {
        return table_.at(rank);
    }

    /// The whole buffer: the bytes of rank 0, then rank 1, and on.
    dstd::Span<const std::byte> data() const noexcept { return table_.data; }

    /// What keeps the buffer alive: holding a copy of it keeps every view.
    const dstd::SharedPtr<const void>& keep() const noexcept {
        return table_.keep;
    }

    /// The bytes of rank @p rank as values of @p T. The rank must have brought
    /// a whole number of aligned values of @p T, which a typed call of equal
    /// element counts always gives.
    template <class T>
    dstd::Span<const T> as(std::uint32_t rank) const noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        const dstd::Span<const std::byte> bytes = table_.at(rank);
        if (bytes.size() % sizeof(T) != 0 ||
            reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(T) != 0)
            dstd::fatal("comm: a rank's bytes are not a run of aligned values");
        return dstd::Span<const T>(reinterpret_cast<const T*>(bytes.data()),
                                   bytes.size() / sizeof(T));
    }

   private:
    backend::Table table_;
};

namespace detail {

/// Which collective a call makes. Two ranks that arrive at one sequence number
/// must name the same one.
enum class CallKind : std::uint8_t {
    Barrier,
    MonitoredBarrier,
    Split,
    Broadcast,
    Gather,
    Scatter,
    AllGather,
    Reduce,
    AllReduce,
};

/// Whether a call of @p kind moves bytes, so a failure of it names the ranks
/// that did not arrive.
constexpr bool carries_data(CallKind kind) noexcept {
    return kind >= CallKind::Broadcast;
}

/// What a @ref backend::Record names in @ref backend::Record::reduce.
inline constexpr std::uint32_t REDUCE_NONE = 0;
inline constexpr std::uint32_t REDUCE_SUM = 1;
inline constexpr std::uint32_t REDUCE_MIN = 2;
inline constexpr std::uint32_t REDUCE_MAX = 3;
inline constexpr std::uint32_t REDUCE_CUSTOM = 4;

/// The value of @ref backend::Record::color of a scatter whose pieces differ in
/// size, which the root packs behind a table of 64-bit sizes.
inline constexpr std::uint32_t SCATTER_PACKED = 1;

/// What every rank got out of one call: the table the provider gave, or the
/// fold of it.
struct Outcome {
    backend::Table table;
};

/// Ends the call of one rank with its share of an @ref Outcome.
using Finisher = dstd::Function<void(const Outcome&)>;

/// One data call as the group sees it.
struct DataCall {
    CallKind kind = CallKind::Broadcast;
    backend::Record record;
    backend::Contribution contribution;
    /// The merge of a reduction; the provider runs it for
    /// @ref backend::RECORD_COMBINE, else the group does in rank order.
    backend::Combine fold;
};

/// A data call of @p kind with the given shape and @p contribution. The call
/// carries a table, and the caller adds what its kind needs.
inline DataCall make_data_call(CallKind kind, std::uint32_t root,
                               std::uint32_t elem_size, std::uint64_t count,
                               std::uint32_t flags,
                               backend::Contribution contribution) {
    DataCall call;
    call.kind = kind;
    call.record.root = root;
    call.record.elem_size = elem_size;
    call.record.count = count;
    call.record.flags = flags | backend::RECORD_TABLE;
    call.contribution = std::move(contribution);
    return call;
}

template <class T>
inline constexpr bool IS_SPAN_V = false;
template <class T, std::size_t Extent>
inline constexpr bool IS_SPAN_V<dstd::Span<T, Extent>> = true;

/// A value a collective moves as raw bytes.
template <class T>
concept Scalar = std::is_trivially_copyable_v<T> && !IS_SPAN_V<T>;

/// The bytes of one value of @p T.
template <class T>
dstd::Span<const std::byte> bytes_of_value(const T& value) noexcept {
    return dstd::Span<const std::byte>(
        reinterpret_cast<const std::byte*>(&value), sizeof(T));
}

/// A copy of @p view in a block the call owns. A call may end, by an abort or
/// by the cancel of its waiter, while the provider still reads its
/// contribution, so a view of the memory of the caller is copied here.
inline backend::Contribution own_bytes(dstd::Span<const std::byte> view) {
    Bytes owned = Bytes::copy_of(view);
    return backend::Contribution{owned.view, std::move(owned.keep), {}};
}

/// The contribution of one value of @p T, which the call owns in one block.
template <class T>
backend::Contribution own_value(const T& value) {
    auto block = dstd::make_shared<T>(value);
    const dstd::Span<const std::byte> view = bytes_of_value(*block);
    return backend::Contribution{view, std::move(block), {}};
}

/// The values of @p bytes as an owned vector, which is the one copy a typed
/// result makes out of the shared buffer.
template <class T>
dstd::Vector<T> vector_of(dstd::Span<const std::byte> bytes) {
    dstd::Vector<T> out(bytes.size() / sizeof(T));
    if (!bytes.empty()) std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

/// One value of @p T from @p bytes, or a default one when it is too short.
template <class T>
T scalar_of(dstd::Span<const std::byte> bytes) {
    T out{};
    if (bytes.size() >= sizeof(T)) std::memcpy(&out, bytes.data(), sizeof(T));
    return out;
}

/// The element-wise merge of runs of @p T by @p fn.
template <class T, class F>
backend::Combine make_combine(F fn) {
    return [fn = std::move(fn)](std::byte* acc, const std::byte* in,
                                std::size_t bytes) {
        for (std::size_t at = 0; at + sizeof(T) <= bytes; at += sizeof(T)) {
            T left;
            T right;
            std::memcpy(&left, acc + at, sizeof(T));
            std::memcpy(&right, in + at, sizeof(T));
            left = fn(left, right);
            std::memcpy(acc + at, &left, sizeof(T));
        }
    };
}

/// The size of piece @p index in the packed block of a scatter: 64-bit sizes,
/// one per rank, then the pieces.
inline std::uint64_t packed_size(dstd::Span<const std::byte> block,
                                 std::uint32_t index) noexcept {
    std::uint64_t size = 0;
    std::memcpy(&size, block.data() + index * sizeof(std::uint64_t),
                sizeof(size));
    return size;
}

/// The merge a built-in @p op names for @p T.
template <class T>
backend::Combine builtin_combine(ReduceOp op) {
    static_assert(std::is_arithmetic_v<T>,
                  "a built-in reduction needs an arithmetic type; pass a "
                  "function for another one");
    switch (op) {
        case ReduceOp::Sum:
            return make_combine<T>(
                [](const T& a, const T& b) { return a + b; });
        case ReduceOp::Min:
            return make_combine<T>(
                [](const T& a, const T& b) { return b < a ? b : a; });
        case ReduceOp::Max:
            return make_combine<T>(
                [](const T& a, const T& b) { return a < b ? b : a; });
    }
    return {};
}

constexpr std::uint32_t reduce_code(ReduceOp op) noexcept {
    switch (op) {
        case ReduceOp::Sum:
            return REDUCE_SUM;
        case ReduceOp::Min:
            return REDUCE_MIN;
        case ReduceOp::Max:
            return REDUCE_MAX;
    }
    return REDUCE_NONE;
}

}  // namespace detail

}  // namespace dftracer::utils::comm

#endif  // DFTRACER_UTILS_COMM_COLLECTIVE_H
