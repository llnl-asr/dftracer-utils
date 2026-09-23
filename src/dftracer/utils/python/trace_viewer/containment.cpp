#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_seq_helpers.h>
#include <dftracer/utils/python/trace_viewer_detail.h>

#include <string>
#include <vector>

namespace dftracer::utils::python::trace_viewer_detail {

bool parse_partition(PyObject* seq_obj, std::vector<std::string>& out) {
    return parse_string_seq(seq_obj, "partition must be a sequence", out);
}

// Decode a session containment branch's sink "partition_csv\x1f ts\x1f dur\x1f
// name" back into its fields (defaults to the dftracer schema on a short cfg).
void parse_containment_cfg(const std::string& sink,
                           std::vector<std::string>& partition, std::string& ts,
                           std::string& dur, std::string& name) {
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : sink) {
        if (ch == '\x1f') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    parts.push_back(cur);
    if (parts.size() == 4 && !parts[0].empty()) {
        std::string p;
        for (char ch : parts[0]) {
            if (ch == ',') {
                partition.push_back(p);
                p.clear();
            } else {
                p.push_back(ch);
            }
        }
        partition.push_back(p);
    }
    ts = parts.size() == 4 ? parts[1] : "ts";
    dur = parts.size() == 4 ? parts[2] : "dur";
    name = parts.size() == 4 ? parts[3] : "name";
}

// call_tree(partition, ts_col, dur_col) -> scan the view, then the columnar
// containment fold; returns the events DataFrame plus level/parent_id.
PyObject* tv_call_tree(TraceViewerObject* self, PyObject* args) {
#ifndef DFTRACER_UTILS_ENABLE_ARROW
    PyErr_SetString(PyExc_RuntimeError,
                    "call_tree() requires the arrow-enabled build");
    return nullptr;
#else
    PyObject* part = nullptr;
    const char* ts = "ts";
    const char* dur = "dur";
    const char* name = "name";
    if (!PyArg_ParseTuple(args, "O|sss", &part, &ts, &dur, &name))
        return nullptr;
    std::vector<std::string> partition;
    if (!parse_partition(part, partition)) return nullptr;
    Runtime* rt = resolve_runtime(self);
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    DataFrame table;
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            table = rt->submit(v.call_tree(partition, ts, dur, name)).get();
        }))
        return nullptr;
    return dftracer::utils::python::wrap_dataframe(std::move(table));
#endif
}

// flamegraph(partition) -> scan the view, then fold by name path; returns the
// node DataFrame.
PyObject* tv_flamegraph(TraceViewerObject* self, PyObject* args) {
#ifndef DFTRACER_UTILS_ENABLE_ARROW
    PyErr_SetString(PyExc_RuntimeError,
                    "flamegraph() requires the arrow-enabled build");
    return nullptr;
#else
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
    DataFrame table;
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            table =
                rt->submit(v.flamegraph(partition, ts, dur, name, group)).get();
        }))
        return nullptr;
    return dftracer::utils::python::wrap_dataframe(std::move(table));
#endif
}

// containment(partition, ts, dur, name) -> (call_tree_df, flamegraph_df) from
// ONE scan and one buffered fold.
PyObject* tv_containment(TraceViewerObject* self, PyObject* args) {
#ifndef DFTRACER_UTILS_ENABLE_ARROW
    PyErr_SetString(PyExc_RuntimeError,
                    "containment() requires the arrow-enabled build");
    return nullptr;
#else
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
    std::pair<DataFrame, DataFrame> pr;
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            pr = rt->submit(v.containment(partition, ts, dur, name, group))
                     .get();
        }))
        return nullptr;
    PyObject* ct = dftracer::utils::python::wrap_dataframe(std::move(pr.first));
    if (!ct) return nullptr;
    PyObject* fg =
        dftracer::utils::python::wrap_dataframe(std::move(pr.second));
    if (!fg) {
        Py_DECREF(ct);
        return nullptr;
    }
    PyObject* t = PyTuple_Pack(2, ct, fg);  // Pack INCREFs both
    Py_DECREF(ct);
    Py_DECREF(fg);
    return t;
#endif
}

}  // namespace dftracer::utils::python::trace_viewer_detail
