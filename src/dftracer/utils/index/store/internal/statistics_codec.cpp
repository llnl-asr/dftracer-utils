#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/internal/payload_codec.h>
#include <dftracer/utils/index/store/internal/statistics_codec.h>

namespace dftracer::utils::index::store::internal {

std::string encode_file_scalar_stats_value(
    const index::schemas::dft::ChunkStatistics& stats,
    std::uint64_t num_chunks) {
    std::string value;
    append_u64(value, stats.total_events);
    append_u64(value, stats.min_timestamp_us);
    append_u64(value, stats.max_timestamp_us);
    append_i64(value, stats.duration_sum_us);
    append_u64(value, stats.duration_min_us);
    append_u64(value, stats.duration_max_us);
    append_u64(value, stats.duration_count);
    append_double(value, stats.duration_m2);

    auto duration_sketch = stats.duration_sketch.serialize();
    append_blob(value, duration_sketch);

    auto duration_histogram = stats.duration_histogram.to_json();
    append_string(value, duration_histogram);

    append_u64(value, num_chunks);
    append_u64(value, stats.min_nonzero_timestamp_us);

    return value;
}

index::schemas::dft::MergedStatisticsResult decode_file_scalar_stats_value(
    std::string_view value) {
    if (value.size() < 8) {
        throw IndexerError(IndexerError::Type::DATABASE_ERROR,
                           "Corrupt file scalar statistics value");
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
        stats.duration_sketch =
            utilities::common::statistics::DDSketch::deserialize(
                reinterpret_cast<const std::uint8_t*>(duration_sketch.data()),
                duration_sketch.size());
    }

    auto duration_histogram = cursor.str();
    if (!duration_histogram.empty()) {
        stats.duration_histogram =
            utilities::common::statistics::Log2Histogram::from_json(
                duration_histogram);
    }

    result.num_chunks = cursor.u64();
    stats.min_nonzero_timestamp_us = cursor.u64();

    return result;
}

}  // namespace dftracer::utils::index::store::internal
