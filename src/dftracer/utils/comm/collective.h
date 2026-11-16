// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_COLLECTIVE_H
#define DFTRACER_UTILS_COMM_COLLECTIVE_H

#include <dftracer/utils/comm/backend/collectives.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/try_reserve.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

/**
 * @brief The blocks a rank got from its in-neighbors, one per in-neighbor in
 * rank order.
 *
 * A copy shares the buffer, so every view stays valid while any copy lives.
 */
class NeighborBlocks {
   public:
    NeighborBlocks() noexcept = default;
    NeighborBlocks(
        Gathered all,
        dstd::SharedPtr<const dstd::Vector<std::uint32_t>> in) noexcept
        : all_(std::move(all)), in_(std::move(in)) {}

    /// How many in-neighbors sent a block.
    std::uint32_t size() const noexcept {
        return in_ ? static_cast<std::uint32_t>(in_->size()) : 0;
    }

    /// The group rank of in-neighbor @p index.
    std::uint32_t source(std::uint32_t index) const noexcept {
        return (*in_)[index];
    }

    /// The block of in-neighbor @p index.
    dstd::Span<const std::byte> operator[](std::uint32_t index) const noexcept {
        return all_[(*in_)[index]];
    }

    /// What keeps the buffer alive.
    const dstd::SharedPtr<const void>& keep() const noexcept {
        return all_.keep();
    }

    /// The block of in-neighbor @p index as values of @p T, under the rules of
    /// @ref Gathered::as.
    template <class T>
    dstd::Span<const T> as(std::uint32_t index) const noexcept {
        return all_.as<T>((*in_)[index]);
    }

