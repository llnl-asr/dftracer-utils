#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/trace_viewer_detail.h>
#include <dftracer/utils/trace/time_metric.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::python::trace_viewer_detail {

PyObject* tv_filter(TraceViewerObject* self, PyObject* arg) {
    // Accept a DSL string or a query.Expr (or any object whose str() is a DSL
    // predicate); str(str) is the string itself, so this stays zero-cost for
    // the common string case.
    PyObject* s = PyObject_Str(arg);
    if (!s) return nullptr;
    const char* dsl = PyUnicode_AsUTF8(s);
    if (!dsl) {
        Py_DECREF(s);
        return nullptr;
    }
    TraceViewerObject* c = clone(self);
    if (!c) {
        Py_DECREF(s);
        return nullptr;
    }
    plan_of(c)->filters.emplace_back(dsl);
    Py_DECREF(s);
    return (PyObject*)c;
}

PyObject* tv_group_by(TraceViewerObject* self, PyObject* args) {
    std::vector<GroupKey> keys;
    const Py_ssize_t n = PyTuple_Size(args);
    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* s = as_utf8(PyTuple_GetItem(args, i));
        if (!s) return nullptr;
        GroupKey k;
        if (!parse_group_key(s, k)) {
            PyErr_Format(PyExc_ValueError, "unknown group key: %s", s);
            return nullptr;
        }
        keys.push_back(k);
    }
    TraceViewerObject* c = clone_agg(self);
    if (!c) return nullptr;
    plan_of(c)->group_by = std::move(keys);
    return (PyObject*)c;
}

PyObject* tv_agg(TraceViewerObject* self, PyObject* args) {
    std::vector<AggSpec> specs;
    const Py_ssize_t n = PyTuple_Size(args);
    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* s = as_utf8(PyTuple_GetItem(args, i));
        if (!s) return nullptr;
        AggSpec spec;
        if (!parse_agg_spec(s, spec)) {
            PyErr_Format(PyExc_ValueError, "unknown agg spec: %s", s);
            return nullptr;
        }
        specs.push_back(spec);
    }
    TraceViewerObject* c = clone_agg(self);
    if (!c) return nullptr;
    plan_of(c)->agg = std::move(specs);
    return (PyObject*)c;
}

PyObject* tv_time_bucket(TraceViewerObject* self, PyObject* args,
                         PyObject* kwds) {
    long long us = 0;
    PyObject* normalize_to = nullptr;  // None, an int origin, or "min"
    static const char* kwlist[] = {"interval_us", "normalize_to", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "L|O", const_cast<char**>(kwlist), &us, &normalize_to))
        return nullptr;
    if (us < 0) {
        PyErr_SetString(PyExc_ValueError, "interval_us must be >= 0");
        return nullptr;
    }
    std::uint64_t origin = 0;
    bool origin_min = false;
    if (normalize_to && normalize_to != Py_None) {
        if (PyUnicode_Check(normalize_to)) {
            const char* s = PyUnicode_AsUTF8(normalize_to);
            if (!s) return nullptr;
            if (std::strcmp(s, "min") != 0) {
                PyErr_SetString(PyExc_ValueError,
                                "normalize_to must be an int origin or 'min'");
                return nullptr;
            }
            origin_min = true;
        } else {
            const long long o = PyLong_AsLongLong(normalize_to);
            if (o < 0 && PyErr_Occurred()) return nullptr;
            if (o < 0) {
                PyErr_SetString(PyExc_ValueError, "normalize_to must be >= 0");
                return nullptr;
            }
            origin = static_cast<std::uint64_t>(o);
        }
    }
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->time_bucket_us = static_cast<std::uint64_t>(us);
    plan_of(c)->bucket_origin_us = origin;
    plan_of(c)->bucket_origin_min = origin_min;
    return (PyObject*)c;
}

PyObject* tv_occ_cell(TraceViewerObject* self, PyObject* arg) {
    long long us = PyLong_AsLongLong(arg);
    if (us < 0 && PyErr_Occurred()) return nullptr;
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->occ_cell_us = (std::uint64_t)us;
    return (PyObject*)c;
}

PyObject* tv_time_unit(TraceViewerObject* self, PyObject* arg) {
    const char* s = as_utf8(arg);
    if (!s) return nullptr;
    std::string t(s);
    dftracer::utils::trace::TimeMetric target;
    if (t == "ns")
        target = dftracer::utils::trace::TimeMetric::NS;
    else if (t == "us")
        target = dftracer::utils::trace::TimeMetric::US;
    else if (t == "ms")
        target = dftracer::utils::trace::TimeMetric::MS;
    else if (t == "sec" || t == "s")
        target = dftracer::utils::trace::TimeMetric::SEC;
    else {
        PyErr_SetString(PyExc_ValueError, "time_unit must be ns/us/ms/sec/s");
        return nullptr;
    }
    // Source unit read once from the first file (assumes the run is uniform).
    auto files = extract_files(self);
    dftracer::utils::trace::TimeMetric source =
        files.empty() ? dftracer::utils::trace::TimeMetric::US
                      : dftracer::utils::trace::read_time_metric(files[0]);
    double scale =
        static_cast<double>(
            dftracer::utils::trace::time_metric_ns_per_unit(source)) /
        static_cast<double>(
            dftracer::utils::trace::time_metric_ns_per_unit(target));
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->time_scale = scale;
    return (PyObject*)c;
}

