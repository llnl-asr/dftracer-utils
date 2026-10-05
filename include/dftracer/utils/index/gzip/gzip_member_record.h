#ifndef DFTRACER_UTILS_INDEX_GZIP_GZIP_MEMBER_RECORD_H
#define DFTRACER_UTILS_INDEX_GZIP_GZIP_MEMBER_RECORD_H

#include <cstdint>

namespace dftracer::utils::index::gzip {

/// How a reader starts a record: decode a whole member, stream a large member
/// from its gzip header, or stream from a restart point inside a member.
enum class GzipRecordKind : std::uint8_t { MEMBER = 0, HEAD = 1, RESTART = 2 };

/// One gzip member, or one piece of a member larger than the checkpoint size:
/// the unit of random access. Boundaries come from the inflater, not a header
/// scan, so they are exact rather than candidates. The table is an
/// optimization - a reader must tolerate its absence and fall back to
/// `enumerate_gzip_member_candidates`.
///
/// A RESTART piece starts at the first whole byte `c_offset` zlib reads; the
/// top `bits` bits of the byte before it belong to the piece, and its 32 KiB
/// window is stored apart (`IndexDatabase::query_restart_window`). A piece
/// need not start at a line start.
struct GzipMemberRecord {
    std::uint64_t member_idx = 0;
    std::uint64_t c_offset = 0;
    std::uint64_t c_size = 0;
    std::uint64_t uc_offset = 0;
    std::uint64_t uc_size = 0;
    std::uint64_t first_line_num = 0;
    std::uint64_t last_line_num = 0;
    GzipRecordKind kind = GzipRecordKind::MEMBER;
    std::uint8_t bits = 0;
};

}  // namespace dftracer::utils::index::gzip

#endif  // DFTRACER_UTILS_INDEX_GZIP_GZIP_MEMBER_RECORD_H
