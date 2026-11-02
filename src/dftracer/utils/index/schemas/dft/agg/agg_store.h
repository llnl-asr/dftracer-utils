// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/chunk_columns.h>
#include <dftracer/utils/index/store/layout.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The index-wide aggregation records. The records of one file are read through
// tables.h.
namespace dftracer::utils::index {
class Index;
class Snapshot;
}  // namespace dftracer::utils::index

namespace dftracer::utils::index::schemas::dft::agg::tier {

struct Config {
    std::uint64_t time_interval_us = 0;
    /// AggregationConfig::params_hash of the build.
    std::uint64_t params_hash = 0;
    /// Consumers that read fhash off a key check this before concluding the
    /// trace touched no files.
    bool group_by_file = true;
    /// The partials hold their values, joined by 0x1F in this order.
    std::vector<std::string> extra_group_keys;
};

struct TimeBounds {
    std::uint64_t min_bucket = 0;
    std::uint64_t max_bucket = 0;
};

/// nullopt when the index holds no config, or one in another format.
dstd::Expected<dstd::Optional<Config>, dstd::Error> read_config(
    const Snapshot& snapshot);
/// Replaces the index-wide config, blocking until it is published.
void write_config(const Index& index, const Config& config);

/// nullopt when the index holds no time bounds, or ones in another format.
dstd::Expected<dstd::Optional<TimeBounds>, dstd::Error> read_time_bounds(
    const Snapshot& snapshot);
/// Replaces the index-wide time bounds, blocking until they are published.
void write_time_bounds(const Index& index, TimeBounds bounds);

/// Removes every record of the aggregation tier from `index`, the config and
/// the time bounds included. Blocking.
void clear(const Index& index);

}  // namespace dftracer::utils::index::schemas::dft::agg::tier

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
