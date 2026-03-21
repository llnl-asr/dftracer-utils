#ifndef DFTRACER_UTILS_UTILITIES_READER_TRACE_READER_H
#define DFTRACER_UTILS_UTILITIES_READER_TRACE_READER_H

#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace dftracer::utils::utilities::reader {

using fileio::lines::Line;

struct TraceReaderConfig {
    std::string file_path;
    std::string index_dir;
    std::size_t checkpoint_size = 32 * 1024 * 1024;
    bool auto_build_index = false;
    std::size_t index_threshold = 8 * 1024 * 1024;
};

struct ReadConfig {
    std::size_t start_line = 0;
    std::size_t end_line = 0;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;

    bool line_aligned = true;
    bool multi_line = true;

    std::size_t buffer_size = 4 * 1024 * 1024;

    // Query DSL string for event filtering (empty = no filter).
    // When set and an index exists, chunk pruning skips non-matching chunks.
    // Per-event filtering always applies.
    std::string query;

    bool has_line_range() const { return start_line > 0 || end_line > 0; }
    bool has_byte_range() const { return start_byte > 0 || end_byte > 0; }
};

class TraceReader {
   public:
    explicit TraceReader(TraceReaderConfig config);

    // Parsed lines
    coro::AsyncGenerator<Line> read_lines(ReadConfig config = {});

    // Raw bytes
    coro::AsyncGenerator<std::span<const char>> read_raw(
        ReadConfig config = {});

    bool has_index() const;
    std::size_t get_max_bytes();
    std::size_t get_num_lines();

   private:
    TraceReaderConfig config_;
    bool has_index_ = false;
    std::string idx_path_;
    ArchiveFormat format_ = ArchiveFormat::UNKNOWN;
    std::size_t cached_max_bytes_ = 0;
    std::size_t cached_num_lines_ = 0;
    bool metadata_cached_ = false;

    void probe_index();
    void ensure_metadata_cached();

    std::shared_ptr<internal::Reader> create_indexed_reader();

    internal::StreamType resolve_raw_stream_type(
        const ReadConfig& config) const;

    internal::RangeType resolve_range_type(const ReadConfig& config) const;
};

}  // namespace dftracer::utils::utilities::reader

#endif  // DFTRACER_UTILS_UTILITIES_READER_TRACE_READER_H
