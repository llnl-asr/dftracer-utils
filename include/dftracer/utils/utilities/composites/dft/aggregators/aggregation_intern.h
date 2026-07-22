#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_INTERN_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_INTERN_H

#include <dftracer/utils/core/common/string_intern.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft::aggregators {

/// String ids in aggregation keys are index-local: id 3 is a different string
/// in every index, so a process touching two indexes needs a table per index.
struct AggInternTable {
    StringIntern intern;
    /// Entries already in that index's dictionary; a flush appends past it.
    std::atomic<std::uint32_t> flushed_entries{0};
};

using AggInternPtr = std::shared_ptr<AggInternTable>;

/// Shared by every handle onto `index_path` in this process, so a writer and
/// a reader of one index agree on ids.
AggInternPtr intern_for_index(std::string_view index_path);

/// A table bound to no index, for aggregating without persisting.
AggInternPtr make_intern_table();

/// Make every table created afterwards in this process derive ids from string
/// content, so ranks writing SSTs for one index agree on key bytes. Set before
/// any table is created.
void enable_deterministic_intern_ids();

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_INTERN_H
