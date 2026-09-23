#ifndef DFTRACER_UTILS_PYTHON_TRACE_VIEWER_DETAIL_H
#define DFTRACER_UTILS_PYTHON_TRACE_VIEWER_DETAIL_H

#include <Python.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/python/trace_viewer.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Private seam shared by the TraceViewer extension translation units
// (trace_viewer.cpp plus trace_viewer/{plan,builders,collect,session,join,
// materialize,export,stream,containment}.cpp). trace_viewer.cpp owns the
// TraceViewer/AggregatedTraceViewer type objects, the PyMethodDef tables,
// init_trace_viewer, and defines the shared plan/clone/build file-statics;
// every family TU calls them and defines the method bodies referenced by the
// tables back in trace_viewer.cpp.
namespace dftracer::utils::python::trace_viewer_detail {

using dftracer::utils::Runtime;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::query::Query;
using dftracer::utils::trace::views::AggOp;
using dftracer::utils::trace::views::AggregatedView;
using dftracer::utils::trace::views::AggSpec;
using dftracer::utils::trace::views::Deferred;
using dftracer::utils::trace::views::ExportStats;
using dftracer::utils::trace::views::GroupKey;
using dftracer::utils::trace::views::Phase;
using dftracer::utils::trace::views::TypedResult;
using dftracer::utils::trace::views::View;
using dftracer::utils::trace::views::ViewFile;
using dftracer::utils::trace::views::ViewSession;

// The accumulated builder ops, lowered onto a fresh View by each terminal.
struct ViewerPlan {
    std::vector<std::string> filters;  // DSL predicates, AND-combined
    int phase = -1;                    // -1 = View default; else Phase value
    std::vector<GroupKey> group_by;
    std::vector<AggSpec> agg;
    std::uint64_t time_bucket_us = 0;
    std::uint64_t bucket_origin_us = 0;
    bool bucket_origin_min = false;
    std::uint64_t occ_cell_us = 0;
    std::optional<std::pair<double, double>> time_range;
    std::vector<std::string> select;
    std::uint64_t memory_budget = 0;
    bool auto_spill = false;
    bool auto_numeric = false;
    std::vector<AggSpec> numeric_arg_aggs;  // reductions per discovered arg
    double time_scale = 1.0;  // ts/dur normalization factor (1.0 = none)
    std::uint64_t limit = 0;
    std::uint64_t offset = 0;
    std::string rollup_root;  // "" = derive the aggregation-cache dir
    std::string views_root;   // "" = derive <parent-of-index>/.dftindex-views
    // Post-aggregation ordering applied to the collect() result DataFrame (same
    // dataframe kernels as DataFrame.sort_by/topk), so the View and dataframe
    // surfaces match.
    std::string sort_col;
    bool sort_desc = false;
    std::string topk_col;
    std::int64_t topk_k = -1;  // -1 = no topk
    bool topk_largest = true;
};

// Shared plan/clone/build file-statics; defined in trace_viewer.cpp.
ViewerPlan* plan_of(TraceViewerObject* self);
View build_view_from_data(const std::vector<std::string>& file_paths,
                          const std::string& index_dir, const ViewerPlan& p,
                          bool aggregate = true);
std::vector<std::string> extract_files(TraceViewerObject* self);
std::string extract_index_dir(TraceViewerObject* self);
AggregatedView build_agg_view(const std::vector<std::string>& files,
                              const std::string& index_dir,
                              const ViewerPlan& p);
TraceViewerObject* clone_as(TraceViewerObject* self, PyTypeObject* type);
TraceViewerObject* clone(TraceViewerObject* self);
TraceViewerObject* clone_agg(TraceViewerObject* self);

// Cross-TU parse helpers.
bool parse_group_key(const char* s, GroupKey& out);  // plan.cpp
bool parse_agg_spec(const char* s, AggSpec& out);    // plan.cpp
bool parse_partition(PyObject* seq_obj,              // containment.cpp
                     std::vector<std::string>& out);
void parse_containment_cfg(const std::string& sink,  // containment.cpp
                           std::vector<std::string>& partition, std::string& ts,
                           std::string& dur, std::string& name);
bool parse_join_type(const char* how,                // join.cpp
                     dftracer::utils::trace::views::JoinType* out);

// trace_viewer/plan.cpp
PyObject* tv_phase(TraceViewerObject* self, PyObject* arg);

// trace_viewer/builders.cpp
PyObject* tv_filter(TraceViewerObject* self, PyObject* arg);
PyObject* tv_group_by(TraceViewerObject* self, PyObject* args);
PyObject* tv_agg(TraceViewerObject* self, PyObject* args);
PyObject* tv_time_bucket(TraceViewerObject* self, PyObject* args,
                         PyObject* kwds);
PyObject* tv_occ_cell(TraceViewerObject* self, PyObject* arg);
PyObject* tv_time_unit(TraceViewerObject* self, PyObject* arg);
PyObject* tv_time_scale(TraceViewerObject* self, PyObject* arg);
PyObject* tv_time_range(TraceViewerObject* self, PyObject* args);
PyObject* tv_select(TraceViewerObject* self, PyObject* args);
PyObject* tv_memory_budget(TraceViewerObject* self, PyObject* arg);
PyObject* tv_rollup_root(TraceViewerObject* self, PyObject* arg);
PyObject* tv_views_root(TraceViewerObject* self, PyObject* arg);
PyObject* tv_auto_spill(TraceViewerObject* self, PyObject*);
PyObject* tv_auto_numeric_args(TraceViewerObject* self, PyObject* args);
PyObject* tv_limit(TraceViewerObject* self, PyObject* arg);
PyObject* tv_sort_by(TraceViewerObject* self, PyObject* args, PyObject* kwds);
PyObject* tv_topk(TraceViewerObject* self, PyObject* args, PyObject* kwds);
PyObject* tv_offset(TraceViewerObject* self, PyObject* arg);

// trace_viewer/collect.cpp
PyObject* tv_collect(TraceViewerObject* self, PyObject*);
PyObject* tv_collect_typed(TraceViewerObject* self, PyObject* args,
                           PyObject* kwds);
PyObject* tv_columns(TraceViewerObject* self, PyObject*);
PyObject* tv_schema(TraceViewerObject* self, PyObject*);
PyObject* tv_time_metric(TraceViewerObject* self, PyObject*);
PyObject* tv_statistics(TraceViewerObject* self, PyObject*);

// trace_viewer/containment.cpp
PyObject* tv_call_tree(TraceViewerObject* self, PyObject* args);
PyObject* tv_flamegraph(TraceViewerObject* self, PyObject* args);
PyObject* tv_containment(TraceViewerObject* self, PyObject* args);

// trace_viewer/session.cpp
PyObject* tv_session_run(TraceViewerObject* self, PyObject* branches);

// trace_viewer/join.cpp
PyObject* tv_join(TraceViewerObject* self, PyObject* args, PyObject* kwds);
PyObject* tv_compare(TraceViewerObject* self, PyObject* args, PyObject* kwds);

// trace_viewer/materialize.cpp
PyObject* tv_aggregate_partial(TraceViewerObject* self, PyObject*);
PyObject* tv_merge_partials(TraceViewerObject* self, PyObject* arg);
PyObject* tv_flamegraph_partial(TraceViewerObject* self, PyObject* args);
PyObject* tv_merge_flamegraph_partials(TraceViewerObject*, PyObject* arg);
PyObject* tv_materialize_partials(TraceViewerObject* self, PyObject* arg);
PyObject* tv_materialize(TraceViewerObject* self, PyObject* args,
                         PyObject* kwds);
PyObject* tv_mv_source(TraceViewerObject* self, PyObject*);
PyObject* tv_materialize_dir(TraceViewerObject* self, PyObject*);
PyObject* tv_register_materialized(TraceViewerObject* self, PyObject* arg);
PyObject* tv_reconstruct_if_cached(TraceViewerObject* self, PyObject*);

// trace_viewer/export.cpp
PyObject* tv_export(TraceViewerObject* self, PyObject* args, PyObject* kwds);

// trace_viewer/stream.cpp
PyObject* tv_stream(TraceViewerObject* self, PyObject* args, PyObject* kwds);

}  // namespace dftracer::utils::python::trace_viewer_detail

#endif  // DFTRACER_UTILS_PYTHON_TRACE_VIEWER_DETAIL_H
