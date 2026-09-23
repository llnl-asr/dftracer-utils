#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_seq_helpers.h>
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/trace_viewer_detail.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::python::trace_viewer_detail {

// Aggregate this (shard's) files into an opaque, combinable partial. The bytes
// carry the running accumulators (count/sum/sumsq/sketch), so distributed
// callers merge them with merge_partials() - correct for mean/std/percentiles,
// unlike re-aggregating finalized per-shard values.
PyObject* tv_aggregate_partial(TraceViewerObject* self, PyObject*) {
    Runtime* rt = resolve_runtime(self);
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    std::string out;
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            out = rt->submit(v.aggregate_partial()).get();
        }))
        return nullptr;
    return PyBytes_FromStringAndSize(out.data(),
                                     static_cast<Py_ssize_t>(out.size()));
}

// Combine partials from aggregate_partial() into the final aggregation Table.
// Uses this viewer's group_by/agg as the shape; files are not scanned.
PyObject* tv_merge_partials(TraceViewerObject* self, PyObject* arg) {
#ifndef DFTRACER_UTILS_ENABLE_ARROW
    PyErr_SetString(PyExc_RuntimeError,
                    "merge_partials() requires the arrow-enabled build");
    return nullptr;
#else
    std::vector<std::string> owned;
    if (!parse_bytes_seq(arg, "merge_partials expects a sequence", owned))
        return nullptr;

    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    DataFrame table;
    if (!run_blocking([&] {
            std::vector<std::string_view> parts(owned.begin(), owned.end());
            View v = build_view_from_data(files, index_dir, plan);
            table = v.merge_partials_to_table(parts);
        }))
        return nullptr;
    return dftracer::utils::python::wrap_dataframe(std::move(table));
#endif
}

// Scan this rank's files into a serialized flamegraph arena partial (bytes).
PyObject* tv_flamegraph_partial(TraceViewerObject* self, PyObject* args) {
    PyObject* part = nullptr;
    const char* ts = "ts";
    const char* dur = "dur";
    const char* name = "name";
    PyObject* group_obj = nullptr;
    if (!PyArg_ParseTuple(args, "O|sssO", &part, &ts, &dur, &name, &group_obj))
        return nullptr;
    std::vector<std::string> partition, group;
    if (!parse_partition(part, partition)) return nullptr;
    if (group_obj && group_obj != Py_None && !parse_partition(group_obj, group))
        return nullptr;
    Runtime* rt = resolve_runtime(self);
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    std::string out;
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            out = rt->submit(
                        v.flamegraph_partial(partition, ts, dur, name, group))
                      .get();
        }))
        return nullptr;
    return PyBytes_FromStringAndSize(out.data(),
                                     static_cast<Py_ssize_t>(out.size()));
}

// Merge flamegraph arena partials (from flamegraph_partial across ranks) into
// the final node DataFrame. No scan.
PyObject* tv_merge_flamegraph_partials(TraceViewerObject*, PyObject* arg) {
#ifndef DFTRACER_UTILS_ENABLE_ARROW
    PyErr_SetString(PyExc_RuntimeError,
                    "merge_flamegraph_partials() requires the arrow build");
    return nullptr;
#else
    std::vector<std::string> owned;
    if (!parse_bytes_seq(arg, "merge_flamegraph_partials expects a sequence",
                         owned))
        return nullptr;
    std::vector<std::string_view> parts(owned.begin(), owned.end());
    DataFrame table = View::merge_flamegraph_partials(parts);
    return dftracer::utils::python::wrap_dataframe(std::move(table));
#endif
}

// Distributed materialize: reduce rank-local partials and write the rollup.
PyObject* tv_materialize_partials(TraceViewerObject* self, PyObject* arg) {
    std::vector<std::string> owned;
    if (!parse_bytes_seq(arg, "materialize_partials expects a sequence", owned))
        return nullptr;

    Runtime* rt = resolve_runtime(self);
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    if (!run_blocking([&] {
            std::vector<std::string_view> parts(owned.begin(), owned.end());
            AggregatedView v = build_agg_view(files, index_dir, plan);
            rt->submit(v.materialize_partials(parts)).get();
        }))
        return nullptr;
    Py_RETURN_NONE;
}

