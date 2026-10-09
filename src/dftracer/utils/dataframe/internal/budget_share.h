#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_BUDGET_SHARE_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_BUDGET_SHARE_H

#include <cstdint>

// The fractions of its memory budget a pipeline breaker gives to each of the
// things it holds at once. Divide the budget by one of these. The measured
// ratios between the bytes of data and the bytes a sort, a hash table or a
// spool really occupies are why no part is given the whole budget.
namespace dftracer::utils::dataframe::share {

/// In-memory state (an aggregate, a set of keys, the spool of a hash path)
/// spills past this fraction of the budget.
inline constexpr std::uint64_t SPILL_STATE = 2;
/// A replay spool keeps this fraction of the budget in memory before it
/// writes the rest to disk.
inline constexpr std::uint64_t SPOOL = 4;
/// A chunk, an output batch or a table sized from the budget, such as a
/// window chunk or the groups of a hash pass.
inline constexpr std::uint64_t CHUNK = 4;
/// A sort run is cut at this fraction of the budget: sorting holds the rows,
/// their sorted copy and the permutation at once.
inline constexpr std::uint64_t SORT_RUN = 4;
/// The output morsel of an op that makes more rows than it reads (a join, an
/// explode, an unpivot, a set of dummy columns): the result and the copy that
/// gathers it stay within this fraction of the budget.
inline constexpr std::uint64_t OUTPUT = 4;
/// A part of a split, a read-ahead batch or a statistics table.
inline constexpr std::uint64_t SMALL = 8;

}  // namespace dftracer::utils::dataframe::share

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_BUDGET_SHARE_H
