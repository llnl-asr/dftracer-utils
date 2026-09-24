#ifndef DFTRACER_UTILS_TRACE_VIEWS_NATIVE_ROW_FOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_NATIVE_ROW_FOLD_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index::plan {
class GroupResolver;
}  // namespace dftracer::utils::index::plan

namespace dftracer::utils::trace::views::detail {

/// Sentinel select tokens the agg engine uses to ask build_row_frame for a
/// group-key STRING column rendered exactly as the engine agg path builds its
/// key (PodSource append_arg for an Arg key, append_value for a Field key): ""
/// for a missing value, numbers stringified. Internal to the agg-engine <->
/// row-fold seam; never a user-facing select.
inline constexpr std::string_view AGG_KEY_ARG_PREFIX = "__aggkey_arg:";
inline constexpr std::string_view AGG_KEY_FIELD_PREFIX = "__aggkey_field:";

/// Sentinel select token the agg engine uses to ask build_row_frame for a
/// numeric-only Float64 VALUE column for one auto-discovered numeric arg (the
/// auto_numeric_metrics / numeric_arg_aggs dyn path). The column carries the
/// arg's numeric value where the event holds it as a number and null otherwise,
/// matching the engine agg path's per-arg FieldStat (fold_numeric_args_t, which
/// feeds only PodSource::for_each_numeric_arg values). The special name "size"
/// resolves to the io-cat-derived byte size (derived_size_t), falling back to a
/// literal numeric "size" arg. Internal to the agg-engine <-> row-fold seam.
inline constexpr std::string_view AGG_NUM_ARG_PREFIX = "__aggnum_arg:";

/// Sentinel select token the agg engine uses to ask build_row_frame for a
/// fold-DERIVED typed VALUE column: "size" is the io-cat-derived byte size,
/// "te" is ts+dur. Built with the U64 domain agg_field_typed_t assigns, so a
/// Sum/Min/Max over it matches the engine agg path column-for-column. Internal
/// to the agg-engine <-> row-fold seam; never a user-facing select.
inline constexpr std::string_view AGG_DERIVED_PREFIX = "__aggderiv:";

/// Select tokens for an aggregation over the window [lo, hi) of raw ts.
/// window_token(lo, hi, sel) is `sel`'s column, null for events that do not
/// start in the window; an empty `sel` gives an Int64 column of 1.
/// clip_token(lo, hi, "ts" or "dur") is the event's [ts, ts + dur) clamped to
/// the window, as a Uint64 start or length. Internal to the agg-engine <->
/// row-fold seam.
std::string window_token(std::uint64_t lo, std::uint64_t hi,
                         std::string_view sel);
std::string clip_token(std::uint64_t lo, std::uint64_t hi,
                       std::string_view part);
/// The record field a select token reads (an agg-engine token's field, a
/// window token's inner field), or `sel` itself; empty for a clip token.
std::string_view select_source_field(std::string_view sel);

/// The `sel` a window_token wraps, or `sel` itself.
std::string_view window_inner(std::string_view sel);
bool is_clip_token(std::string_view sel);

/// Build one native DataFrame from `events`: top-level columns plus every arg
/// (empty `select`) or a projected subset. Arg columns infer their type per key
/// and null-fill absent rows. `fhash`/`hhash` resolve from their dedicated
/// fields; a selected `resolved.<key>.<field>` column resolves through
/// `resolver` (the index dictionaries), or is all-null when it is null. With
/// `by_path` (path-decoded records) every column is a field named by its
/// exact path, with no fixed columns. Shared by the materialized collect and
/// the streaming chunk builder.
dataframe::DataFrame build_row_frame(
    const std::vector<FoldEvent>& events,
    const dftracer::utils::StringIntern& intern,
    const std::vector<std::string>& select, double time_scale = 1.0,
    const dftracer::utils::index::plan::GroupResolver* resolver = nullptr,
    bool by_path = false);

/// The per-batch auto-numeric dyn value columns for `events`: one Float64
/// column per discovered numeric arg (fold_numeric_args_t's rule - the
/// io-cat-derived "size" plus every non-reserved, non-preagg numeric arg),
/// each named `AGG_NUM_ARG_PREFIX + arg`. The set varies batch to batch; the
/// streaming aggregation carries these out of band (Morsel::dyn_*) so the fixed
/// column layout is unchanged. Empty when the batch holds no numeric arg.
/// With a window or clip token in `select`, only the events that start in its
/// window contribute.
std::vector<std::pair<std::string, dataframe::Series>>
build_dyn_numeric_columns(const std::vector<FoldEvent>& events,
                          const dftracer::utils::StringIntern& intern,
                          const std::vector<std::string>& select);

/// The `resolved.` columns `select` names; the caller builds a GroupResolver
/// (which reads the index dictionaries) for build_row_frame when non-empty.
std::vector<std::string> select_resolved(
    const std::vector<std::string>& select);

/// The non-scalar fields a `select` needs the scan to capture: nested paths and
/// non-POD top-level fields the scan does not carry by default (args, nested
/// ones flattened, come from needs_args, POD scalars are inherent). An agg
/// group-key sentinel is mapped to its underlying real field. A row fold
/// returns this from extra_captures() so build_row_frame's select branch can
/// resolve those fields.
std::vector<std::string> row_fold_extra_captures(
    const std::vector<std::string>& select);

/// The output column name build_row_frame's select branch gives `sel`: a
/// top-level field or resolved column keeps its own name;
/// fhash/hhash keep their bare name; anything else is an arg, flat or a nested
/// dotted path such as "dur.p99", and is canonicalized to "args.<key>"
/// (accepting `sel` bare or "args."-prefixed).
/// ViewSource::names() uses this so a streamed morsel's schema always matches
/// what build_row_frame actually emits for the same select.
std::string canonical_row_column_name(std::string_view sel,
                                      bool by_path = false);

/// The statically known type of the column `canonical_row_column_name(sel)`
/// names, or TypeId::Unknown when it is data-dependent (a flattened args.*
/// value, whose type build_row_frame infers per batch). Uses the same field
/// classification as canonical_row_column_name/build_row_frame, so it cannot
/// drift from what the scan actually emits.
dataframe::TypeId row_column_type(std::string_view sel, bool by_path = false);

/// Collect matching raw events straight into a native DataFrame. The
/// row-query terminal (collect() / stream() with no group_by/agg) folds every
/// event's top-level fields plus its args into columns, a nested object or
/// array arg as one column per scalar leaf ("args.dur.p99", "args.hosts.1");
/// `select` projects a subset (top-level names or arg keys, bare or
/// "args."-prefixed), and an empty
/// `select` emits every column (the union of all args seen, null-filled where
/// an event lacks a key). ph="M" metadata is skipped unless `keep_metadata` is
/// set (phase("metadata")), which emits the records as rows with their args
/// flattened like any event's.
///
/// build() materializes one frame over every accumulated event, so the column
/// union is exact and no cross-slot schema reconciliation is needed. Types are
/// inferred per arg column: int64 unless a real forces Float64, or a string
/// value forces String (numbers stringified).
class NativeRowFold : public Fold {
   public:
    NativeRowFold(
        const dftracer::utils::StringIntern& intern,
        std::vector<std::string> select, double time_scale = 1.0,
        std::shared_ptr<const dftracer::utils::index::plan::GroupResolver>
            resolver = nullptr,
        bool keep_metadata = false, bool by_path = false)
        : intern_(&intern),
          select_(std::move(select)),
          time_scale_(time_scale),
          resolver_(std::move(resolver)),
          keep_metadata_(keep_metadata),
          by_path_(by_path) {}

