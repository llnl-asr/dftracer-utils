// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_TABLE_TABLE_DECODE_H
#define DFTRACER_UTILS_TRACE_TABLE_TABLE_DECODE_H

#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/table/table_header.h>
#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dftracer::utils::trace::table::detail {

inline constexpr std::size_t PADDING = simdjson::SIMDJSON_PADDING;

/// Bytes with simdjson padding after them, so a line parses in place.
struct Buffer {
    dstd::UniquePtr<unsigned char[]> data;
    std::size_t size = 0;
    std::size_t capacity = 0;

    static Buffer with_capacity(std::size_t n);
    const char* text() const {
        return reinterpret_cast<const char*>(data.get());
    }
    /// Grows to hold `n` bytes, keeping the first `size` bytes.
    void reserve(std::size_t n);
};

/// The rows of a run of lines, as columns.
struct Decoded {
    dstd::Vector<dataframe::Series> columns;  ///< one per wanted field
    std::size_t rows = 0;
};

/// Decodes the non-blank lines of `range`, one row each, into one column for
/// each field in `wanted` (indexes into `header.fields`, in output order). A
/// key of a row that no wanted field names is skipped unread, so an unwanted
/// cell is never checked. `range` must have @ref PADDING readable bytes after
/// its end. `parts` is how many pieces it is cut into and parsed in parallel;
/// 0 is automatic. A row that is not JSON, lacks a wanted field or holds a
/// value its dtype cannot take is a Parse error.
dstd::Result<Decoded> decode_lines(const Header& header,
                                   dstd::Span<const std::size_t> wanted,
                                   dstd::StringView range,
                                   std::size_t parts = 0);

/// Reads a whole file into one buffer.
dstd::Result<Buffer> read_whole(dstd::StringView path);

/// Whether `bytes` starts with the gzip magic.
bool has_gzip_magic(const unsigned char* bytes, std::size_t size);

/// What a parallel inflate of a gzip file did.
struct InflateStats {
    std::size_t members_in_parallel = 0;
    bool inflated_in_order = false;
    /// The content would pass the caller's limit; nothing was inflated.
    bool exceeded = false;
};

/// The whole content of a gzip file, its members inflated in parallel when
/// their marks hold, else one after another. A content over `max_bytes` is not
/// inflated: the result is empty and `stats.exceeded` is set.
dstd::Result<Buffer> inflate_gzip(
    const Buffer& compressed, InflateStats& stats,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max());

}  // namespace dftracer::utils::trace::table::detail

#endif  // DFTRACER_UTILS_TRACE_TABLE_TABLE_DECODE_H
