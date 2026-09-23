#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_SOURCE_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_SOURCE_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::trace::views {

/// Cursor over a View's buffered scan result: yields max_rows-sized slices of
/// the buffer, zero-copy. A buffer with any nested (List/Struct) column
/// - a histogram agg produces one - is handed out as a single whole morsel
/// instead, since concat_columns can't rejoin a nested column across chunks.
class ViewCursor : public dftracer::utils::dataframe::Cursor {
   public:
    explicit ViewCursor(
        std::shared_ptr<const dftracer::utils::dataframe::DataFrame> buf)
        : buf_(std::move(buf)) {}

    coro::CoroTask<std::optional<dftracer::utils::dataframe::Morsel>> next(
        std::int64_t max_rows) override;

   private:
    bool has_nested_column() const;

    std::shared_ptr<const dftracer::utils::dataframe::DataFrame> buf_;
    std::int64_t offset_ = 0;
    std::int64_t next_index_ = 0;
    mutable std::optional<bool> nested_;
};

/// Adapts a View as a LazyFrame Source, so View::collect() can build a
/// LazyFrame without running the scan. An aggregation buffers one cached run
/// of View::collect_frame(); a plain row query (see can_stream_rows) streams
/// instead, so collect() never buffers the whole matching set.
class ViewSource : public dftracer::utils::dataframe::Source {
   public:
    /// `emit_dyn` makes the streaming cursor attach per-morsel auto-numeric-arg
    /// dyn columns (the aggregation engine's single-scan dyn feed); it is
    /// independent of the plan's row-query classification.
    explicit ViewSource(View view, bool emit_dyn = false)
        : view_(std::move(view)), emit_dyn_(emit_dyn) {}

    dftracer::utils::dataframe::Schema schema() const override;
    const dftracer::utils::dataframe::DataFrame* as_frame() const override;
    /// Push projection into View::select and each translatable predicate into
    /// the View's query (reported Exact - the View filters events, not just
    /// prunes I/O); the untranslatable rest stay No for the engine to apply.
    dftracer::utils::dataframe::ScanResult scan(
        const dftracer::utils::dataframe::ScanRequest& req) const override;

   private:
    /// A row query with no sort/topk/pagination/select - the post-scan ops
    /// run_collect_rows applies after the fuse, which streaming cannot.
    bool can_stream_rows() const;

    /// Best-effort column list for the streaming source's schema(), from index
    /// metadata rather than a scan.
    std::vector<std::string> row_schema() const;

    /// Open a bounded-channel streaming cursor over `v` (a row query). Used by
    /// scan() once projection + filters are folded into `v`. `fnames` is the
    /// column list the cursor's morsels carry (schema() when no projection
    /// was pushed), so the returned cursor's narrow() can translate a
    /// predicate positional against it the same way scan() does.
    std::unique_ptr<dftracer::utils::dataframe::Cursor> open_stream(
        const View& v, std::uint64_t memory_budget,
        std::vector<std::string> fnames) const;

    std::shared_ptr<const dftracer::utils::dataframe::DataFrame> buffer()
        const {
        if (!buf_)
            buf_ =
                std::make_shared<const dftracer::utils::dataframe::DataFrame>(
                    view_.collect_frame().get());
        return buf_;
    }

    View view_;
    bool emit_dyn_ = false;
    mutable std::shared_ptr<const dftracer::utils::dataframe::DataFrame> buf_;
};

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_SOURCE_H
