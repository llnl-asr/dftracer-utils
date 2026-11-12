// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_SCANNER_UTILITY_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_SCANNER_UTILITY_H

#include <dftracer/utils/common/config.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/intern_cache.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_definition.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/arrow/arrow_export.h>
#include <dftracer/utils/trace/arrow/column_builder.h>
#endif

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::trace::views {

struct ViewScannerInput {
    dstd::String file_path;
    dstd::String index_path;
    std::size_t checkpoint_size = dftracer::utils::trace::index::gzip::
        CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    std::uint64_t checkpoint_idx = 0;
    /// Reads through this indexer when set. Tasks over one file share it, so
    /// its member table loads once.
    dstd::SharedPtr<dftracer::utils::trace::index::gzip::CheckpointIndexer>
        indexer;
    std::size_t batch_size = 4 * 1024 * 1024;  // IO buffer size
    std::size_t event_batch_size = 10000;      // events per batch
    ViewDefinition view;
    dstd::Optional<duql::Query> query;

    /// Fold mode: when set, the scanner parses each matching event once and
    /// emits an owned interned FoldEvent (in ViewScannerBatch::fold_events)
    /// instead of a string_view, so the fold consumer does not re-parse. The
    /// intern table is shared across workers for consistent ids.
    dftracer::utils::dstd::StringIntern* fold_intern = nullptr;
    /// Where fold mode decodes events. The caller keeps one per worker across
    /// units so its slots keep their capacity; null uses a buffer private to
    /// the scan. The events a batch exposes live here until the generator is
    /// resumed.
    detail::FoldEventBuffer* fold_buffer = nullptr;
    /// A cache in front of `fold_intern` that the caller keeps per worker, so
    /// it stays warm across units. Null makes one for the scan.
    dftracer::utils::dstd::InternCache* fold_cache = nullptr;
    bool fold_needs_args = false;
    /// Top-level fields (type, ph, ...) to capture raw into each FoldEvent's
    /// args, for schemaless Field group keys the POD does not natively carry.
    /// Non-owning; the pointee outlives the scan. Null = capture nothing.
    const dstd::Vector<dstd::String>* fold_extra_fields = nullptr;
    /// In fold mode, also keep each matching event's raw line in `events` (for
    /// a raw fold that parses it itself). Ignored when fold_intern is null -
    /// the non-fold scan already yields raw lines in `events`.
    bool fold_keep_raw = false;
    /// In fold mode, enumerate every scalar leaf of each event into
    /// FoldEvent::schema_leaves for schemaless column discovery (the index
    /// build). Ignored when fold_intern is null.
    bool fold_capture_schema = false;
    /// With fold_capture_schema, leave the values at `never_index` paths out
    /// of the decoded events; set only when no fold reads them.
    bool fold_skip_never_indexed = false;

    ViewScannerInput& with_file_path(const dstd::String& path);
    ViewScannerInput& with_index_path(const dstd::String& path);
    ViewScannerInput& with_checkpoint_size(std::size_t sz);
    ViewScannerInput& with_byte_range(std::size_t start, std::size_t end);
    ViewScannerInput& with_checkpoint_idx(std::uint64_t idx);
    ViewScannerInput& with_batch_size(std::size_t sz);
    ViewScannerInput& with_event_batch_size(std::size_t sz);
    ViewScannerInput& with_view(const ViewDefinition& v);
};

struct ExportStats;
struct ViewScannerBatch;

/// One worker's share of a scan's counts.
struct ScanCounts {
    std::uint64_t events_matched = 0;
    std::uint64_t events_scanned = 0;
    std::uint64_t lines_invalid = 0;
    std::uint64_t values_unconverted = 0;

    void add(const ViewScannerBatch& b);
    void add_to(ExportStats& st) const;
};

struct ViewScannerBatch {
    /// Event lines. In stream mode these are string_view into the
    /// decompressed chunk (zero copy, valid until next generator resume).
    /// Metadata events use owned strings stored in owned_events.
    dstd::Vector<dstd::StringView> events;
    /// Owned storage for metadata events that outlive their source chunk.
    /// Uses deque so push_back doesn't invalidate string_view refs.
    dstd::Deque<dstd::String> owned_events;
    /// Fold mode (ViewScannerInput::fold_intern set): each matching event
    /// parsed once into an owned FoldEvent, so `events` stays empty. Borrowed
    /// from the scan's buffer; valid until the next generator resume.
    dstd::Span<const detail::FoldEvent> fold_events;
    std::uint64_t events_matched = 0;
    std::uint64_t events_scanned = 0;
    std::uint64_t lines_invalid = 0;
    std::uint64_t values_unconverted = 0;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    dstd::Result<trace::arrow::ArrowExportResult> to_arrow() const;
    dstd::Result<trace::arrow::ArrowExportResult> to_arrow(
        trace::arrow::RecordBatchBuilder& builder) const;
#endif
};

inline void ScanCounts::add(const ViewScannerBatch& b) {
    events_matched += b.events_matched;
    events_scanned += b.events_scanned;
    lines_invalid += b.lines_invalid;
    values_unconverted += b.values_unconverted;
}

struct ViewScannerUtility {
    /// The batches of one unit. A failed read, or a lookup that failed on a
    /// record, is a last error result, after which the generator ends.
    coro::AsyncGenerator<dstd::Result<ViewScannerBatch>> operator()(
        const ViewScannerInput& input);
};

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_SCANNER_UTILITY_H
