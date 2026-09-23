#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/arrow_bridge.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/lazyframe.h>
#include <dftracer/utils/python/plugin_host.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_list_helpers.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_seq_helpers.h>
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/series.h>
#include <dftracer/utils/python/trace_viewer.h>
#include <dftracer/utils/python/trace_viewer_detail.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/comparator/compare_view.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/time_metric.h>
#include <dftracer/utils/trace/trace_config.h>
#include <dftracer/utils/trace/views/result_batch.h>
#include <dftracer/utils/trace/views/result_join.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/common/string_arena.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/python/arrow_helpers.h>
#include <dftracer/utils/python/batch_byte_size.h>
#include <dftracer/utils/python/streaming_iterator.h>
#endif

#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::python::trace_viewer_detail {

using dftracer::utils::utilities::filesystem::FileEntry;
using dftracer::utils::utilities::filesystem::PatternDirectoryScannerUtility;
using dftracer::utils::utilities::filesystem::
    PatternDirectoryScannerUtilityInput;
namespace dftint = dftracer::utils::trace::internal;

namespace {

// Parallel scan of a directory for trace files (recursive, .pfw.gz only).
// Returns false with a Python error set on failure. Sorted for determinism.
bool scan_dir_trace_files(const std::string& dir,
                          std::vector<std::string>& out) {
    auto* rt = dftracer::utils::python::get_default_runtime();
    PatternDirectoryScannerUtilityInput input(dir, {".pfw.gz"},
                                              /*recursive=*/true,
                                              /*populate_size=*/false);
    std::vector<FileEntry> entries;
    if (!run_blocking([&] {
            rt->submit(dftracer::utils::run_coro_scope(
                           rt->executor(),
                           [](dftracer::utils::CoroScope& scope,
                              PatternDirectoryScannerUtilityInput in,
                              std::vector<FileEntry>* o)
                               -> dftracer::utils::coro::CoroTask<void> {
                               PatternDirectoryScannerUtility scanner;
                               *o = co_await scanner(scope, in);
                           },
                           std::move(input), &entries),
                       "tv-scan-dir")
                .get();
        })) {
        return false;
    }
    out.clear();
    out.reserve(entries.size());
    for (auto& e : entries) out.push_back(e.path.string());
    std::sort(out.begin(), out.end());
    return true;
}

}  // namespace

ViewerPlan* plan_of(TraceViewerObject* self) {
    return static_cast<ViewerPlan*>(self->plan_ptr);
}

// Throws DFTUtilsException on a bad filter DSL.
View build_view_from_data(const std::vector<std::string>& file_paths,
                          const std::string& index_dir, const ViewerPlan& p,
                          bool aggregate) {
    std::vector<ViewFile> files;
    files.reserve(file_paths.size());
    for (const auto& fp : file_paths) {
        ViewFile vf;
        vf.file_path = fp;
        vf.index_path = dftint::determine_index_path(vf.file_path, index_dir);
        files.push_back(std::move(vf));
    }

    View v = View::from_files(std::move(files));
    for (const auto& q : p.filters) {
        auto parsed = Query::from_string(q);
        if (!parsed)
            throw dftracer::utils::DFTUtilsException(
                dftracer::utils::ErrorCode::INVALID_ARGUMENT,
                "invalid filter query: " + q);
        v = v.filter(parsed.value());
    }
    if (p.phase >= 0) v = v.phase(static_cast<Phase>(p.phase));
    if (p.time_scale != 1.0) v = v.time_scale(p.time_scale);
    if (p.time_bucket_us) {
        if (p.bucket_origin_min)
            v = v.time_bucket_min(p.time_bucket_us);
        else if (p.bucket_origin_us)
            v = v.time_bucket(p.time_bucket_us, p.bucket_origin_us);
        else
            v = v.time_bucket(p.time_bucket_us);
    }
    if (p.occ_cell_us) v = v.occ_cell(p.occ_cell_us);
    if (p.time_range)
        v = v.time_range(p.time_range->first, p.time_range->second);
    // The aggregation and everything that operates on the aggregated result;
    // CompareView owns this step, so it asks for a base view (aggregate=false).
    if (aggregate) {
        if (!p.group_by.empty()) v = v.group_by(p.group_by);
        if (!p.agg.empty()) v = v.agg(p.agg);
        if (p.auto_numeric)
            v = p.numeric_arg_aggs.empty()
                    ? v.agg_numeric_args()
                    : v.agg_numeric_args(p.numeric_arg_aggs);
        if (!p.select.empty()) v = v.select(p.select);
        if (!p.sort_col.empty()) v = v.sort_by(p.sort_col, p.sort_desc);
        if (!p.topk_col.empty())
            v = v.topk(p.topk_col, p.topk_k, p.topk_largest);
        if (p.limit) v = v.limit(p.limit);
        if (p.offset) v = v.offset(p.offset);
    }
    if (p.memory_budget)
        v = v.memory_budget(p.memory_budget);
    else if (p.auto_spill)
        v = v.auto_spill();
    if (!p.rollup_root.empty()) v = v.rollup_root(p.rollup_root);
    if (!p.views_root.empty()) v = v.views_root(p.views_root);
    return v;
}