// Build-only materialize (fire and forget): persist this query as a
// materialized view so a later matching read reuses it. A row query writes a
// filtered trace split into `part_size`-byte files at `checkpoint_size`
// granularity; an aggregation persists a rollup. Idempotent. Returns None.
PyObject* tv_materialize(TraceViewerObject* self, PyObject* args,
                         PyObject* kwds) {
    long long checkpoint_size = 0, part_size = 0;
    PyObject* progress_obj = nullptr;
    static const char* kwlist[] = {"checkpoint_size", "part_size", "progress",
                                   nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|LLO", const_cast<char**>(kwlist), &checkpoint_size,
            &part_size, &progress_obj))
        return nullptr;

    namespace views = dftracer::utils::trace::views;
    views::ProgressFn progress_fn;
    const views::ProgressFn* progress_ptr = nullptr;
    if (progress_obj && progress_obj != Py_None) {
        Py_INCREF(progress_obj);
        std::shared_ptr<PyObject> cb(progress_obj, [](PyObject* p) {
            PyGILState_STATE g = PyGILState_Ensure();
            Py_DECREF(p);
            PyGILState_Release(g);
        });
        progress_fn = [cb](std::size_t done, std::size_t total) {
            PyGILState_STATE g = PyGILState_Ensure();
            PyObject* r = PyObject_CallFunction(cb.get(), "nn",
                                                static_cast<Py_ssize_t>(done),
                                                static_cast<Py_ssize_t>(total));
            if (r)
                Py_DECREF(r);
            else
                PyErr_Clear();
            PyGILState_Release(g);
        };
        progress_ptr = &progress_fn;
    }

    Runtime* rt = resolve_runtime(self);
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            rt->submit(
                  v.materialize(static_cast<std::uint64_t>(checkpoint_size),
                                static_cast<std::uint64_t>(part_size))
                      .run(progress_ptr))
                .get();
        }))
        return nullptr;
    Py_RETURN_NONE;
}

// Observability: the MV trace file(s) that would serve this query, or an empty
// list if a read would scan the base. Lets callers report/assert MV reuse.
PyObject* tv_mv_source(TraceViewerObject* self, PyObject*) {
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    std::vector<std::string> paths;
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            paths = v.mv_source();
        }))
        return nullptr;
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(paths.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < paths.size(); ++i)
        PyList_SET_ITEM(
            list, static_cast<Py_ssize_t>(i),
            PyUnicode_FromStringAndSize(
                paths[i].data(), static_cast<Py_ssize_t>(paths[i].size())));
    return list;
}

// Distributed row-MV coordinator: create and return the shared MV directory
// derived from this (full-file-set) view's plan. Ranks export their filtered
// files into subdirs of it; the coordinator then calls register_materialized.
PyObject* tv_materialize_dir(TraceViewerObject* self, PyObject*) {
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    std::string dir;
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            dir = v.materialize_dir();
        }))
        return nullptr;
    return PyUnicode_FromStringAndSize(dir.data(),
                                       static_cast<Py_ssize_t>(dir.size()));
}

// Write the MV manifest at `dir` over this (full) view's base set, after every
// rank has materialized its shard subdir. Makes the MV discoverable.
PyObject* tv_register_materialized(TraceViewerObject* self, PyObject* arg) {
    const char* dir = as_utf8(arg);
    if (!dir) return nullptr;
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    std::string d(dir);
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            v.register_materialized(d);
        }))
        return nullptr;
    Py_RETURN_NONE;
}

// Reconstruct the cached aggregation into a pyarrow.Table, or None on a cache
// miss (no scan/decode). Lets a distributed caller pick read vs recompute.
PyObject* tv_reconstruct_if_cached(TraceViewerObject* self, PyObject*) {
#ifndef DFTRACER_UTILS_ENABLE_ARROW
    PyErr_SetString(PyExc_RuntimeError,
                    "reconstruct requires the arrow-enabled build");
    return nullptr;
#else
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    std::optional<DataFrame> table;
    if (!run_blocking([&] {
            AggregatedView v = build_agg_view(files, index_dir, plan);
            table = v.reconstruct_if_cached();
        }))
        return nullptr;
    if (!table) Py_RETURN_NONE;
    return dftracer::utils::python::wrap_dataframe(std::move(*table));
#endif
}

}  // namespace dftracer::utils::python::trace_viewer_detail
