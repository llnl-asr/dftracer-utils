// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_TABLE_TABLE_DECODE_H
#define DFTRACER_UTILS_TRACE_TABLE_TABLE_DECODE_H

#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/table/table_header.h>
#include <simdjson.h>

#include <cstddef>
#include <cstdint>

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
    /// Zeroes the @ref PADDING bytes after `size`, which simdjson may read.
    void seal();
};

/// The engine type a field decodes to: the integer width and Float32 the
/// header names, Float64 for the other floats, Int64 for category labels.
dataframe::TypeId column_type(const Field& field);

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
/// value its dtype cannot take is a Parse error, which names the row counted
/// from `row_base`.
dstd::Result<Decoded> decode_lines(const Header& header,
                                   dstd::Span<const std::size_t> wanted,
                                   dstd::StringView range,
                                   std::size_t parts = 0,
                                   std::size_t row_base = 0);

/// Reads a whole file into one buffer.
dstd::Result<Buffer> read_whole(dstd::StringView path);

/// Whether `bytes` starts with the gzip magic.
bool has_gzip_magic(const unsigned char* bytes, std::size_t size);

/// What a parallel inflate of a gzip file did.
struct InflateStats {
    std::size_t members_in_parallel = 0;
    /// Members inflated one at a time to find their true end.
    std::size_t members_alone = 0;
    bool inflated_in_order = false;
};

/// The whole content of a gzip file, its members inflated in parallel when
/// their marks hold, else one after another.
dstd::Result<Buffer> inflate_gzip(const Buffer& compressed,
                                  InflateStats& stats);

/// Reads a table file a slab of text at a time, so what it holds stays about
/// one slab of text and the compressed bytes under it, whatever the file size.
/// The gzip members of a slab inflate in parallel. When a member mark does not
/// hold (a false mark, a header the scan does not take, a trailer that does not
/// match), the member at the last start that held inflates alone to find its
/// true end, and the parallel inflate goes on after it. A member whose end is
/// not found in the bytes held, or whose text passes the hold, sends the rest
/// of the file to an in-order inflate, still a slab at a time.
class SlabReader {
   public:
    /// `file` must stay open while the reader lives.
    SlabReader(const dstd::File& file, std::uint64_t file_size, bool gzip,
               std::size_t slab_bytes);

    /// The next text, about `slab_bytes` long and cut anywhere, sealed; empty
    /// at the end of the file.
    coro::CoroTask<dstd::Result<Buffer>> next();

    const InflateStats& stats() const { return stats_; }

   private:
    dstd::Result<void> read_more(std::size_t want);
    void scan();
    void start_in_order();
    void drop(std::size_t used);
    dstd::Result<bool> inflate_first(Buffer& out);
    dstd::Result<bool> inflate_ready(Buffer& out);

    const dstd::File& file_;
    std::uint64_t file_size_;
    bool gzip_;
    std::size_t slab_;
    InflateStats stats_;
    // Gzip: file bytes [held_at_, read_at_) in comp_, with the member starts
    // found in it (the first is always 0) and how far it was scanned.
    Buffer comp_;
    std::uint64_t held_at_ = 0;
    std::uint64_t read_at_ = 0;
    dstd::Vector<std::size_t> starts_;
    std::size_t scanned_ = 1;
    dstd::Optional<coro::AsyncGenerator<dstd::Result<dstd::StringView>>>
        in_order_;
};

}  // namespace dftracer::utils::trace::table::detail

#endif  // DFTRACER_UTILS_TRACE_TABLE_TABLE_DECODE_H