   private:
    Gathered all_;
    dstd::SharedPtr<const dstd::Vector<std::uint32_t>> in_;
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
    AllToAll,
    ReduceScatter,
    Scan,
    ExclusiveScan,
    Neighbors,
    NeighborExchange,
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

/// The value of @ref backend::Record::color of an object call that shares its
/// objects inside the process instead of serializing them.
inline constexpr std::uint32_t OBJECT_SHARED = 3;

/// What @ref backend::Record::color says of an all_to_all: equal blocks of a
/// typed value, one byte block per member, or rows of one element type that a
/// key function sends.
inline constexpr std::uint32_t ALL_TO_ALL_EQUAL = 0;
inline constexpr std::uint32_t ALL_TO_ALL_BLOCKS = 1;
inline constexpr std::uint32_t ALL_TO_ALL_ROWS = 2;

/// What @ref backend::Record::key says of a reduce_scatter: equal blocks,
/// blocks of the sizes of a counts list, or a counts list that does not split
/// the values.
inline constexpr std::uint32_t SPLIT_EQUAL = 0;
inline constexpr std::uint32_t SPLIT_COUNTS = 1;
inline constexpr std::uint32_t SPLIT_BAD = 2;

/// What every rank got out of one call: the table the provider gave, or the
/// fold of it.
struct Outcome {
    backend::Table table;
    /// What each rank of this process put in its slot of the call, for a call
    /// that shares its objects instead of serializing them; null otherwise.
    const dstd::Vector<dstd::SharedPtr<const void>>* objects = nullptr;
};

/// Ends the call of one rank with its share of an @ref Outcome. Gives the error
/// that ended the call when the rank could not use the outcome, so the group
/// aborts: its ranks no longer agree on the data.
using Finisher = dstd::Function<dstd::Optional<dstd::Error>(const Outcome&)>;

template <class T>
inline constexpr bool IS_RESULT_V = false;
template <class T>
inline constexpr bool IS_RESULT_V<dstd::Result<T>> = true;

/// One data call as the group sees it.
struct DataCall {
    CallKind kind = CallKind::Broadcast;
    backend::Record record;
    backend::Contribution contribution;
    /// The merge of a reduction; the provider runs it for
    /// @ref backend::RECORD_COMBINE, else the group does in rank order.
    backend::Combine fold;
    /// The object a call of the shared path gives to the ranks of this
    /// process, which they read through @ref Outcome::objects.
    dstd::SharedPtr<const void> object;
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
/// result makes out of the shared buffer. Bytes that are not a whole number of
/// values came from a peer that broke the protocol and give an error.
template <class T>
dstd::Result<dstd::Vector<T>> vector_of(dstd::Span<const std::byte> bytes) {
    if (bytes.size() % sizeof(T) != 0) {
        return dstd::Unexpected(dstd::Error::from_format<
                                "{} bytes are not a whole number of {}-byte "
                                "values">(dstd::Condition::InvalidArgument,
                                          bytes.size(), sizeof(T)));
    }
    dstd::Vector<T> out(bytes.size() / sizeof(T));
    if (!bytes.empty()) std::memcpy(out.data(), bytes.data(), bytes.size());
    return dstd::Result<dstd::Vector<T>>(std::move(out));
}

/// One value of @p T from @p bytes, or an error when they are not exactly one.
template <class T>
dstd::Result<T> scalar_of(dstd::Span<const std::byte> bytes) {
    if (bytes.size() != sizeof(T)) {
        return dstd::Unexpected(
            dstd::Error::from_format<"{} bytes are not one {}-byte value">(
                dstd::Condition::InvalidArgument, bytes.size(), sizeof(T)));
    }
    T out;
    std::memcpy(&out, bytes.data(), sizeof(T));
    return dstd::Result<T>(out);
}

/// An owned contribution of @p members pieces for @ref
/// backend::RECORD_PERSONAL, where @p piece(d) gives the bytes for member `d`:
/// the sizes, then the pieces, copied once into one block the call owns.
template <class Piece>
backend::Contribution own_personal(std::uint32_t members, Piece piece) {
    auto block = dstd::make_shared<dstd::Vector<std::byte>>();
    std::uint64_t total = 0;
    for (std::uint32_t dest = 0; dest < members; ++dest)
        total += piece(dest).size();
    const std::size_t head = std::size_t{members} * sizeof(std::uint64_t);
    block->reserve(head + total);
    block->resize(head);
    for (std::uint32_t dest = 0; dest < members; ++dest) {
        const std::uint64_t size = piece(dest).size();
        std::memcpy(block->data() + dest * sizeof(std::uint64_t), &size,
                    sizeof(size));
    }
    for (std::uint32_t dest = 0; dest < members; ++dest) {
        const dstd::Span<const std::byte> bytes = piece(dest);
        block->insert(block->end(), bytes.begin(), bytes.end());
    }
    const dstd::Span<const std::byte> view(block->data(), block->size());
    return backend::Contribution{view, std::move(block), {}};
}

/// A contribution of @p members pieces for @ref backend::RECORD_PERSONAL in one
/// block of the exact size, whose pieces a caller serializes in place: ask for
/// the span of each piece in member order with @ref piece. The sizes in the
/// head are written as the pieces are handed out.
class PersonalBlock {
   public:
    /// The block for @p members pieces of @p piece_bytes bytes in all, or the
    /// error of a size that does not fit or cannot be allocated.
    static dstd::Result<PersonalBlock> make(std::uint32_t members,
                                            std::size_t piece_bytes) {
        const std::size_t head = std::size_t{members} * sizeof(std::uint64_t);
        if (piece_bytes > std::numeric_limits<std::size_t>::max() - head) {
            return dstd::Unexpected(dstd::Error::from_format<
                                    "pieces of {} bytes do not fit one block">(
                dstd::Condition::OutOfMemory, piece_bytes));
        }
        auto block = dstd::make_shared<dstd::Vector<std::byte>>();
        dstd::Result<void> room = dstd::try_reserve(*block, head + piece_bytes);
        if (!room) return dstd::Unexpected(room.error());
        block->resize(head + piece_bytes);
        return dstd::Result<PersonalBlock>(
            PersonalBlock(std::move(block), members));
    }

