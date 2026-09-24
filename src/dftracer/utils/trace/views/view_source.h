#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_SOURCE_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_SOURCE_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::trace::views {

/// Cursor over a view's buffered scan result: yields max_rows-sized slices of
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

/// What a trace source yields: its view's events or aggregate, or one of the
/// trace terminals over the view's events.
enum class TraceOutput : std::uint8_t {
    Events,
    CallTree,
    Flamegraph,
    FlamegraphPartial,
    AggregatePartial,
    ExportJson,
    Branch
};

/// The lane, interval and label fields of a containment terminal (see
/// View::flamegraph).
struct ContainmentArgs {
    std::vector<std::string> partition{"pid", "tid"};
    std::string ts = "ts";
    std::string dur = "dur";
    std::string name = "name";
    std::vector<std::string> group;
    bool operator==(const ContainmentArgs&) const = default;
};

/// Adapts a scan plan as a LazyFrame Source, so a View can build a
/// LazyFrame without running the scan. An aggregation buffers one cached run
/// of the plan; a plain row query (see can_stream_rows) streams
/// instead, so collect() never buffers the whole matching set.
class ViewSource : public dftracer::utils::dataframe::Source {
   public:
    /// `emit_dyn` makes the streaming cursor attach per-morsel auto-numeric-arg
    /// dyn columns (the aggregation engine's single-scan dyn feed); it is
    /// independent of the plan's row-query classification.

    explicit ViewSource(detail::ScanPlan plan, bool emit_dyn = false)
        : plan_(std::move(plan)), emit_dyn_(emit_dyn) {}

    /// A trace terminal over `view` as a plan leaf. A containment output
    /// takes `tree`; ExportJson writes to `sink` and yields its ExportStats as
    /// one row. A partial yields one
    /// Binary row named "partial". Terminal leaves absorb nothing: the ops
    /// above them apply to the terminal's frame.
    ViewSource(detail::ScanPlan plan, TraceOutput output,
               ContainmentArgs tree = {},
               std::shared_ptr<ExportSink> sink = nullptr)
        : plan_(std::move(plan)),
          output_(output),
          tree_(std::move(tree)),
          sink_(std::move(sink)) {}

    /// A caller branch of the scan as a plan leaf that yields no columns.
    ViewSource(detail::ScanPlan plan, SessionBranch branch)
        : plan_(std::move(plan)),
          output_(TraceOutput::Branch),
          branch_(std::move(branch)) {}

    /// The raw row scan the view's own aggregation engine groups over. It
    /// never absorbs a group-by: that group-by is the view's execution, and
    /// absorbing it would recurse into the same engine.
    static std::shared_ptr<ViewSource> engine_scan(detail::ScanPlan raw,
                                                   bool emit_dyn) {
        auto s = std::make_shared<ViewSource>(std::move(raw), emit_dyn);
        s->absorb_aggregation_ = false;
        return s;
    }

    dftracer::utils::dataframe::Schema schema() const override;
    const dftracer::utils::dataframe::DataFrame* as_frame() const override;

    /// A row query absorbs a filter (the same translation scan() applies, run
    /// at plan time), a plain column projection, and a group-by on fixed
    /// event fields with simple numeric aggregates. Sort, top-k and limits
    /// stay with the engine above the scan.
    std::optional<dftracer::utils::dataframe::SourceApplication> apply_filter(
        const dftracer::utils::dataframe::Expr& predicate) const override;
    std::optional<dftracer::utils::dataframe::SourceApplication>
    apply_projection(const std::vector<dftracer::utils::dataframe::NamedExpr>&
                         exprs) const override;
    std::optional<dftracer::utils::dataframe::SourceApplication>
    apply_aggregation(
        const dftracer::utils::dataframe::AggregateSpec& spec) const override;

