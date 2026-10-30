#ifndef DFTRACER_UTILS_DATAFRAME_AGG_SPILL_H
#define DFTRACER_UTILS_DATAFRAME_AGG_SPILL_H

#include <dftracer/utils/dataframe/agg.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::dataframe {

/// Out-of-core accumulation for a long-lived AggState, with the budget the
/// DataFrame breakers use (memory::resolve_spill_budget). Offer the state after
/// each accumulate. Past the budget it goes to a sorted temp run and an empty
/// state replaces it. drain() reads the runs back and finalizes.
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
    /// same specs when it exceeds the budget. Returns true when it spilled.
    bool maybe_spill(AggStatePtr& state);

    /// Take over `other`'s runs, leaving it empty. For a driver that merges a
    /// worker slice's accumulator into a master one.
    void adopt(AggSpiller& other);

    /// The whole aggregate: every run merged with the still-live `live`,
    /// finalized with `key_names`. Repeatable - the runs are re-read, not
    /// consumed, and `live` is not modified. With no runs this is exactly
    /// agg_finalize(live, key_names).
    DataFrame drain(const AggState& live,
                    const std::vector<std::string>& key_names) const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_AGG_SPILL_H
