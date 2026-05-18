#ifndef DFTRACER_UTILS_UTILITIES_READER_TRACE_READER_H
#define DFTRACER_UTILS_UTILITIES_READER_TRACE_READER_H

#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/utilities/common/json/parser.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#endif

#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace dftracer::utils::utilities::reader {

using common::json::JsonParser;
using fileio::lines::Line;

struct JsonLine {
    std::string_view content;
    std::size_t line_number;
    JsonParser* parser;
};

/// File-level configuration for TraceReader.
struct TraceReaderConfig {
    std::string file_path;  ///< Path to trace file (.pfw.gz or plain).
    std::string index_dir;  ///< Directory containing `.dftindex` roots.
    std::size_t checkpoint_size = 32 * 1024 * 1024;  ///< Checkpoint interval.
    bool auto_build_index = false;  ///< Auto-build index if missing.
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
    /// chunks. Per-event filtering always applies unless chunk_prune_only
    /// is set.
    std::string query;

    /// When true, the query is used only for chunk-level pruning via
    /// the index. Per-line filtering is skipped (caller handles it).
    bool chunk_prune_only = false;

    /// When true, the reader skips its own chunk pruner pass entirely and
    /// trusts the caller's start_line/end_line window. Intended for the
    /// checkpoint-level work-item dispatcher, which already pruned once
    /// per file at enumeration time. Without this the pruner would
    /// re-run per work item (hundreds-of-thousands of RocksDB opens).
    bool skip_pruning = false;

    bool start_at_checkpoint = false;
    bool end_at_checkpoint = false;

    /// When true, top-level object values (e.g. `args`) are expanded one
    /// level into `parent.child` columns with native Arrow types instead
    /// of being serialized as a JSON string column. One-level only; deeper
    /// nesting still round-trips as JSON text under the flattened key.
    bool flatten_objects = false;

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

    /// Read parsed JSON lines. Parses each line once with simdjson ondemand,
    /// applies query filtering, and yields the parsed document.
    /// The yielded JsonParser is valid until the next next() call.
    coro::AsyncGenerator<JsonLine> read_json(ReadConfig config = {});

    /// Read raw byte chunks.
    coro::AsyncGenerator<std::span<const char>> read_raw(
        ReadConfig config = {});

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    /// Direct Arrow batch pipeline: chunk-prune + line-level prefilter +
    /// simdjson iterate_many + inline row build. Yields complete Arrow
    /// record batches sized at `batch_size` rows. Emits the final
    /// partial batch on generator close. Non-normalized schema only
    /// (dynamic columns follow the first row seen).
    coro::AsyncGenerator<common::arrow::ArrowExportResult> read_arrow(
        ReadConfig config = {}, std::size_t batch_size = 10000);
#endif

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
