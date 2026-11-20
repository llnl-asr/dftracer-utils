// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_RUN_COUNTERS_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_RUN_COUNTERS_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <mutex>

namespace dftracer::utils::dataframe {

/// What one plugin counted under one name during a run.
struct CounterTotal {
    dstd::String owner;
    dstd::String name;
    std::int64_t value = 0;
};

/// The named counters plugins add to while one EXPLAIN ANALYZE run drives a
/// plan (dftu_ctx_vt::counter). A run makes its own and hands it to every scan
/// and node it opens through @ref ScanRequest::counters, and each cursor keeps
/// a reference, so a run's counters live as long as anything that adds to
/// them, and two runs never mix. Safe to add to from any thread.
class RunCounters {
   public:
    /// Adds `delta` to the counter `name` of `owner`.
    void add(dstd::StringView owner, dstd::StringView name,
             std::int64_t delta) noexcept;

    /// The sum of every counter, sorted by owner then name.
    dstd::Vector<CounterTotal> totals() const;

   private:
    mutable std::mutex mutex_;
    dstd::Vector<CounterTotal> entries_;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_RUN_COUNTERS_H
