#ifndef DFTRACER_UTILS_INDEX_GZIP_MEMBER_H
#define DFTRACER_UTILS_INDEX_GZIP_MEMBER_H

#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/index/store/queries.h>

#include <cstdint>

namespace dftracer::utils::index::gzip {

using GzipMemberRecord = index::gzip::GzipMemberRecord;
using TimeBounds = index::store::queries::TimeBounds;

/// Uncompressed extent and line range of one pruner chunk. The authoritative
/// chunk -> position mapping for every reader: derived from the gzip member
/// table.
struct ChunkSpan {
    std::uint64_t uc_offset = 0;
    std::uint64_t uc_size = 0;
    std::uint64_t first_line_num = 0;
    std::uint64_t last_line_num = 0;
};

}  // namespace dftracer::utils::index::gzip

#endif  // DFTRACER_UTILS_INDEX_GZIP_MEMBER_H
