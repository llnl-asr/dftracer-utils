// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_UDAF_STATES_H
#define DFTRACER_UTILS_DATAFRAME_UDAF_STATES_H

#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/udaf_registry.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::dataframe {

/// The states of one bound UDAF for groups 0 to group_count() - 1.
///
/// The states live in fixed blocks of aligned memory from the memory library,
/// so the address of a state never changes while the table grows. The table
/// holds its @ref UdafBinding, so the library behind the callbacks stays
/// loaded while a state lives, and the destructor destroys every state before
/// it releases the binding.
///
/// Not thread safe: one thread uses a table at a time. Tables of one UDAF may
/// be fed on different threads and merged with @ref combine_from when the UDAF
/// is parallel safe.
class UdafStates {
   public:
    explicit UdafStates(UdafBinding binding);
    UdafStates(UdafStates&& other) noexcept;
    UdafStates& operator=(UdafStates&&) = delete;
    UdafStates(const UdafStates&) = delete;
    UdafStates& operator=(const UdafStates&) = delete;
    ~UdafStates();

    const DataType& result_type() const noexcept {
        return binding_.result_type();
    }
    const UdafRecord& udaf() const noexcept { return binding_.udaf(); }

    std::int64_t group_count() const noexcept { return count_; }

    /// Makes the table hold `groups` groups: every new state is zero-filled,
    /// then initialized by one init call. Fails with OutOfMemory when the
    /// memory is refused. A smaller count changes nothing.
    dstd::Result<void> grow(std::int64_t groups);

    /// Feeds one batch: row i of the columns `args` (one per bound argument,
    /// each `gids.size()` rows) goes to group `gids[i]`, which grow must have
    /// made. A negative id skips the row. Unless the UDAF is null aware, a row
    /// where an argument is null is skipped too. A failure may leave some rows
    /// applied.
    dstd::Result<void> update(dstd::Span<const std::int64_t> gids,
                              dstd::Span<const Series* const> args);

    /// Merges group g of `other` into group `mapping[g]` of this table, for
    /// every group of `other`, then destroys its states and leaves it with no
    /// group. Both tables must hold the same UDAF. On failure nothing of
    /// `other` is destroyed.
    dstd::Result<void> combine_from(UdafStates& other,
                                    dstd::Span<const std::int64_t> mapping);

    /// The result column: one row per group, of @ref result_type. The states
    /// stay as they are.
    dstd::Result<Series> finalize();

    /// Appends group `group`, as bytes the UDAF reads back with
    /// @ref deserialize. A POD state is its `state_size` bytes.
    dstd::Result<void> serialize(std::int64_t group, dstd::String& out) const;

    /// Reads bytes @ref serialize wrote into group `group`, which grow made
    /// and nothing has fed.
    dstd::Result<void> deserialize(std::int64_t group, dstd::StringView bytes);

    /// The bytes the table holds, for a memory budget.
    std::size_t approx_bytes() const noexcept;

   private:
    class Block;

    void* state_at(std::int64_t group) const noexcept;
    void fill_all(dstd::Vector<void*>& out) const;
    void destroy_all() noexcept;

    UdafBinding binding_;
    dstd::Vector<Block> blocks_;
    dstd::Vector<void*> scratch_;
    std::size_t stride_;
    std::size_t per_block_;
    std::int64_t count_ = 0;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_UDAF_STATES_H
