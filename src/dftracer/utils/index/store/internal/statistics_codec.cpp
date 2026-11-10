// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/internal/payload_codec.h>
#include <dftracer/utils/index/store/internal/statistics_codec.h>

#include <string>

namespace dftracer::utils::index::store::internal {

dstd::String encode_file_scalar_stats_value(
    const index::schemas::dft::ChunkStatistics& stats,
    std::uint64_t chunk_count) {
    dstd::String value;
    dstd::append_u64_be(value, stats.total_events);
    dstd::append_u64_be(value, stats.min_timestamp_us);
    dstd::append_u64_be(value, stats.max_timestamp_us);
    dstd::append_u64_be(value,
                        static_cast<std::uint64_t>(stats.duration_sum_us));
    dstd::append_u64_be(value, stats.duration_min_us);
    dstd::append_u64_be(value, stats.duration_max_us);
    dstd::append_u64_be(value, stats.duration_count);
    dstd::append_f64_be(value, stats.duration_m2);

    auto duration_sketch = stats.duration_sketch.serialize();
    append_blob(value, duration_sketch);

    auto duration_histogram = stats.duration_histogram.to_json();
    append_string(value, duration_histogram);

    dstd::append_u64_be(value, chunk_count);
    dstd::append_u64_be(value, stats.min_nonzero_timestamp_us);

    return value;
}

dstd::Result<index::schemas::dft::MergedStatisticsResult>
decode_file_scalar_stats_value(dstd::StringView value) {
    if (value.size() < 8) {
        return indexer_error(IndexerErrorKind::DatabaseError,
                             "Corrupt file scalar statistics value",
                             common::ErrorCode::Parse);
    }
    Cursor cursor(value);
    index::schemas::dft::MergedStatisticsResult result;
    auto& stats = result.stats;
    stats.total_events = cursor.u64();
    stats.min_timestamp_us = cursor.u64();
    stats.max_timestamp_us = cursor.u64();
    stats.duration_sum_us = cursor.i64();
    stats.duration_min_us = cursor.u64();
    stats.duration_max_us = cursor.u64();
    stats.duration_count = cursor.u64();
    stats.duration_m2 = cursor.f64();

    auto duration_sketch = cursor.blob_view();
    if (!duration_sketch.empty()) {
        stats.duration_sketch = dataframe::DDSketch::deserialize(
            reinterpret_cast<const std::uint8_t*>(duration_sketch.data()),
            duration_sketch.size());
    }

    auto duration_histogram = cursor.str();
    if (!duration_histogram.empty()) {
        stats.duration_histogram = dataframe::Log2Histogram::from_json(
            dstd::String(duration_histogram));
    }

    result.chunk_count = cursor.u64();
    stats.min_nonzero_timestamp_us = cursor.u64();

    if (cursor.failed()) return cursor.error();
    return result;
}

}  // namespace dftracer::utils::index::store::internal