std::vector<std::string> extract_files(TraceViewerObject* self) {
    std::vector<std::string> out;
    parse_str_list(self->files, "files", out);  // already a validated list[str]
    return out;
}

std::string extract_index_dir(TraceViewerObject* self) {
    return (self->index_path && self->index_path != Py_None)
               ? as_utf8(self->index_path)
               : "";
}

// Same plan, typed as an AggregatedView so the cache terminals are reachable.
// group_by is idempotent (re-applies the plan's own keys), so this only shifts
// the type, not the plan. The runtime guard rejects a plan with no aggregation.
AggregatedView build_agg_view(const std::vector<std::string>& files,
                              const std::string& index_dir,
                              const ViewerPlan& p) {
    return build_view_from_data(files, index_dir, p).group_by(p.group_by);
}

namespace {

PyObject* tv_new(PyTypeObject* type, PyObject*, PyObject*) {
    TraceViewerObject* self = (TraceViewerObject*)type->tp_alloc(type, 0);
    if (!self) return nullptr;
    self->files = nullptr;
    self->index_path = nullptr;
    self->runtime_obj = nullptr;
    self->plan_ptr = new ViewerPlan();
    return (PyObject*)self;
}

void tv_dealloc(TraceViewerObject* self) {
    Py_XDECREF(self->files);
    Py_XDECREF(self->index_path);
    Py_XDECREF(self->runtime_obj);
    delete plan_of(self);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

int tv_init(TraceViewerObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"files", "index_path", "runtime", nullptr};
    PyObject* files = nullptr;
    PyObject* index_path = nullptr;
    PyObject* runtime_arg = nullptr;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|OO",
                                     const_cast<char**>(kwlist), &files,
                                     &index_path, &runtime_arg))
        return -1;

    // Accept a directory, a single path, or an iterable of paths. A directory
    // is expanded to its .pfw.gz files via the parallel scanner.
    PyObject* list = nullptr;
    if (PyUnicode_Check(files)) {
        const char* path = PyUnicode_AsUTF8(files);
        std::error_code ec;
        if (path && fs::is_directory(path, ec)) {
            std::vector<std::string> scanned;
            if (!scan_dir_trace_files(path, scanned)) return -1;
            list = PyList_New(static_cast<Py_ssize_t>(scanned.size()));
            if (!list) return -1;
            for (std::size_t i = 0; i < scanned.size(); ++i) {
                PyObject* item = PyUnicode_FromString(scanned[i].c_str());
                if (!item) {
                    Py_DECREF(list);
                    return -1;
                }
                PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item);
            }
        } else {
            list = PyList_New(1);
            if (!list) return -1;
            Py_INCREF(files);
            PyList_SET_ITEM(list, 0, files);
        }
    } else {
        list = PySequence_List(files);
        if (!list) return -1;
    }
    self->files = list;

    if (index_path && index_path != Py_None) {
        Py_INCREF(index_path);
        self->index_path = index_path;
    }
    if (runtime_arg && runtime_arg != Py_None) {
        if (PyObject_TypeCheck(runtime_arg, &RuntimeType)) {
            Py_INCREF(runtime_arg);
            self->runtime_obj = runtime_arg;
        } else {
            PyObject* native = PyObject_GetAttrString(runtime_arg, "_native");
            if (native && PyObject_TypeCheck(native, &RuntimeType)) {
                self->runtime_obj = native;
            } else {
                Py_XDECREF(native);
                PyErr_SetString(PyExc_TypeError,
                                "runtime must be a Runtime instance or None");
                return -1;
            }
        }
    }
    return 0;
}

}  // namespace

