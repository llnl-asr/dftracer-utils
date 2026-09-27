#ifndef DFTRACER_UTILS_TRACE_VIEWS_CONTAINMENT_FOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_CONTAINMENT_FOLD_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::views::detail {

// A named field bound to its source: a POD scalar (fast path) or a captured
// arg/nested value looked up by interned key id.
struct FieldRef {
    enum class Kind { Pid, Tid, Ts, Dur, Name, Cat, Captured } kind;
    std::uint32_t key_id = 0xFFFFFFFF;
};

// The buffered per-event tuple: lane key + interval + label, plus the optional
// flamegraph root-group value (interned; 0xFFFFFFFF when no group key is set).
struct ContainmentRow {
    std::uint64_t lane;
    std::int64_t pid;
    std::int64_t tid;
    std::int64_t start;
    std::int64_t dur;
    std::uint32_t name_id;
    std::uint32_t group_id = 0xFFFFFFFF;
};

// The output ts and dur are the stored values times `ts` and `dur`; floats
// when a path schema's time or duration unit is not microseconds.
struct TimeOut {
    double ts = 1.0;
    double dur = 1.0;
    bool floating = false;
};

// Field bindings resolved once against the shared intern; drives which fields
// the scan must capture (flat args and nested paths). `group_fields` roots the
// flamegraph by an arbitrary key (over raw events, so it is not the aggregation
// group_by): every event carries its group value and each lane folds under a
// synthetic node named by it.
struct ContainmentSpec {
    std::vector<FieldRef> lane_fields;
    std::vector<FieldRef> group_fields;
    FieldRef start_ref;
    FieldRef dur_ref;
    FieldRef name_ref;
    TimeOut out;
    bool needs_args = false;
    std::vector<std::string> nested_captures;
};

ContainmentSpec make_containment_spec(
    dftracer::utils::StringIntern& intern,
    const std::vector<std::string>& partition, const std::string& start_field,
    const std::string& dur_field, const std::string& name_field,
    const std::vector<std::string>& group = {},
    const dftracer::utils::index::RecordSchema* path_schema = nullptr,
    double time_scale = 1.0);

// Resolve one event into a row; returns false for a row with no interval.
// `intern` is used to intern the combined group-key value when the spec sets a
// group (thread-safe: the shared intern).
bool containment_row(const FoldEvent& ev, const ContainmentSpec& spec,
                     dftracer::utils::StringIntern& intern,
                     ContainmentRow& out);

// Group rows into lanes (rows sharing the lane key), each sorted by start
// (enclosing-first on a tie), ready for the containment walk. Shared by both
// builders so a combined build sorts once.
std::vector<std::vector<std::int64_t>> sorted_lanes(
    const std::vector<ContainmentRow>& rows);

dataframe::DataFrame build_call_tree(
    const std::vector<ContainmentRow>& rows,
    const std::vector<std::vector<std::int64_t>>& lanes,
    const dftracer::utils::StringIntern& intern, const TimeOut& out);
dataframe::DataFrame build_flamegraph(
    const std::vector<ContainmentRow>& rows,
    const std::vector<std::vector<std::int64_t>>& lanes,
    const dftracer::utils::StringIntern& intern, const TimeOut& out);

// Both frames from one buffer, sorting each lane once. .first = call_tree,
// .second = flamegraph.
std::pair<dataframe::DataFrame, dataframe::DataFrame> build_containment_both(
    const std::vector<ContainmentRow>& rows,
    const dftracer::utils::StringIntern& intern, const TimeOut& out);

// Distributed flamegraph: a rank folds its rows into an arena and serializes it
// (flamegraph_partial); rank 0 merges every rank's blob by name path and emits
// the node frame (merge_flamegraph_partials). The arena is the mergeable
// partial for MPI gather and Dask tree-reduce alike.
std::string flamegraph_partial(const std::vector<ContainmentRow>& rows,
                               const dftracer::utils::StringIntern& intern,
                               const TimeOut& out);
dataframe::DataFrame merge_flamegraph_partials(
    const std::vector<std::string_view>& partials);

// The View's containment terminal as a fuse-engine Fold: buffers rows during
// the shared scan, builds in parallel at the end. One fold, two builders
// (call_tree / flamegraph) over one buffer. With `path_schema`, the fields
// pid, tid, ts, dur and name (and the role paths) read its entity, lane,
// time, duration and name roles.
class ContainmentFold : public Fold {
   public:
    ContainmentFold(
        dftracer::utils::StringIntern& intern,
        std::vector<std::string> partition, std::string start_field = "ts",
        std::string dur_field = "dur", std::string name_field = "name",
        double time_scale = 1.0, std::vector<std::string> group = {},
        const dftracer::utils::index::RecordSchema* path_schema = nullptr);

    bool accepts(const ScanShape&) const override { return true; }
    bool needs_args() const override { return spec_.needs_args; }
    std::vector<std::string> extra_captures() const override {
        return spec_.nested_captures;
    }

    std::unique_ptr<Fold> slice() const override;
    void step(const FoldBatch& batch) override;
    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}
    void merge(Fold& other) override;
    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return true;
    }

    dataframe::DataFrame call_tree() const {
        return build_call_tree(rows_, sorted_lanes(rows_), *intern_, spec_.out);
    }
    dataframe::DataFrame flamegraph() const {
        return build_flamegraph(rows_, sorted_lanes(rows_), *intern_,
                                spec_.out);
    }
    std::pair<dataframe::DataFrame, dataframe::DataFrame> containment() const {
        return build_containment_both(rows_, *intern_, spec_.out);
    }
    std::string flamegraph_partial() const {
        return dftracer::utils::trace::views::detail::flamegraph_partial(
            rows_, *intern_, spec_.out);
    }

   private:
    // Non-const: the group-key harvest interns a combined value per event. The
    // intern is shared across sliced folds and is thread-safe, like the parse
    // path that also interns during the fused scan.
    dftracer::utils::StringIntern* intern_;
    ContainmentSpec spec_;
    std::vector<ContainmentRow> rows_;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_CONTAINMENT_FOLD_H
