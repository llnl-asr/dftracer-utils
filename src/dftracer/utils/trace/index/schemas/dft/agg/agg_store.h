// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
#define DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/chunk_columns.h>
#include <dftracer/utils/trace/index/store/layout.h>

#include <cstddef>
#include <cstdint>
#include <utility>

// The index-wide aggregation records. The records of one file are read through
// tables.h.
namespace dftracer::utils::trace::index {
class Index;
class Snapshot;
}  // namespace dftracer::utils::trace::index

namespace dftracer::utils::trace::index::schemas::dft::agg::tier {

struct Config {
    std::uint64_t time_interval_us = 0;
    /// AggregationConfig::params_hash of the build.
    std::uint64_t params_hash = 0;
    /// Consumers that read fhash off a key check this before concluding the
    /// trace touched no files.
    bool group_by_file = true;
    /// The partials hold their values, joined by 0x1F in this order.
    dstd::Vector<dstd::String> extra_group_keys;
    /// The rest of the AggregationConfig the hash covers, so a rebuild
    /// recovers the exact settings.
    bool use_relative_time = false;
    bool normalize_time = false;
    bool compute_statistics = true;
    bool compute_percentiles = false;
    std::uint64_t reference_timestamp = 0;
    double sketch_accuracy = 0.01;
    dstd::Vector<double> percentiles;
};

/// The config record of a build with `config`.
Config make_config(const AggregationConfig& config);
/// The settings `config` was built with; its params_hash is
/// `config.params_hash`.
AggregationConfig aggregation_config_of(const Config& config);

struct TimeBounds {
    std::uint64_t min_bucket = 0;
    std::uint64_t max_bucket = 0;
};

/// nullopt when the index holds no config, or one in another format, such as
/// a record of an earlier layout.
dstd::Expected<dstd::Optional<Config>, dstd::Error> read_config(
    const Snapshot& snapshot);
/// The config a request with `want` is built and read with: the stored one
/// when it reads back exactly and covers `want`, else `want`.
dstd::Result<AggregationConfig> serving_config(const Snapshot& snapshot,
                                               const AggregationConfig& want);
/// Whether the index holds a config record, readable or not.
dstd::Expected<bool, dstd::Error> has_config_record(const Snapshot& snapshot);
/// Replaces the index-wide config, blocking until it is published. An extra
/// group key that is empty or holds 0x1F is an error: the record could not
/// give it back.
dstd::Result<void> write_config(const Index& index, const Config& config);

/// nullopt when the index holds no time bounds, or ones in another format.
dstd::Expected<dstd::Optional<TimeBounds>, dstd::Error> read_time_bounds(
    const Snapshot& snapshot);
/// Replaces the index-wide time bounds, blocking until they are published.
dstd::Result<void> write_time_bounds(const Index& index, TimeBounds bounds);

/// Removes every record of the aggregation tier from `index`, the config and
/// the time bounds included. Blocking.
dstd::Result<void> clear(const Index& index);

/// Removes the aggregation records of the files in `paths`, their logical
/// paths, and nothing else: the records of other files, the config and the
/// time bounds stay. A path with no records is skipped. Blocking; the views
/// need only outlive the call.
dstd::Result<void> clear_files(const Index& index,
                               dstd::Span<const dstd::StringView> paths);

}  // namespace dftracer::utils::trace::index::schemas::dft::agg::tier

#endif  // DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_AGG_AGG_STORE_H