// Shallow-clone the object with a deep-copied plan; the caller mutates the
// returned plan to implement one builder op (immutable-value semantics).
TraceViewerObject* clone_as(TraceViewerObject* self, PyTypeObject* type) {
    TraceViewerObject* c = (TraceViewerObject*)type->tp_alloc(type, 0);
    if (!c) return nullptr;
    Py_XINCREF(self->files);
    c->files = self->files;
    Py_XINCREF(self->index_path);
    c->index_path = self->index_path;
    Py_XINCREF(self->runtime_obj);
    c->runtime_obj = self->runtime_obj;
    c->plan_ptr = new ViewerPlan(*plan_of(self));
    return c;
}

// Builder ops preserve the object's type (a chained AggregatedTraceViewer stays
// aggregated); group_by/agg/agg_numeric_args promote to AggregatedTraceViewer,
// which alone carries the cache terminals.
TraceViewerObject* clone(TraceViewerObject* self) {
    return clone_as(self, Py_TYPE(self));
}
TraceViewerObject* clone_agg(TraceViewerObject* self) {
    return clone_as(self, &AggregatedTraceViewerType);
}

namespace {

PyMethodDef tv_methods[] = {
    {"filter", DFTU_PYCFUNCTION(tv_filter), METH_O,
     "Keep events matching a query-DSL predicate (AND-combined)."},
    {"query", DFTU_PYCFUNCTION(tv_filter), METH_O, "Alias of filter()."},
    {"phase", DFTU_PYCFUNCTION(tv_phase), METH_O,
     "Select 'events' (ph=X), 'counters' (ph=C), 'aggregated' (ph=A), "
     "'metadata' (ph=M), or 'any'."},
    {"group_by", DFTU_PYCFUNCTION(tv_group_by), METH_VARARGS,
     "Group by keys: name/cat/pid/tid/fhash/arg:<key>."},
    {"agg", DFTU_PYCFUNCTION(tv_agg), METH_VARARGS,
     "Aggregations: count, sum:/min:/max:/mean:/var:/std:<field>, "
     "argmax:<field>:<by>, set_union:<field> (distinct values, one string "
     "column joined by \\x1e)."},
    {"time_bucket", DFTU_PYCFUNCTION(tv_time_bucket),
     METH_VARARGS | METH_KEYWORDS,
     "time_bucket(interval_us, normalize_to=None): bucket events into fixed "
     "intervals. normalize_to aligns bucket boundaries: an int origin, or "
     "'min' "
     "for the trace's minimum timestamp (from the index, no scan); None = "
     "aligned to 0."},
    {"occ_cell", DFTU_PYCFUNCTION(tv_occ_cell), METH_O,
     "Occupancy cell size (busy quantum) in microseconds; 0 = default. Finer "
     "resolves overlap on short events (honored with time_range)."},
    {"time_unit", DFTU_PYCFUNCTION(tv_time_unit), METH_O,
     "Normalize ts/dur to a target unit (ns/us/ms/sec, s=sec); source read "
     "from "
     "the "
     "trace's CM time_metric. Higher-level helper over time_scale()."},
    {"time_scale", DFTU_PYCFUNCTION(tv_time_scale), METH_O,
     "Multiply ts/dur/te by this ratio (source_ns/target_ns; 1.0 = none). The "
     "raw primitive behind time_unit()."},
    {"time_range", DFTU_PYCFUNCTION(tv_time_range), METH_VARARGS,
     "Restrict to a [begin, end) timestamp window."},
    {"select", DFTU_PYCFUNCTION(tv_select), METH_VARARGS, "Project columns."},
    {"memory_budget", DFTU_PYCFUNCTION(tv_memory_budget), METH_O,
     "Spill aggregation above this many bytes (0 = in-memory)."},
    {"auto_spill", DFTU_PYCFUNCTION(tv_auto_spill), METH_NOARGS,
     "Spill at ~1/3 of available memory."},
    {"agg_numeric_args", DFTU_PYCFUNCTION(tv_auto_numeric_args), METH_VARARGS,
     "Aggregate every auto-discovered numeric arg (size, ret, ...). No args = "
     "one bare-named mean column per arg; pass op names (sum/min/max/mean/var/"
     "std/skew/kurt) for one <op>_<arg> column per (arg, op)."},
    {"limit", DFTU_PYCFUNCTION(tv_limit), METH_O, "Cap produced rows/events."},
    {"offset", DFTU_PYCFUNCTION(tv_offset), METH_O,
     "Skip the first N rows/events."},
    {"sort_by", DFTU_PYCFUNCTION(tv_sort_by), METH_VARARGS | METH_KEYWORDS,
     "sort_by(name, descending=False): order the collect() result by a "
     "column (same dataframe kernel as DataFrame.sort_by)."},
    {"topk", DFTU_PYCFUNCTION(tv_topk), METH_VARARGS | METH_KEYWORDS,
     "topk(name, k, largest=True): keep the k best rows of the collect() "
     "result (same dataframe kernel as DataFrame.topk)."},
    {"columns", DFTU_PYCFUNCTION(tv_columns), METH_NOARGS,
     "List the columns discoverable from the index (no trace scan)."},
    {"schema", DFTU_PYCFUNCTION(tv_schema), METH_NOARGS,
     "Map each column to its type (no trace scan)."},
    {"time_metric", DFTU_PYCFUNCTION(tv_time_metric), METH_NOARGS,
     "The trace's native time unit ('us'/'ns'/'ms'/'sec') from the first "
     "file's CM record (head-read only, no scan)."},
    {"collect", DFTU_PYCFUNCTION(tv_collect), METH_NOARGS,
     "Build the group_by+agg plan and return a LazyFrame; nothing scans "
     "until you call .collect() (-> DataFrame) or .to_arrow()/.to_pandas() "
     "on the result."},
    {"call_tree", DFTU_PYCFUNCTION(tv_call_tree), METH_VARARGS,
     "call_tree(partition) -> scan, then the events DataFrame plus "
     "level/parent_id (containment nesting per lane)."},
    {"flamegraph", DFTU_PYCFUNCTION(tv_flamegraph), METH_VARARGS,
     "flamegraph(partition) -> scan, then a folded node DataFrame (node_id, "
     "parent, name, level, total, self, count)."},
    {"containment", DFTU_PYCFUNCTION(tv_containment), METH_VARARGS,
     "containment(partition) -> (call_tree_df, flamegraph_df) from one scan "
     "and "
     "one buffered fold."},
    {"join", DFTU_PYCFUNCTION(tv_join), METH_VARARGS | METH_KEYWORDS,
     "Aggregate and equi-join another viewer on the shared group key; "
     "how=inner|left|right|full|semi|anti. Returns a DataFrame with l_/r_ "
     "prefixed value columns."},
    {"compare", DFTU_PYCFUNCTION(tv_compare), METH_VARARGS | METH_KEYWORDS,
     "Compare this viewer (baseline) against another (variant) using this "
     "viewer's group_by + agg plan. Both sides aggregate in parallel; returns "
     "a DataFrame with the group key, l_/r_ per metric, and delta_/pct_."},
    {"collect_typed", DFTU_PYCFUNCTION(tv_collect_typed),
     METH_VARARGS | METH_KEYWORDS,
     "One-pass read of the aggregation index's three record families over "
     "shard range [shard_begin, shard_end) (shard_end<=0 = all); returns a "
     "dict {'regular','aggregated','counters'} of pyarrow.Table."},
    {"stream", DFTU_PYCFUNCTION(tv_stream), METH_VARARGS | METH_KEYWORDS,
     "Iterate matching events as native DataFrame chunks (parallel, bounded "
     "memory). kwargs: batch_size, workers, normalize."},
    {"_session_execute", DFTU_PYCFUNCTION(tv_session_run), METH_O,
     "Internal: run a Session's branches over one shared scan. Arg: a list of "
     "(kind, viewer, sink) tuples; returns a list of _DataFrame (collect) / "
     "stats dict (export) / None (materialize) in the same order."},
    {"statistics", DFTU_PYCFUNCTION(tv_statistics), METH_NOARGS,
     "One-row summary: count, mean/stddev dur, min/max ts (dict)."},
    {"aggregate_partial", DFTU_PYCFUNCTION(tv_aggregate_partial), METH_NOARGS,
     "Combinable aggregation partial (bytes) for distributed merge."},
    {"merge_partials_to_table", DFTU_PYCFUNCTION(tv_merge_partials), METH_O,
     "Merge aggregate_partial() bytes into the final pyarrow.Table."},
    {"flamegraph_partial", DFTU_PYCFUNCTION(tv_flamegraph_partial),
     METH_VARARGS,
     "flamegraph_partial(partition) -> serialized flamegraph arena (bytes) for "
     "a distributed merge (combine with _ext.merge_flamegraph_partials)."},
    {"rollup_root", DFTU_PYCFUNCTION(tv_rollup_root), METH_O,
     "Override the aggregation-cache root dir (default: derive from index)."},
    {"views_root", DFTU_PYCFUNCTION(tv_views_root), METH_O,
     "Override the materialized-view root dir (default: derive "
     "<parent-of-index>/.dftindex-views)."},
    {"materialize", DFTU_PYCFUNCTION(tv_materialize),
     METH_VARARGS | METH_KEYWORDS,
     "Build-only: persist this query as a materialized view so a later "
     "matching read reuses it (row query -> filtered trace, aggregation -> "
     "rollup). kwargs: checkpoint_size, part_size, progress (called with "
     "(done, total) scan units). Returns None."},
    {"mv_source", DFTU_PYCFUNCTION(tv_mv_source), METH_NOARGS,
     "The materialized-view trace file(s) that would serve this query, or an "
     "empty list if a read would scan the base."},
    {"materialize_dir", DFTU_PYCFUNCTION(tv_materialize_dir), METH_NOARGS,
     "Distributed row-MV coordinator: create and return the shared MV dir for "
     "this full-file-set view. Ranks export into subdirs of it."},
    {"register_materialized", DFTU_PYCFUNCTION(tv_register_materialized),
     METH_O,
     "Write the MV manifest at the given dir over this view's base set, after "
     "ranks have materialized their shard subdirs."},
    {"export_trace", DFTU_PYCFUNCTION(tv_export), METH_VARARGS | METH_KEYWORDS,
     "Write a dftracer trace: the aggregation when group_by/agg is set, else "
     "the matching events. gzip+re-indexable by default. kwargs: compress, "
     "index, member_size, level, part_size."},
    {nullptr, nullptr, 0, nullptr}};

// AggregatedTraceViewer adds the distributed rollup terminals; collect() and
// every builder op / other terminal are inherited from TraceViewer via tp_base.
// A plain collect() already reads a subsuming rollup, so there is no cache
// flag.
PyMethodDef atv_methods[] = {
    {"materialize_partials", DFTU_PYCFUNCTION(tv_materialize_partials), METH_O,
     "Materialize the rollup from aggregate_partial() bytes; no rescan."},
    {"reconstruct_if_cached", DFTU_PYCFUNCTION(tv_reconstruct_if_cached),
     METH_NOARGS,
     "Materialized aggregation as a pyarrow.Table, or None on a miss."},
    {nullptr, nullptr, 0, nullptr}};

}  // namespace

}  // namespace dftracer::utils::python::trace_viewer_detail

