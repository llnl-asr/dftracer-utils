// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_STATISTICS_CODEC_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_STATISTICS_CODEC_H

#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/types.h>

#include <cstdint>

namespace dftracer::utils::index::store::internal {

dstd::String encode_file_scalar_stats_value(
    const index::schemas::dft::ChunkStatistics& stats,
    std::uint64_t chunk_count);

dstd::Result<index::schemas::dft::MergedStatisticsResult>
decode_file_scalar_stats_value(dstd::StringView value);

}  // namespace dftracer::utils::index::store::internal

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_STATISTICS_CODEC_H
