// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/internal/index_decoding.h>
#include <dftracer/utils/index/store/internal/index_encoding.h>
#include <dftracer/utils/index/store/internal/payload_codec.h>

#include <string>

namespace dftracer::utils::index::store::internal::decoding {

index::schemas::dft::ChunkStatistics decode_chunk_statistics_value(
    dstd::StringView value) {
    Cursor cursor(value);
    index::schemas::dft::ChunkStatistics stats;
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

    auto name_sketches = cursor.blob_view();
    if (!name_sketches.empty()) {
        stats.name_duration_sketches = index::schemas::dft::ChunkStatistics::
            deserialize_name_duration_sketches(
                reinterpret_cast<const std::uint8_t*>(name_sketches.data()),
                name_sketches.size());
    }

    stats.name_duration_histograms =
        index::schemas::dft::ChunkStatistics::parse_histogram_map_json(
            cursor.str());
    stats.name_duration_sums =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());
    stats.name_duration_sum_sqs =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());
    stats.name_category =
        index::schemas::dft::ChunkStatistics::parse_string_map_json(
            cursor.str());

    auto cat_sketches = cursor.blob_view();
    if (!cat_sketches.empty()) {
        stats.cat_duration_sketches = index::schemas::dft::ChunkStatistics::
            deserialize_name_duration_sketches(
                reinterpret_cast<const std::uint8_t*>(cat_sketches.data()),
                cat_sketches.size());
    }
    stats.cat_duration_sums =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());

    auto pid_sketches = cursor.blob_view();
    if (!pid_sketches.empty()) {
        stats.pid_duration_sketches = index::schemas::dft::ChunkStatistics::
            deserialize_name_duration_sketches(
                reinterpret_cast<const std::uint8_t*>(pid_sketches.data()),
                pid_sketches.size());
    }
    stats.pid_duration_sums =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());

    return stats;
}

index::gzip::GzipMemberRecord decode_gzip_member(std::uint32_t member_idx,
                                                 dstd::StringView body) {
    index::gzip::GzipMemberRecord member;
    member.member_idx = member_idx;
    Cursor cursor(body);
    member.c_offset = cursor.u64();
    member.c_size = cursor.u64();
    member.uc_offset = cursor.u64();
    member.uc_size = cursor.u64();
    member.first_line_num = cursor.u64();
    member.last_line_num = cursor.u64();
    if (body.size() >= 6 * sizeof(std::uint64_t) + 2) {
        const auto kind = cursor.u8();
        if (kind >
            static_cast<std::uint8_t>(index::gzip::GzipRecordKind::Restart))
            throw common::DFTUtilsException::cat(
                common::ErrorCode::Indexer, "unknown gzip record kind ", kind);
        member.kind = static_cast<index::gzip::GzipRecordKind>(kind);
        const auto bits = cursor.u8();
        member.bits = bits & static_cast<std::uint8_t>(
                                 ~encoding::MEMBER_FLAG_OPENS_MID_LINE);
        member.opens_mid_line =
            (bits & encoding::MEMBER_FLAG_OPENS_MID_LINE) != 0;
    }
    return member;
}

dstd::StringViewMap<std::uint64_t> decode_count_map_value(
    dstd::StringView value) {
    Cursor cursor(value);
    dstd::StringViewMap<std::uint64_t> counts;
    auto entry_count = cursor.u32();
    counts.reserve(entry_count);
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        auto key = cursor.str();
        counts.emplace(std::move(key), cursor.u64());
    }
    return counts;
}

FileMetadataResult decode_metadata_record(dstd::StringView value) {
    Cursor cursor(value);
    FileMetadataResult meta;
    meta.checkpoint_size = cursor.u64();
    meta.line_count = cursor.u64();
    meta.max_bytes = cursor.u64();
    const std::uint64_t flags = cursor.u64();
    meta.truncated = (flags & encoding::METADATA_FLAG_TRUNCATED) != 0;
    meta.has_open_line = (flags & encoding::METADATA_FLAG_OPEN_LINE) != 0;
    meta.has_mid_line_chunk =
        (flags & encoding::METADATA_FLAG_MID_LINE_CHUNK) != 0;
    meta.next_member_idx = cursor.u64();
    return meta;
}

}  // namespace dftracer::utils::index::store::internal::decoding