    /// Equal for sources that read the same files with the same scan
    /// settings (time range and scale, metadata, index roots, spill budget),
    /// so they can share one ViewSession scan. Branch-local shape (filters,
    /// phase, group-by, aggregates, select, time bucket) is not part of it.
    /// None for a plan the session cannot run as a branch.
    std::optional<std::string> batch_key() const override;
    coro::CoroTask<std::vector<dftracer::utils::dataframe::DataFrame>>
    collect_batch(
        std::vector<std::shared_ptr<const dftracer::utils::dataframe::Source>>
            members) const override;

    /// Row members stream as branches of one ViewSession scan; an aggregation
    /// member arrives as one frame once the scan ends. std::nullopt when a
    /// member's rows cannot stream (a select that needs the name resolver).
    std::optional<
        std::vector<std::unique_ptr<dftracer::utils::dataframe::Cursor>>>
    open_batch(
        std::vector<std::shared_ptr<const dftracer::utils::dataframe::Source>>
            members,
        std::uint64_t memory_budget) const override;

    struct Batch {
        std::vector<dftracer::utils::dataframe::DataFrame> frames;
        ExportStats stats;
    };
    /// Run `members`, which share one batch_key(), as branches of one
    /// ViewSession: an aggregation as collect(), a row query as
    /// collect_events().
    static coro::CoroTask<Batch> run_batch(
        std::vector<std::shared_ptr<const ViewSource>> members);

    /// The plan this source scans.
    const detail::ScanPlan& plan() const { return plan_; }
    TraceOutput output() const { return output_; }
    /// Push projection into the plan's select and each translatable predicate
    /// into its query (reported Exact - the scan filters events, not just
    /// prunes I/O); the untranslatable rest stay No for the engine to apply.
    dftracer::utils::dataframe::ScanResult scan(
        const dftracer::utils::dataframe::ScanRequest& req) const override;

   private:
    /// The session every member of a batch shares: their files and scan
    /// settings, with no member's own shape.
    static ViewSession base_session(const detail::ViewPlan& p);

    /// Register each member not in `skip` as a branch of `session`; the i-th
    /// call reads member i's frame once the session has executed.
    static std::vector<std::function<
        dftracer::utils::dataframe::DataFrame(const ExportStats&)>>
    add_branches(ViewSession& session,
                 const std::vector<const ViewSource*>& members,
                 const std::vector<bool>& skip);

    dftracer::utils::dataframe::Schema compute_schema() const;

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
        const detail::ScanPlan& v, std::uint64_t memory_budget,
        std::vector<std::string> fnames) const;

    std::shared_ptr<const dftracer::utils::dataframe::DataFrame> buffer()
        const {
        if (!buf_)
            buf_ =
                std::make_shared<const dftracer::utils::dataframe::DataFrame>(
                    run_alone().get());
        return buf_;
    }

    /// This source's frame from a scan of its own.
    coro::CoroTask<dftracer::utils::dataframe::DataFrame> run_alone() const;

    detail::ScanPlan plan_;
    TraceOutput output_ = TraceOutput::Events;
    ContainmentArgs tree_;
    std::shared_ptr<ExportSink> sink_;
    SessionBranch branch_;
    bool emit_dyn_ = false;
    // Fingerprints of the predicates already absorbed, so offering one again
    // answers "no change".
    std::vector<std::uint64_t> applied_filters_;
    bool absorb_aggregation_ = true;
    mutable std::shared_ptr<const dftracer::utils::dataframe::DataFrame> buf_;
    // The source never changes, so its schema (index metadata reads for a row
    // query, the buffered result otherwise) is computed once.
    mutable std::mutex schema_mu_;
    mutable std::optional<dftracer::utils::dataframe::Schema> schema_;
};

namespace detail {

/// A partial as the one-row frame a partial terminal yields, and back.
dftracer::utils::dataframe::DataFrame partial_frame(std::string_view partial);
std::string partial_of(const dftracer::utils::dataframe::DataFrame& frame);

/// ExportStats as the one-row frame a sink terminal yields, and back.
dftracer::utils::dataframe::DataFrame stats_frame(const ExportStats& stats);
ExportStats stats_of(const dftracer::utils::dataframe::DataFrame& frame);

}  // namespace detail

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_SOURCE_H
