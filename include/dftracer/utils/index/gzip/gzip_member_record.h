#ifndef DFTRACER_UTILS_INDEX_GZIP_GZIP_MEMBER_RECORD_H
#define DFTRACER_UTILS_INDEX_GZIP_GZIP_MEMBER_RECORD_H

#include <cstdint>

namespace dftracer::utils::index::gzip {

/// One gzip member: the unit of random access and one-shot decode.
/// Boundaries come from the inflater, not a header scan, so they are exact
/// rather than candidates. The table is an optimization - a reader must
/// tolerate its absence and fall back to
/// `enumerate_gzip_member_candidates`.
struct GzipMemberRecord {
    std::uint64_t member_idx = 0;
    std::uint64_t c_offset = 0;
    std::uint64_t c_size = 0;
    std::uint64_t uc_offset = 0;
    std::uint64_t uc_size = 0;
    std::uint64_t first_line_num = 0;
    std::uint64_t last_line_num = 0;
};

}  // namespace dftracer::utils::index::gzip

#endif  // DFTRACER_UTILS_INDEX_GZIP_GZIP_MEMBER_RECORD_H