using namespace dftracer::utils::python::trace_viewer_detail;

PyTypeObject TraceViewerType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "dftracer_utils_ext.TraceViewer",
    sizeof(TraceViewerObject),
    0,
    (destructor)tv_dealloc,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    "Arrow-first composable view over a trace.",
    0,
    0,
    0,
    0,
    0,
    0,
    tv_methods,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    (initproc)tv_init,
    0,
    tv_new,
};

// Same object layout and builders as TraceViewer (tp_base), plus the cache
// terminals. group_by/agg/agg_numeric_args return this type.
PyTypeObject AggregatedTraceViewerType = {
    PyVarObject_HEAD_INIT(nullptr,
                          0) "dftracer_utils_ext.AggregatedTraceViewer",
    sizeof(TraceViewerObject),
    0,
    0,  // tp_dealloc inherited
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    Py_TPFLAGS_DEFAULT,
    "A TraceViewer with a group_by/agg; adds the materialized-view cache.",
    0,
    0,
    0,
    0,
    0,
    0,
    atv_methods,
    0,
    0,
    &TraceViewerType,  // tp_base
    0,
    0,
    0,
    0,
    0,  // tp_init inherited
    0,
    0,  // tp_new inherited
};

int dftracer::utils::python::init_trace_viewer(PyObject* m) {
    if (register_type(m, &TraceViewerType, "_TraceViewer") < 0) return -1;
    if (register_type(m, &AggregatedTraceViewerType, "_AggregatedTraceViewer") <
        0)
        return -1;

    // A pure reduce (no scan / viewer state), so it is a module function, not a
    // viewer method. tv_merge_flamegraph_partials ignores its first argument.
    static PyMethodDef merge_fg_def = {
        "merge_flamegraph_partials",
        DFTU_PYCFUNCTION(tv_merge_flamegraph_partials), METH_O,
        "merge_flamegraph_partials(partials) -> node DataFrame (no scan)."};
    PyObject* fn = PyCFunction_NewEx(&merge_fg_def, nullptr, nullptr);
    if (!fn) return -1;
    if (PyModule_AddObject(m, "merge_flamegraph_partials", fn) < 0) {
        Py_DECREF(fn);
        return -1;
    }
    return 0;
}
