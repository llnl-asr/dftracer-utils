#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/lazyframe.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/trace_viewer_detail.h>
#include <dftracer/utils/trace/time_metric.h>

#include <cctype>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::python::trace_viewer_detail {

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#endif

// collect(cache=False) -> a LazyFrame over this viewer's plan; nothing runs
// until the caller materializes it (LazyFrame.collect() -> DataFrame, or
// .to_arrow()/.to_pandas()/.to_polars()). cache uses the materialized-view
// cache (reconstruct on hit, else scan + persist + return); it requires an
// aggregation (group_by/agg), enforced by the runtime guard.
PyObject* tv_collect(TraceViewerObject* self, PyObject*) {
#ifndef DFTRACER_UTILS_ENABLE_ARROW
    PyErr_SetString(PyExc_RuntimeError,
                    "collect() requires the arrow-enabled build");
    return nullptr;
#else
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    View v = build_view_from_data(files, index_dir, plan);
    return dftracer::utils::python::wrap_lazyframe(v.collect());
#endif
}

// columns() -> list[str]: the distinct columns discoverable from the index
// (base axis + harvested scalar leaves + resolved.* aliases). No trace scan.
PyObject* tv_columns(TraceViewerObject* self, PyObject*) {
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    std::vector<std::string> cols;
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            cols = v.columns();
        }))
        return nullptr;
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(cols.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < cols.size(); ++i) {
        PyObject* s = PyUnicode_FromString(cols[i].c_str());
        if (!s) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), s);
    }
    return list;
}

// schema() -> dict[str, str]: each column mapped to its type ("int64" /
// "float64" / "string"). Same discovery as columns(); no trace scan.
PyObject* tv_schema(TraceViewerObject* self, PyObject*) {
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    std::vector<View::ColumnInfo> sc;
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            sc = v.schema();
        }))
        return nullptr;
    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;
    for (const auto& c : sc) {
        PyObject* val = PyUnicode_FromString(c.type.c_str());
        if (!val || PyDict_SetItemString(dict, c.name.c_str(), val) != 0) {
            Py_XDECREF(val);
            Py_DECREF(dict);
            return nullptr;
        }
        Py_DECREF(val);
    }
    return dict;
}

// time_metric() -> str: the trace's native time unit ("us"/"ns"/"ms"/"sec")
// from the first file's CM record. Head-read only, no scan; "us" with no files.
PyObject* tv_time_metric(TraceViewerObject* self, PyObject*) {
    auto files = extract_files(self);
    const dftracer::utils::trace::TimeMetric m =
        files.empty() ? dftracer::utils::trace::TimeMetric::US
                      : dftracer::utils::trace::read_time_metric(files[0]);
    std::string s(dftracer::utils::trace::time_metric_to_string(m));
    for (char& c : s) c = static_cast<char>(std::tolower(c));
    return PyUnicode_FromString(s.c_str());
}

// One-pass read of the aggregation index's three record families, returned as a
// dict of native DataFrames: {"regular", "aggregated", "counters"}.
PyObject* tv_collect_typed(TraceViewerObject* self, PyObject* args,
                           PyObject* kwds) {
#ifndef DFTRACER_UTILS_ENABLE_ARROW
    PyErr_SetString(PyExc_RuntimeError,
                    "collect_typed() requires the arrow-enabled build");
    return nullptr;
#else
    int shard_begin = 0, shard_end = 0;  // shard_end <= 0 = all shards
    PyObject* progress_obj = nullptr;
    static const char* kwlist[] = {"shard_begin", "shard_end", "progress",
                                   nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iiO",
                                     const_cast<char**>(kwlist), &shard_begin,
                                     &shard_end, &progress_obj))
        return nullptr;

    // Bridge a Python (done, total) callback to the C++ scan; it fires from a
    // runtime worker thread while run_blocking holds the GIL released, so each
    // call re-acquires the GIL.
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
    TypedResult typed;
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan);
            typed = rt->submit(
                          v.collect_typed(shard_begin, shard_end, progress_ptr))
                        .get();
        }))
        return nullptr;
    namespace py = dftracer::utils::python;
    PyObject* regular = py::wrap_dataframe(std::move(typed.regular));
    if (!regular) return nullptr;
    PyObject* aggregated = py::wrap_dataframe(std::move(typed.aggregated));
    if (!aggregated) {
        Py_DECREF(regular);
        return nullptr;
    }
    PyObject* counters = py::wrap_dataframe(std::move(typed.counters));
    if (!counters) {
        Py_DECREF(regular);
        Py_DECREF(aggregated);
        return nullptr;
    }
    PyObject* d = PyDict_New();
    if (!d) {
        Py_DECREF(regular);
        Py_DECREF(aggregated);
        Py_DECREF(counters);
        return nullptr;
    }
    PyDict_SetItemString(d, "regular", regular);
    PyDict_SetItemString(d, "aggregated", aggregated);
    PyDict_SetItemString(d, "counters", counters);
    Py_DECREF(regular);
    Py_DECREF(aggregated);
    Py_DECREF(counters);
    return d;
#endif
}

// One-row summary aggregation: count, mean/std of dur, min/max ts.
PyObject* tv_statistics(TraceViewerObject* self, PyObject*) {
    Runtime* rt = resolve_runtime(self);
    auto files = extract_files(self);
    auto index_dir = extract_index_dir(self);
    ViewerPlan plan = *plan_of(self);
    DataFrame table;
    if (!run_blocking([&] {
            View v = build_view_from_data(files, index_dir, plan)
                         .group_by({})
                         .agg({AggSpec(AggOp::Count, "", "duration_count"),
                               AggSpec(AggOp::Mean, "dur", "duration_mean_us"),
                               AggSpec(AggOp::Std, "dur", "duration_stddev_us"),
                               AggSpec(AggOp::Min, "ts", "min_timestamp_us"),
                               AggSpec(AggOp::Max, "ts", "max_timestamp_us")});
            table = rt->submit(v.collect().collect()).get();
        }))
        return nullptr;

    PyObject* d = PyDict_New();
    if (!d) return nullptr;
    double count = 0, mean = 0, stddev = 0, mn = 0, mx = 0;
    if (table.num_rows() >= 1 && table.columns.size() >= 5) {
        count = dftracer::utils::dataframe::read_f64(table.columns[0], 0);
        mean = dftracer::utils::dataframe::read_f64(table.columns[1], 0);
        stddev = dftracer::utils::dataframe::read_f64(table.columns[2], 0);
        mn = dftracer::utils::dataframe::read_f64(table.columns[3], 0);
        mx = dftracer::utils::dataframe::read_f64(table.columns[4], 0);
    }
    dict_set_i64(d, "duration_count", (long long)count);
    dict_set_f64(d, "duration_mean_us", mean);
    dict_set_f64(d, "duration_stddev_us", stddev);
    dict_set_i64(d, "min_timestamp_us", (long long)mn);
    dict_set_i64(d, "max_timestamp_us", (long long)mx);
    return d;
}

}  // namespace dftracer::utils::python::trace_viewer_detail
