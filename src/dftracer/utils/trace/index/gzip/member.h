// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_GZIP_MEMBER_H
#define DFTRACER_UTILS_TRACE_INDEX_GZIP_MEMBER_H

#include <dftracer/utils/trace/index/gzip/gzip_member_record.h>
#include <dftracer/utils/trace/index/store/queries.h>

#include <cstdint>

namespace dftracer::utils::trace::index::gzip {

using GzipMemberRecord = trace::index::gzip::GzipMemberRecord;
using TimeBounds = trace::index::store::queries::TimeBounds;

/// Uncompressed extent and line range of one pruner chunk. The authoritative
/// chunk -> position mapping for every reader: derived from the gzip member
/// table.
struct ChunkSpan {
    std::uint64_t uc_offset = 0;
    std::uint64_t uc_size = 0;
    std::uint64_t first_line_num = 0;
    std::uint64_t last_line_num = 0;
    bool opens_mid_line = false;
};

}  // namespace dftracer::utils::trace::index::gzip

#endif  // DFTRACER_UTILS_TRACE_INDEX_GZIP_MEMBER_H
