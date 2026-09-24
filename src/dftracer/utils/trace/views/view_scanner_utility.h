#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_SCANNER_UTILITY_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_SCANNER_UTILITY_H

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_definition.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#endif

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::trace::views {

struct ViewScannerInput {
    std::string file_path;
    std::string index_path;
    std::size_t checkpoint_size = dftracer::utils::index::gzip::
        CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    std::uint64_t checkpoint_idx = 0;
    std::size_t batch_size = 4 * 1024 * 1024;  // IO buffer size
    std::size_t event_batch_size = 10000;      // events per batch
    ViewDefinition view;
    std::optional<query::Query> query;

    /// Fold mode: when set, the scanner parses each matching event once and
    /// emits an owned interned FoldEvent (in ViewScannerBatch::fold_events)
    /// instead of a string_view, so the fold consumer does not re-parse. The
    /// intern table is shared across workers for consistent ids.
    dftracer::utils::StringIntern* fold_intern = nullptr;
    bool fold_needs_args = false;
    /// Top-level fields (type, ph, ...) to capture raw into each FoldEvent's
    /// args, for schemaless Field group keys the POD does not natively carry.
    /// Non-owning; the pointee outlives the scan. Null = capture nothing.
    const std::vector<std::string>* fold_extra_fields = nullptr;
    /// In fold mode, also keep each matching event's raw line in `events` (for
    /// a raw fold that parses it itself). Ignored when fold_intern is null -
    /// the non-fold scan already yields raw lines in `events`.
    bool fold_keep_raw = false;
    /// In fold mode, enumerate every scalar leaf of each event into
    /// FoldEvent::schema_leaves for schemaless column discovery (the index
    /// build). Ignored when fold_intern is null.
    bool fold_capture_schema = false;

    ViewScannerInput& with_file_path(const std::string& path);
    ViewScannerInput& with_index_path(const std::string& path);
    ViewScannerInput& with_checkpoint_size(std::size_t sz);
    ViewScannerInput& with_byte_range(std::size_t start, std::size_t end);
    ViewScannerInput& with_checkpoint_idx(std::uint64_t idx);
    ViewScannerInput& with_batch_size(std::size_t sz);
    ViewScannerInput& with_event_batch_size(std::size_t sz);
    ViewScannerInput& with_view(const ViewDefinition& v);
};

struct ViewScannerBatch {
    /// Event lines. In stream mode these are string_view into the
    /// decompressed chunk (zero copy, valid until next generator resume).
    /// Metadata events use owned strings stored in owned_events.
    std::vector<std::string_view> events;
    /// Owned storage for metadata events that outlive their source chunk.
    /// Uses deque so push_back doesn't invalidate string_view refs.
    std::deque<std::string> owned_events;
    /// Fold mode (ViewScannerInput::fold_intern set): each matching event
    /// parsed once into an owned FoldEvent, so `events` stays empty.
    std::vector<detail::FoldEvent> fold_events;
    std::uint64_t events_matched = 0;
    std::uint64_t events_scanned = 0;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    utilities::common::arrow::ArrowExportResult to_arrow() const;
    utilities::common::arrow::ArrowExportResult to_arrow(
        utilities::common::arrow::RecordBatchBuilder& builder) const;
#endif
};

struct ViewScannerUtility {
    coro::AsyncGenerator<ViewScannerBatch> operator()(
        const ViewScannerInput& input);
};

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_SCANNER_UTILITY_H
