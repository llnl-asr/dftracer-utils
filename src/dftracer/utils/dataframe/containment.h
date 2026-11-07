// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_CONTAINMENT_H
#define DFTRACER_UTILS_DATAFRAME_CONTAINMENT_H

#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::dataframe {

// The one containment core shared by the materialized call_tree() column path
// and the streaming flamegraph/critical-path fold: given ONE lane's events
// already sorted by start (ascending, enclosing-first on a tie), do a single
// nesting-stack pass and, for each event, hand the sink its `level` (stack
// depth) and `parent_handle`. The sink returns the handle to push for this
// event, so a column sink can push the row index (parent_id) while an arena
// sink pushes its node index - the walk stays identical.
//
// `start(k)`/`end(k)` read the interval of the k-th event in the sorted lane
// (end is a functor so a caller can clamp a non-positive duration). An event
// is popped once its end is <= a later event's start. `root` is the parent
// handle at depth 0. `stack` is caller-owned scratch, cleared here and reused
// across lanes so a hot loop allocates nothing. O(n): each event is pushed and
// popped exactly once (the sort is the caller's concern).
// Time is the interval type (Int64 us for the columnar path, double for the
// server's fractional us); Handle is what a sink pushes (row index / arena
// node). Both are deduced from `stack` and `root`, so the pop comparison uses
// the same type the caller sorted by.
template <class Time, class Handle, class Start, class End, class Sink>
void containment_walk(std::int64_t n, Start&& start, End&& end, Handle root,
                      Sink&& sink,
                      dstd::Vector<dstd::Pair<Time, Handle> >& stack) {
    stack.clear();
    for (std::int64_t k = 0; k < n; ++k) {
        const Time s = start(k);
        while (!stack.empty() && stack.back().first <= s) stack.pop_back();
        const std::int64_t level = static_cast<std::int64_t>(stack.size());
        const Handle parent = stack.empty() ? root : stack.back().second;
        stack.push_back({end(k), sink(k, level, parent)});
    }
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_CONTAINMENT_H