    bool accepts(const ScanShape&) const override { return true; }
    bool needs_args() const override { return true; }
    bool wants_metadata() const override { return keep_metadata_; }

    std::vector<std::string> extra_captures() const override {
        return row_fold_extra_captures(select_);
    }

    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<NativeRowFold>(*intern_, select_, time_scale_,
                                               resolver_, keep_metadata_,
                                               by_path_);
    }

    void step(const FoldBatch& batch) override {
        for (const FoldEvent& ev : batch.events) {
            if (ev.phase == RecordPhase::UNKNOWN ||
                (!keep_metadata_ && ev.phase == RecordPhase::METADATA))
                continue;
            events_.push_back(ev);
        }
    }

    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}

    void merge(Fold& other) override {
        auto& o = static_cast<NativeRowFold&>(other);
        events_.insert(events_.end(),
                       std::make_move_iterator(o.events_.begin()),
                       std::make_move_iterator(o.events_.end()));
        o.events_.clear();
    }

    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return true;
    }

    /// Build the accumulated events into one native DataFrame. Drains events_.
    dataframe::DataFrame build();

   private:
    const dftracer::utils::StringIntern* intern_;
    std::vector<std::string> select_;  // empty = every column
    double time_scale_;                // ts/dur multiplier (1.0 = none)
    std::shared_ptr<const dftracer::utils::index::plan::GroupResolver>
        resolver_;                     // resolved columns, or null
    bool keep_metadata_;               // phase("metadata"): keep ph=M records
    bool by_path_;
    std::vector<FoldEvent> events_;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_NATIVE_ROW_FOLD_H