    /// The whole block, for a caller that lays the pieces out itself.
    dstd::Vector<std::byte>& bytes() noexcept { return *block_; }

    /// The span of the piece of the next member, of @p size bytes, which lie
    /// within the @p piece_bytes given at the start. A piece beyond the last
    /// member gets an empty span.
    dstd::Span<std::byte> piece(std::size_t size) noexcept {
        if (next_ >= members_) return {};
        const std::uint64_t wide = size;
        std::memcpy(block_->data() + next_ * sizeof(std::uint64_t), &wide,
                    sizeof(wide));
        ++next_;
        const dstd::Span<std::byte> out(block_->data() + cursor_, size);
        cursor_ += size;
        return out;
    }

    /// The contribution; the members that got no piece have an empty one.
    backend::Contribution finish() {
        const dstd::Span<const std::byte> view(block_->data(), block_->size());
        return backend::Contribution{view, std::move(block_), {}};
    }

   private:
    PersonalBlock(dstd::SharedPtr<dstd::Vector<std::byte>> block,
                  std::uint32_t members)
        : block_(std::move(block)),
          members_(members),
          cursor_(std::size_t{members} * sizeof(std::uint64_t)) {}

    dstd::SharedPtr<dstd::Vector<std::byte>> block_;
    std::uint32_t members_ = 0;
    std::uint32_t next_ = 0;
    std::size_t cursor_ = 0;
};

/// The head of the contribution of a reduce_scatter: a word that a merge sets
/// when the counts of two ranks differ, then one 64-bit count per member.
inline std::size_t split_head(std::uint32_t members) noexcept {
    return (std::size_t{members} + 1) * sizeof(std::uint64_t);
}

/// An owned contribution of a reduce_scatter: the head with @p counts, which
/// are empty for equal blocks, then @p values.
inline backend::Contribution own_split(dstd::Span<const std::byte> values,
                                       dstd::Span<const std::size_t> counts,
                                       std::uint32_t members) {
    const std::size_t head = split_head(members);
    auto block =
        dstd::make_shared<dstd::Vector<std::byte>>(head + values.size());
    for (std::size_t at = 0; at < counts.size() && at < members; ++at) {
        const std::uint64_t wide = counts[at];
        std::memcpy(block->data() + (at + 1) * sizeof(std::uint64_t), &wide,
                    sizeof(wide));
    }
    if (!values.empty())
        std::memcpy(block->data() + head, values.data(), values.size());
    const dstd::Span<const std::byte> view(block->data(), block->size());
    return backend::Contribution{view, std::move(block), {}};
}

/// The merge of a reduce_scatter: it compares the counts of the two sides, sets
/// the first word of @p acc when they differ, and merges the values behind the
/// head with @p inner.
inline backend::Combine make_split_combine(backend::Combine inner,
                                           std::size_t head) {
    return [inner = std::move(inner), head](std::byte* acc, const std::byte* in,
                                            std::size_t bytes) {
        if (bytes < head) return;
        if (std::memcmp(acc + sizeof(std::uint64_t), in + sizeof(std::uint64_t),
                        head - sizeof(std::uint64_t)) != 0) {
            const std::uint64_t differ = 1;
            std::memcpy(acc, &differ, sizeof(differ));
        }
        if (inner) inner(acc + head, in + head, bytes - head);
    };
}

/// One object per type @p T, which only its address is read for: the address
/// names the type exactly within one process, which no hash of its name does.
template <class T>
inline char type_key = 0;

/// Puts the identity of @p T in @p record, over the fields an object call does
/// not otherwise use (the key is compared across ranks), so ranks that pass
/// different types fail the shape check. The key is an address of this process
/// and belongs to the shared path only.
template <class T>
void set_type_key(backend::Record& record) noexcept {
    const std::uint64_t key = reinterpret_cast<std::uintptr_t>(&type_key<T>);
    record.elem_size = static_cast<std::uint32_t>(key);
    record.key = static_cast<std::uint32_t>(key >> 32);
}

/// The merge of entries 0 to @p last of @p table in rank order, which is the
/// one copy of entry 0 and one merge per further entry. An entry that is not
/// @p values values long is an error: the table came from another rank.
template <class T>
dstd::Result<dstd::Vector<T>> prefix_values(const backend::Table& table,
                                            std::uint32_t last,
                                            const backend::Combine& combine,
                                            std::size_t values) {
    if (last >= table.size()) {
        return dstd::Unexpected(dstd::Error::from_format<
                                "the table has {} entries but a scan needs {}">(
            dstd::Condition::Internal, table.size(), last + 1));
    }
    const std::size_t bytes = values * sizeof(T);
    if (table.at(0).size() != bytes) {
        return dstd::Unexpected(dstd::Error::from_format<
                                "rank 0 gave {} bytes to a scan of {} bytes">(
            dstd::Condition::Internal, table.at(0).size(), bytes));
    }
    auto first = vector_of<T>(table.at(0));
    if (!first) return dstd::Unexpected(first.error());
    dstd::Vector<T> acc = std::move(*first);
    for (std::uint32_t from = 1; from <= last; ++from) {
        const dstd::Span<const std::byte> in = table.at(from);
        if (in.size() != bytes) {
            return dstd::Unexpected(dstd::Error::from_format<
                                    "rank {} gave {} bytes to a scan of {} "
                                    "bytes">(dstd::Condition::Internal, from,
                                             in.size(), bytes));
        }
        combine(reinterpret_cast<std::byte*>(acc.data()), in.data(), bytes);
    }
    return dstd::Result<dstd::Vector<T>>(std::move(acc));
}

/// The error of a codec that wrote @p written bytes where it named @p named.
inline dstd::Error wrong_size(std::size_t written, std::size_t named) {
    return dstd::Error::from_format<
        "a codec wrote {} bytes but serialized_size named {}">(
        dstd::Condition::InvalidArgument, written, named);
}

/// The bytes of @p value in one block the call owns, serialized in place at the
/// size the codec names. A codec that fails, writes another size than it named,
/// or names a size that cannot be allocated gives an error.
template <class T, class C>
dstd::Result<backend::Contribution> serialize_owned(const C& codec,
                                                    const T& value) {
    const std::size_t size = codec.serialized_size(value);
    auto block = dstd::make_shared<dstd::Vector<std::byte>>();
    dstd::Result<void> room = dstd::try_reserve(*block, size);
    if (!room) return dstd::Unexpected(room.error());
    block->resize(size);
    dstd::Result<std::size_t> done =
        codec.serialize(value, dstd::Span<std::byte>(block->data(), size));
    if (!done) return dstd::Unexpected(done.error());
    if (*done != size) return dstd::Unexpected(wrong_size(*done, size));
    const dstd::Span<const std::byte> view(block->data(), block->size());
    return dstd::Result<backend::Contribution>(
        backend::Contribution{view, std::move(block), {}});
}

/// The object of rank @p from that @p codec reads from @p bytes, shared and
/// read-only, or the error that names that rank.
template <class T, class C>
dstd::Result<dstd::SharedPtr<const T>> deserialize_from(
    const C& codec, dstd::Span<const std::byte> bytes, std::uint32_t from) {
    dstd::Result<T> made = codec.deserialize(bytes);
    if (!made) {
        return dstd::Unexpected(
            dstd::Error::from_format<
                "could not deserialize the object of rank {}: "
                "{}">(made.error().condition(), from, made.error().message()));
    }
    return dstd::Result<dstd::SharedPtr<const T>>(
        dstd::make_shared<const T>(std::move(*made)));
}

/// The objects of every entry of @p table, in rank order.
template <class T, class C>
dstd::Result<dstd::Vector<dstd::SharedPtr<const T>>> deserialize_all(
    const C& codec, const backend::Table& table) {
    dstd::Vector<dstd::SharedPtr<const T>> out;
    out.reserve(table.size());
    for (std::uint32_t from = 0; from < table.size(); ++from) {
        auto one = deserialize_from<T>(codec, table.at(from), from);
        if (!one) return dstd::Unexpected(one.error());
        out.push_back(std::move(*one));
    }
    return dstd::Result<dstd::Vector<dstd::SharedPtr<const T>>>(std::move(out));
}

/// The object that rank @p from put in its slot of a call of the shared path,
/// or an error when the call carried none.
template <class T>
dstd::Result<dstd::SharedPtr<const T>> shared_object(
    const dstd::Vector<dstd::SharedPtr<const void>>* objects,
    std::uint32_t from) {
    if (objects == nullptr || from >= objects->size() || !(*objects)[from]) {
        return dstd::Unexpected(
            dstd::Error::from_format<"rank {} gave no object to the call">(
                dstd::Condition::Internal, from));
    }
    return dstd::Result<dstd::SharedPtr<const T>>(
        std::static_pointer_cast<const T>((*objects)[from]));
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
            return make_combine<T>([](const T& a, const T& b) -> T {
                return static_cast<T>(a + b);
            });
        case ReduceOp::Min:
            return make_combine<T>(
                [](const T& a, const T& b) { return b < a ? b : a; });
        case ReduceOp::Max:
            return make_combine<T>(
                [](const T& a, const T& b) { return a < b ? b : a; });
    }
    return {};
}

/// What @ref backend::Record::color names for a built-in reduction: the element
/// type, which a provider with a native reduction maps to its own type.
inline constexpr std::uint32_t ELEM_OPAQUE = 0;
inline constexpr std::uint32_t ELEM_INT8 = 1;
inline constexpr std::uint32_t ELEM_INT16 = 2;
inline constexpr std::uint32_t ELEM_INT32 = 3;
inline constexpr std::uint32_t ELEM_INT64 = 4;
inline constexpr std::uint32_t ELEM_UINT8 = 5;
inline constexpr std::uint32_t ELEM_UINT16 = 6;
inline constexpr std::uint32_t ELEM_UINT32 = 7;
inline constexpr std::uint32_t ELEM_UINT64 = 8;
inline constexpr std::uint32_t ELEM_FLOAT32 = 9;
inline constexpr std::uint32_t ELEM_FLOAT64 = 10;

/// The element code of @p T: the integer and floating types of a fixed width,
/// and @ref ELEM_OPAQUE for any other.
template <class T>
constexpr std::uint32_t elem_code() noexcept {
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, char> ||
                  std::is_same_v<T, wchar_t> || std::is_same_v<T, char8_t> ||
                  std::is_same_v<T, char16_t> || std::is_same_v<T, char32_t>) {
        return ELEM_OPAQUE;
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        if constexpr (sizeof(T) == 1) return ELEM_INT8;
        if constexpr (sizeof(T) == 2) return ELEM_INT16;
        if constexpr (sizeof(T) == 4) return ELEM_INT32;
        if constexpr (sizeof(T) == 8) return ELEM_INT64;
        return ELEM_OPAQUE;
    } else if constexpr (std::is_integral_v<T>) {
        if constexpr (sizeof(T) == 1) return ELEM_UINT8;
        if constexpr (sizeof(T) == 2) return ELEM_UINT16;
        if constexpr (sizeof(T) == 4) return ELEM_UINT32;
        if constexpr (sizeof(T) == 8) return ELEM_UINT64;
        return ELEM_OPAQUE;
    } else if constexpr (std::is_same_v<T, float>) {
        return ELEM_FLOAT32;
    } else if constexpr (std::is_same_v<T, double>) {
        return ELEM_FLOAT64;
    } else {
        return ELEM_OPAQUE;
    }
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