// Raw scale primitive matching C++ View::time_scale: multiply ts/dur/te by
// `ns_ratio` (source_ns / target_ns; 1.0 = none). time_unit() is the higher-
// level helper that derives this ratio from unit names.
PyObject* tv_time_scale(TraceViewerObject* self, PyObject* arg) {
    const double ratio = PyFloat_AsDouble(arg);
    if (ratio == -1.0 && PyErr_Occurred()) return nullptr;
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->time_scale = ratio;
    return (PyObject*)c;
}

PyObject* tv_time_range(TraceViewerObject* self, PyObject* args) {
    double begin, end;
    if (!PyArg_ParseTuple(args, "dd", &begin, &end)) return nullptr;
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->time_range = std::make_pair(begin, end);
    return (PyObject*)c;
}

PyObject* tv_select(TraceViewerObject* self, PyObject* args) {
    std::vector<std::string> cols;
    const Py_ssize_t n = PyTuple_Size(args);
    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* s = as_utf8(PyTuple_GetItem(args, i));
        if (!s) return nullptr;
        cols.emplace_back(s);
    }
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->select = std::move(cols);
    return (PyObject*)c;
}

PyObject* tv_memory_budget(TraceViewerObject* self, PyObject* arg) {
    long long b = PyLong_AsLongLong(arg);
    if (b < 0 && PyErr_Occurred()) return nullptr;
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->memory_budget = (std::uint64_t)b;
    return (PyObject*)c;
}

PyObject* tv_rollup_root(TraceViewerObject* self, PyObject* arg) {
    const char* dir = as_utf8(arg);
    if (!dir) return nullptr;
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->rollup_root = dir;
    return (PyObject*)c;
}

PyObject* tv_views_root(TraceViewerObject* self, PyObject* arg) {
    const char* dir = as_utf8(arg);
    if (!dir) return nullptr;
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->views_root = dir;
    return (PyObject*)c;
}

PyObject* tv_auto_spill(TraceViewerObject* self, PyObject*) {
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->auto_spill = true;
    return (PyObject*)c;
}

PyObject* tv_auto_numeric_args(TraceViewerObject* self, PyObject* args) {
    // Optional op names (e.g. "sum", "max", "mean") apply that reduction to
    // every discovered numeric arg; no args keeps the legacy bare-mean column.
    std::vector<AggSpec> reductions;
    const Py_ssize_t n = PyTuple_Size(args);
    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* s = as_utf8(PyTuple_GetItem(args, i));
        if (!s) return nullptr;
        AggSpec spec;
        if (!parse_agg_spec(s, spec)) {
            PyErr_Format(PyExc_ValueError, "unknown agg spec: %s", s);
            return nullptr;
        }
        reductions.push_back(spec);
    }
    TraceViewerObject* c = clone_agg(self);
    if (!c) return nullptr;
    plan_of(c)->auto_numeric = true;
    plan_of(c)->numeric_arg_aggs = std::move(reductions);
    return (PyObject*)c;
}

PyObject* tv_limit(TraceViewerObject* self, PyObject* arg) {
    long long v = PyLong_AsLongLong(arg);
    if (v < 0 && PyErr_Occurred()) return nullptr;
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->limit = (std::uint64_t)v;
    return (PyObject*)c;
}

// sort_by(name, descending=False): order the collect() result by a column.
PyObject* tv_sort_by(TraceViewerObject* self, PyObject* args, PyObject* kwds) {
    const char* name = nullptr;
    int descending = 0;
    static const char* kwlist[] = {"name", "descending", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "s|p", const_cast<char**>(kwlist), &name, &descending))
        return nullptr;
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->sort_col = name;
    plan_of(c)->sort_desc = descending != 0;
    return (PyObject*)c;
}

// topk(name, k, largest=True): keep the k best rows of the collect() result.
PyObject* tv_topk(TraceViewerObject* self, PyObject* args, PyObject* kwds) {
    const char* name = nullptr;
    Py_ssize_t k = 0;
    int largest = 1;
    static const char* kwlist[] = {"name", "k", "largest", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sn|p",
                                     const_cast<char**>(kwlist), &name, &k,
                                     &largest))
        return nullptr;
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->topk_col = name;
    plan_of(c)->topk_k = static_cast<std::int64_t>(k);
    plan_of(c)->topk_largest = largest != 0;
    return (PyObject*)c;
}

PyObject* tv_offset(TraceViewerObject* self, PyObject* arg) {
    long long v = PyLong_AsLongLong(arg);
    if (v < 0 && PyErr_Occurred()) return nullptr;
    TraceViewerObject* c = clone(self);
    if (!c) return nullptr;
    plan_of(c)->offset = (std::uint64_t)v;
    return (PyObject*)c;
}

}  // namespace dftracer::utils::python::trace_viewer_detail
