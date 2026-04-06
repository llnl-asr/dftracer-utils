#ifndef DFTRACER_UTILS_UTILITIES_READER_TRACE_READER_H
#define DFTRACER_UTILS_UTILITIES_READER_TRACE_READER_H

#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/constants.h>
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

/// File-level configuration for TraceReader.
struct TraceReaderConfig {
    std::string file_path;  ///< Path to trace file (.pfw.gz or plain).
    std::string index_dir;  ///< Directory containing `.dftindex` roots.
    std::size_t checkpoint_size = 32 * 1024 * 1024;  ///< Checkpoint interval.
    bool auto_build_index = false;  ///< Auto-build index if missing.
    std::size_t index_threshold =
        constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD;  ///< Min size for
                                                           ///< auto-index.
};

/// Per-read configuration for range, buffering, and query filtering.
struct ReadConfig {
    std::size_t start_line = 0;  ///< First line (1-indexed, 0 = beginning).
    std::size_t end_line = 0;    ///< Last line (0 = end of file).
    std::size_t start_byte = 0;  ///< First byte offset (0 = beginning).
    std::size_t end_byte = 0;    ///< Last byte offset (0 = end of file).

    bool line_aligned = true;    ///< Align raw chunks to line boundaries.
    bool multi_line = true;      ///< Allow multiple lines per raw chunk.

    std::size_t buffer_size = 4 * 1024 * 1024;  ///< Internal read buffer.

    /// Query DSL string for event filtering (empty = no filter).
    /// When set and an index exists, chunk pruning skips non-matching
    /// chunks. Per-event filtering always applies.
    std::string query;

    bool has_line_range() const { return start_line > 0 || end_line > 0; }
    bool has_byte_range() const { return start_byte > 0 || end_byte > 0; }
};

/// Smart trace file reader with auto-detection of sequential vs indexed
/// reading, optional query filtering, and chunk pruning.
class TraceReader {
   public:
    explicit TraceReader(TraceReaderConfig config);

    /// Read lines with optional query filtering and chunk pruning.
    coro::AsyncGenerator<Line> read_lines(ReadConfig config = {});

    /// Read raw byte chunks.
    coro::AsyncGenerator<std::span<const char>> read_raw(
        ReadConfig config = {});

    /// True if a `.dftindex` database was found at construction time.
    bool has_index() const;
    /// Decompressed size (0 if no index for compressed files).
    std::size_t get_max_bytes();
    /// Total line count (0 if no index).
    std::size_t get_num_lines();

   private:
    TraceReaderConfig config_;
    bool has_index_ = false;
    std::string index_path_;
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
