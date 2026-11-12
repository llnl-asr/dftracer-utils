// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_AGG_SPILL_H
#define DFTRACER_UTILS_DATAFRAME_AGG_SPILL_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace dftracer::utils::dataframe {

/// Out-of-core accumulation for a long-lived AggState, with the budget the
/// @ref DataFrame breakers use (memory::resolve_spill_budget). Offer the state
/// after each accumulate. Past the budget it goes to a sorted temp run and an
/// empty state replaces it. drain() reads the runs back and finalizes.
class AggSpiller {
   public:
    /// `budget` in bytes. 0 means auto (memory::resolve_spill_budget) and
    /// memory::NO_SPILL_BUDGET disables spilling. Resolved once, here.
    explicit AggSpiller(std::uint64_t budget = 0);
    ~AggSpiller();
    AggSpiller(AggSpiller&&) noexcept;
    AggSpiller& operator=(AggSpiller&&) noexcept;
    AggSpiller(const AggSpiller&) = delete;
    AggSpiller& operator=(const AggSpiller&) = delete;

    /// Number of runs written so far; 0 means everything is still in memory.
    /// The spill signal a caller (or a test) observes.
    std::size_t runs() const;

    /// Write `state` to a run and replace it with a fresh empty state over the
    /// same specs when it exceeds the budget. Gives true when it spilled, or
    /// the @ref ErrorCode::Io error of a run that cannot be written.
    dstd::Result<bool> maybe_spill(AggStatePtr& state);

    /// Take over `other`'s runs, leaving it empty. For a driver that merges a
    /// worker slice's accumulator into a master one.
    void adopt(AggSpiller& other);

    /// The whole aggregate: every run merged with the still-live `live`,
    /// finalized with `key_names`. Repeatable - the runs are re-read, not
    /// consumed, and `live` is not modified. With no runs this is exactly
    /// agg_finalize(live, key_names). A run that cannot be read gives an
    /// @ref ErrorCode::Io or @ref ErrorCode::Parse error.
    dstd::Result<DataFrame> drain(
        const AggState& live,
        const dstd::Vector<dstd::String>& key_names) const;

   private:
    struct Impl;
    dstd::UniquePtr<Impl> impl_;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_AGG_SPILL_H
