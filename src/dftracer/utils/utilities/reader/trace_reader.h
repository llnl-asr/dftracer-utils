// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_READER_TRACE_READER_H
#define DFTRACER_UTILS_UTILITIES_READER_TRACE_READER_H

#include <dftracer/utils/common/archive_format.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/json/parser.h>
#include <dftracer/utils/trace/time_metric.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/arrow/arrow_export.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::utilities::reader {

using fileio::lines::Line;
using json::JsonParser;

struct JsonLine {
    dstd::StringView content;
    std::size_t line_number;
    JsonParser* parser;
};

/// Target output unit for `ts`/`dur` (see ReadConfig::normalize_time). `None`
/// returns the file's native unit unchanged.
enum class TimeNormalization {
    None = 0,
    Nanoseconds = 1,
    Microseconds = 2,
    Milliseconds = 3,
    Seconds = 4,
};

/// File-level configuration for @ref TraceReader.
struct TraceReaderConfig {
    dstd::String file_path;  ///< Path to trace file (.pfw.gz or plain).
    dstd::String index_dir;  ///< Directory containing `.dftindex` roots.
    std::size_t checkpoint_size =
        common::constants::indexer::DEFAULT_CHECKPOINT_SIZE;  ///< Checkpoint
                                                              ///< interval.
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
    dstd::String query;

    /// When true, the query is used only for chunk-level pruning via
    /// the index. Per-line filtering is skipped (caller handles it).
    bool chunk_prune_only = false;

    /// When true, the reader skips its own chunk pruner pass entirely and
    /// trusts the caller's start_line/end_line window. Intended for the
    /// checkpoint-level work-item dispatcher, which already pruned once
    /// per file at enumeration time. Without this the pruner would
    /// re-run per work item (hundreds of thousands of index opens).
    bool skip_pruning = false;

    bool start_at_checkpoint = false;
    bool end_at_checkpoint = false;

    /// When true, top-level object values (e.g. `args`) are expanded one
    /// level into `parent.child` columns with native Arrow types instead
    /// of being serialized as a JSON string column. One-level only; deeper
    /// nesting still round-trips as JSON text under the flattened key.
    bool flatten_objects = false;

    /// Target output unit for `ts`/`dur`. Defaults to `None` (native unit
    /// unchanged). Any other value scales from the file's declared `CM`
    /// time_metric (absent = us) to that unit. Query DSL predicates always
    /// match the native index and are unaffected.
    TimeNormalization normalize_time = TimeNormalization::None;

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
    coro::AsyncGenerator<dstd::Span<const char>> read_raw(
        ReadConfig config = {});

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    /// Direct Arrow batch pipeline: chunk-prune + line-level prefilter +
    /// per-line in-place parsing + inline row build. Yields complete Arrow
    /// record batches sized at `batch_size` rows. Emits the final
    /// partial batch on generator close. Non-normalized schema only
    /// (dynamic columns follow the first row seen).
    coro::AsyncGenerator<arrow::ArrowExportResult> read_arrow(
        ReadConfig config = {}, std::size_t batch_size = 10000);
#endif

    /// Resolve the file's native time unit from its leading `CM` time_metric
    /// metadata by scanning up to `max_lines` header lines. Returns US when no
    /// `CM` is declared. Independent of any query filter.
    coro::CoroTask<trace::TimeMetric> read_time_metric(
        std::size_t max_lines = 256);

    /// True if a `.dftindex` database was found at construction time.
    bool has_index() const;
    /// Decompressed size (0 if no index for compressed files).
    std::size_t get_max_bytes();
    /// Total line count (0 if no index).
    std::size_t get_num_lines();

    /// How many lines @ref read_json and @ref read_arrow could not read in full
    /// since this reader was made: lines that are no JSON or are cropped, and
    /// lines broken in a part the query read. A counted line may still reach
    /// the caller: @ref read_json with no query hands over every line as text,
    /// and a line that is valid JSON but no object is not counted there. @ref
    /// read_arrow reads every field, so it also counts a line that is no
    /// object, which has no row, and a line with a field it could not read,
    /// whose readable fields stay in a row. A blank line or a lone bracket is
    /// not a line. A line the query did not match is not counted, and neither
    /// is a line the index pruned or the line prefilter skipped without
    /// parsing. The count of a read is final when its generator has run to its
    /// end; one warning with the path and that count is logged then. A
    /// generator that is dropped early has counted only the lines it passed.
    /// Safe to call from any thread.
    std::size_t dropped_line_count() const noexcept;

   private:
    TraceReaderConfig config_;
    bool has_index_ = false;
    dstd::String index_path_;
    common::ArchiveFormat format_ = common::ArchiveFormat::Unknown;
    std::size_t cached_max_bytes_ = 0;
    std::size_t cached_num_lines_ = 0;
    bool metadata_cached_ = false;

    std::atomic<std::size_t> dropped_lines_{0};

    void probe_index();
    void ensure_metadata_cached();
    // Adds `count` unreadable lines to this reader's total and to `local`, the
    // count of the read that found them.
    void count_dropped(std::size_t& local, std::size_t count = 1) noexcept;
    // Logs one warning for the `local` unreadable lines of a finished read.
    void warn_dropped(std::size_t local) const;

    dstd::SharedPtr<internal::Reader> create_indexed_reader();

    internal::StreamType resolve_raw_stream_type(
        const ReadConfig& config) const;

    internal::RangeType resolve_range_type(const ReadConfig& config) const;
};

}  // namespace dftracer::utils::utilities::reader

#endif  // DFTRACER_UTILS_UTILITIES_READER_TRACE_READER_H
