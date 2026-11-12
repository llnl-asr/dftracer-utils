// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_INTERN_H
#define DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_INTERN_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>

#include <cstdint>
#include <string_view>

namespace dftracer::utils::trace::index::schemas::dft::agg {

/// String ids in aggregation keys are index-local: id 3 is a different string
/// in every index, so a process touching two indexes needs a table per index.
struct AggInternTable {
    dstd::StringIntern intern;
};

using AggInternPtr = dstd::SharedPtr<AggInternTable>;

/// Shared by every handle onto `index_path` in this process, so a writer and
/// a reader of one index agree on ids.
AggInternPtr intern_for_index(dstd::StringView index_path);

/// A table bound to no index, for aggregating without persisting.
AggInternPtr make_intern_table();

/// Make every table created afterwards in this process derive ids from string
/// content, so ranks writing one index agree on key bytes. Call before any
/// table is created.
void enable_deterministic_intern_ids();

}  // namespace dftracer::utils::trace::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_INTERN_H
