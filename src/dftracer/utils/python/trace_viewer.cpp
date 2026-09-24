#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
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
#include <dftracer/utils/python/trace_viewer.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/time_metric.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_source.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace py = dftracer::utils::python;
namespace views = dftracer::utils::trace::views;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::LazyFrame;
using views::AggOp;
using views::AggSpec;
using views::ExportStats;
using views::GroupKey;
using views::Phase;
using views::View;

View& tv_of(PyObject* self) {
    return *reinterpret_cast<TraceViewerObject*>(self)->tv;
}

PyObject* make_viewer(View&& t) {
    auto* self = reinterpret_cast<TraceViewerObject*>(
        TraceViewerType.tp_alloc(&TraceViewerType, 0));
    if (!self) return nullptr;
    self->tv = new View(std::move(t));
    return reinterpret_cast<PyObject*>(self);
}

// Runs `fn` with the GIL held, turning a C++ exception into the typed Python
// error.
template <class Fn>
PyObject* guarded(Fn&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        py::set_typed_py_error(e);
        return nullptr;
    }
}

template <class Fn>
PyObject* build(PyObject* self, Fn&& fn) {
    return guarded([&] { return make_viewer(fn(tv_of(self))); });
}

PyObject* stats_dict(const ExportStats& s) {
    PyObject* d = PyDict_New();
    if (!d) return nullptr;
    if (dict_set_i64(d, "events_matched",
                     static_cast<long long>(s.events_matched)) < 0 ||
        dict_set_i64(d, "events_scanned",
                     static_cast<long long>(s.events_scanned)) < 0 ||
        dict_set_i64(d, "chunks_scanned",
                     static_cast<long long>(s.chunks_scanned)) < 0 ||
        dict_set_i64(d, "chunks_skipped",
                     static_cast<long long>(s.chunks_skipped)) < 0 ||
        dict_set_i64(d, "chunks_covered",
                     static_cast<long long>(s.chunks_covered)) < 0 ||
        dict_set_bool(d, "artifacts_committed", s.artifacts_committed) < 0 ||
        dict_set_bool(d, "truncated", s.truncated) < 0 ||
        dict_set_bool(d, "served_from_mv", s.served_from_mv) < 0) {
        Py_DECREF(d);
        return nullptr;
    }
    return d;
}

// A Python (done, total) callback as a ProgressFn. It fires on a runtime
// worker, so each call takes the GIL.
views::ProgressFn progress_from(PyObject* obj) {
    if (!obj || obj == Py_None) return {};
    Py_INCREF(obj);
    std::shared_ptr<PyObject> cb(obj, [](PyObject* p) {
        PyGILState_STATE g = PyGILState_Ensure();
        Py_DECREF(p);
        PyGILState_Release(g);
    });
    return [cb](std::size_t done, std::size_t total) {
        PyGILState_STATE g = PyGILState_Ensure();
        PyObject* r =
            PyObject_CallFunction(cb.get(), "nn", static_cast<Py_ssize_t>(done),
                                  static_cast<Py_ssize_t>(total));
        if (r)
            Py_DECREF(r);
        else
            PyErr_Clear();
        PyGILState_Release(g);
    };
}

class FileSink : public views::ExportSink {
   public:
    explicit FileSink(FILE* f) : f_(f) {}
    ~FileSink() override {
        if (f_) std::fclose(f_);
    }
    void write(std::string_view data) override {
        std::fwrite(data.data(), 1, data.size(), f_);
    }
    void flush() override { std::fflush(f_); }

   private:
    FILE* f_;
};

// Buffers writes and flushes whole-line gzip members past MEMBER_TARGET, so
// the output is a multi-member re-indexable trace.
class GzipSink : public views::ExportSink {
   public:
    GzipSink(FILE* f, int level) : f_(f), comp_(level) {}
    ~GzipSink() override {
        if (!buf_.empty()) flush(buf_.size());
        std::fclose(f_);
    }
    void write(std::string_view data) override {
        buf_.append(data);
        while (buf_.size() >= MEMBER_TARGET) {
            std::size_t cut = buf_.rfind('\n', buf_.size());
            if (cut == std::string::npos || cut + 1 < MEMBER_TARGET) break;
            flush(cut + 1);
        }
    }

   private:
    static constexpr std::size_t MEMBER_TARGET = 4 * 1024 * 1024;
    void flush(std::size_t n) {
        if (comp_.compress_member_into(scratch_, buf_.data(), n))
            std::fwrite(scratch_.data(), 1, scratch_.size(), f_);
        buf_.erase(0, n);
    }
    FILE* f_;
    dftracer::utils::utilities::fileio::compress::GzipMemberCompressor comp_;
    std::string buf_;
    std::vector<std::uint8_t> scratch_;
};

// "fn(key)" or "fn(key, 'a', 'b')" -> transform + inner key text; false when
// `t` is not a call, leaving `t` to parse as a plain key.
bool split_transform(const std::string& t, GroupKey::Transform& tf,
                     std::vector<std::string>& targs, std::string& inner) {
    const auto lp = t.find('(');
    if (lp == std::string::npos || t.back() != ')') return false;
    const std::string fn = t.substr(0, lp);
    if (fn == "dirname")
        tf = GroupKey::Transform::Dirname;
    else if (fn == "basename")
        tf = GroupKey::Transform::Basename;
    else if (fn == "lower")
        tf = GroupKey::Transform::Lower;
    else if (fn == "bucket")
        tf = GroupKey::Transform::Bucket;
    else
        return false;
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : t.substr(lp + 1, t.size() - lp - 2)) {
        if (ch == ',') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    parts.push_back(cur);
    auto trim = [](const std::string& x) {
        const auto b = x.find_first_not_of(" \t'\"");
        const auto e = x.find_last_not_of(" \t'\"");
        return b == std::string::npos ? std::string() : x.substr(b, e - b + 1);
    };
    inner = trim(parts[0]);
    for (std::size_t i = 1; i < parts.size(); ++i)
        targs.push_back(trim(parts[i]));
    return !inner.empty();
}

// name | cat | pid | tid | fhash | hhash | io_cat | acc_pat | rank |
// file_path | file_name | host_name | arg:<key> | any other field, each
// optionally wrapped in a transform call.
GroupKey parse_group_key(const std::string& s) {
    std::string t = s;
    GroupKey::Transform tf = GroupKey::Transform::None;
    std::vector<std::string> targs;
    std::string inner;
    if (split_transform(t, tf, targs, inner)) t = inner;
    GroupKey out;
    if (t == "name")
        out = GroupKey::name();
    else if (t == "cat")
        out = GroupKey::cat();
    else if (t == "pid")
        out = GroupKey::pid();
    else if (t == "tid")
        out = GroupKey::tid();
    else if (t == "fhash")
        out = GroupKey::fhash();
    else if (t == "hhash")
        out = GroupKey::hhash();
    else if (t == "io_cat")
        out = GroupKey::io_cat();
    else if (t == "acc_pat")
        out = GroupKey::acc_pat();
    else if (t == "file_path" || t == "resolved.fpath" || t == "r.fpath")
        out = GroupKey::file_path();
    else if (t == "file_name")
        out = GroupKey::file_name();
    else if (t == "host_name" || t == "resolved.hostname" ||
             t == "r.hostname" || t == "resolved.host" || t == "r.host")
        out = GroupKey::host_name();
    else if (t == "rank")
        out = GroupKey::rank();
    else if (t.rfind("arg:", 0) == 0)
        out = GroupKey::of_arg(t.substr(4));
    else
        out = GroupKey::field(t);
    out.transform = tf;
    out.transform_args = std::move(targs);
    return out;
}

// "count" | "op:field" | "argmax:field:by" | "pct:field:q" | "pNN:field"
std::optional<AggSpec> parse_agg_spec(const std::string& t) {
    auto c1 = t.find(':');
    std::string op = t.substr(0, c1);
    std::string field, by;
    if (c1 != std::string::npos) {
        std::string rest = t.substr(c1 + 1);
        auto c2 = rest.find(':');
        field = rest.substr(0, c2);
        if (c2 != std::string::npos) by = rest.substr(c2 + 1);
    }
    const std::string occ = field.empty() ? "dur" : field;
    if (op == "count") return AggSpec(AggOp::Count, "", "", "");
    if (op == "sum") return AggSpec(AggOp::Sum, field);
    if (op == "sumsq") return AggSpec(AggOp::SumSq, field);
    if (op == "min") return AggSpec(AggOp::Min, field);
    if (op == "max") return AggSpec(AggOp::Max, field);
    if (op == "mean") return AggSpec(AggOp::Mean, field);
    if (op == "var") return AggSpec(AggOp::Var, field);
    if (op == "std") return AggSpec(AggOp::Std, field);
    if (op == "skew") return AggSpec(AggOp::Skew, field);
    if (op == "kurt") return AggSpec(AggOp::Kurt, field);
    if (op == "hist") return AggSpec(AggOp::Hist, field);
    if (op == "argmax") return AggSpec(AggOp::ArgMax, field, "", by);
    if (op == "set_union" || op == "uniq")
        return AggSpec(AggOp::SetUnion, field);
    if (op == "busy") return AggSpec(AggOp::Busy, occ);
    if (op == "concurrency") return AggSpec(AggOp::Concurrency, occ);
    if (op == "utilization") return AggSpec(AggOp::Utilization, occ);
    if (op == "active") return AggSpec(AggOp::Active, occ);
    if (op == "pct")
        return AggSpec(AggOp::Pct, field, "", "",
                       by.empty() ? 0.0 : std::stod(by));
    if (op.size() >= 2 && op[0] == 'p') {
        double denom = 1.0;
        for (std::size_t i = 1; i < op.size(); ++i) {
            if (op[i] < '0' || op[i] > '9') return std::nullopt;
            denom *= 10.0;
        }
        return AggSpec(AggOp::Pct, field, op + "_" + field, "",
                       std::stod(op.substr(1)) / denom);
    }
    return std::nullopt;
}

bool parse_specs(PyObject* args, std::vector<AggSpec>& out) {
    const Py_ssize_t n = PyTuple_Size(args);
    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* s = as_utf8(PyTuple_GetItem(args, i));
        if (!s) return false;
        std::optional<AggSpec> spec = parse_agg_spec(s);
        if (!spec) {
            PyErr_Format(PyExc_ValueError, "unknown agg spec: %s", s);
            return false;
        }
        out.push_back(std::move(*spec));
    }
    return true;
}

bool parse_containment(PyObject* args, PyObject* kwds, bool with_group,
                       views::ContainmentArgs& out) {
    static const char* kw_group[] = {"partition", "ts",    "dur",
                                     "name",      "group", nullptr};
    static const char* kw_plain[] = {"partition", "ts", "dur", "name", nullptr};
    PyObject* part = nullptr;
    PyObject* group = nullptr;
    const char* ts = "ts";
    const char* dur = "dur";
    const char* name = "name";
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, with_group ? "|OsssO" : "|Osss",
            const_cast<char**>(with_group ? kw_group : kw_plain), &part, &ts,
            &dur, &name, &group))
        return false;
    if (part && part != Py_None) {
        out.partition.clear();
        if (!py::parse_string_seq(part, "partition must be a sequence",
                                  out.partition))
            return false;
    }
    if (group && group != Py_None &&
        !py::parse_string_seq(group, "group must be a sequence", out.group))
        return false;
    out.ts = ts;
    out.dur = dur;
    out.name = name;
    return true;
}

// Runs the task `make` builds on the runtime `runtime_arg` names, with the GIL
// released.
template <class Make, class T>
bool run_on(PyObject* runtime_arg, Make&& make, T& out) {
    std::shared_ptr<dftracer::utils::Runtime> rt =
        runtime_from_arg(runtime_arg);
    if (!rt) return false;
    return run_blocking([&] { out = rt->submit(make()).get(); });
}

PyObject* tv_new(PyTypeObject* type, PyObject*, PyObject*) {
    auto* self = reinterpret_cast<TraceViewerObject*>(type->tp_alloc(type, 0));
    if (self) self->tv = nullptr;
    return reinterpret_cast<PyObject*>(self);
}

void tv_dealloc(TraceViewerObject* self) {
    delete self->tv;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

int tv_init(TraceViewerObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"files", "index_path", nullptr};
    PyObject* files = nullptr;
    PyObject* index_obj = nullptr;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "O|O", const_cast<char**>(kwlist), &files, &index_obj))
        return -1;
    std::string index_dir;
    if (index_obj && index_obj != Py_None) {
        const char* s = as_utf8(index_obj);
        if (!s) return -1;
        index_dir = s;
    }
    std::vector<std::string> paths;
    std::optional<std::string> dir;
    if (PyUnicode_Check(files)) {
        const char* path = PyUnicode_AsUTF8(files);
        if (!path) return -1;
        std::error_code ec;
        if (fs::is_directory(path, ec))
            dir = path;
        else
            paths.emplace_back(path);
    } else if (!py::parse_string_seq(files,
                                     "files must be a path or a "
                                     "sequence of paths",
                                     paths)) {
        return -1;
    }
    View tv;
    if (dir) {
        if (!run_blocking([&] {
                tv = py::get_default_runtime()
                         ->submit(View::from_directory(*dir, index_dir))
                         .get();
            }))
            return -1;
    } else {
        std::vector<views::ViewFile> vfiles;
        vfiles.reserve(paths.size());
        for (const std::string& p : paths)
            vfiles.push_back(views::ViewFile{
                p, dftracer::utils::trace::internal::determine_index_path(
                       p, index_dir)});
        tv = View::from_files(std::move(vfiles));
    }
    delete self->tv;
    self->tv = new View(std::move(tv));
    return 0;
}

PyObject* tv_lazy(PyObject* self, PyObject*) {
    return guarded(
        [&] { return py::wrap_lazyframe(LazyFrame(tv_of(self).lazy())); });
}

PyObject* tv_with_lazy(PyObject* self, PyObject* arg) {
    const LazyFrame* lf = py::lazyframe_of(arg);
    if (!lf) return nullptr;
    return build(self, [&](const View& t) { return t.with_lazy(*lf); });
}

PyObject* tv_filter(PyObject* self, PyObject* arg) {
    PyObject* s = PyObject_Str(arg);
    if (!s) return nullptr;
    const char* dsl = PyUnicode_AsUTF8(s);
    if (!dsl) {
        Py_DECREF(s);
        return nullptr;
    }
    auto parsed = dftracer::utils::query::Query::from_string(dsl);
    Py_DECREF(s);
    if (!parsed) {
        PyErr_Format(PyExc_ValueError, "invalid filter query: %s",
                     parsed.error().message.c_str());
        return nullptr;
    }
    return build(self, [&](const View& t) {
        return t.filter(std::move(parsed.value()));
    });
}

PyObject* tv_select(PyObject* self, PyObject* arg) {
    std::vector<std::string> names;
    if (!py::parse_string_seq(arg, "select expects a sequence of names", names))
        return nullptr;
    return build(self,
                 [&](const View& t) { return t.select(std::move(names)); });
}

PyObject* tv_phase(PyObject* self, PyObject* arg) {
    const char* s = as_utf8(arg);
    if (!s) return nullptr;
    const std::string t(s);
    Phase ph;
    if (t == "events")
        ph = Phase::Events;
    else if (t == "counters")
        ph = Phase::Counters;
    else if (t == "aggregated")
        ph = Phase::Aggregated;
    else if (t == "metadata")
        ph = Phase::Metadata;
    else if (t == "any")
        ph = Phase::Any;
    else {
        PyErr_SetString(PyExc_ValueError,
                        "phase must be 'events', 'counters', 'aggregated', "
                        "'metadata', or 'any'");
        return nullptr;
    }
    return build(self, [&](const View& v) { return v.phase(ph); });
}

PyObject* tv_time_range(PyObject* self, PyObject* args) {
    double begin = 0, end = 0;
    if (!PyArg_ParseTuple(args, "dd", &begin, &end)) return nullptr;
    return build(self, [&](const View& t) { return t.time_range(begin, end); });
}

PyObject* tv_time_bucket(PyObject* self, PyObject* args, PyObject* kwds) {
    long long us = 0;
    PyObject* normalize_to = nullptr;
    static const char* kwlist[] = {"interval_us", "normalize_to", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "L|O", const_cast<char**>(kwlist), &us, &normalize_to))
        return nullptr;
    if (us < 0) {
        PyErr_SetString(PyExc_ValueError, "interval_us must be >= 0");
        return nullptr;
    }
    const auto width = static_cast<std::uint64_t>(us);
    if (!normalize_to || normalize_to == Py_None)
        return build(self, [&](const View& t) { return t.time_bucket(width); });
    if (PyUnicode_Check(normalize_to)) {
        const char* s = PyUnicode_AsUTF8(normalize_to);
        if (!s) return nullptr;
        if (std::strcmp(s, "min") != 0) {
            PyErr_SetString(PyExc_ValueError,
                            "normalize_to must be an int origin or 'min'");
            return nullptr;
        }
        return build(self,
                     [&](const View& t) { return t.time_bucket_min(width); });
    }
    const long long origin = PyLong_AsLongLong(normalize_to);
    if (origin == -1 && PyErr_Occurred()) return nullptr;
    if (origin < 0) {
        PyErr_SetString(PyExc_ValueError, "normalize_to must be >= 0");
        return nullptr;
    }
    return build(self, [&](const View& t) {
        return t.time_bucket(width, static_cast<std::uint64_t>(origin));
    });
}

PyObject* tv_resolution(PyObject* self, PyObject* arg) {
    const long long us = PyLong_AsLongLong(arg);
    if (us == -1 && PyErr_Occurred()) return nullptr;
    if (us < 0) {
        PyErr_SetString(PyExc_ValueError, "resolution must be >= 0");
        return nullptr;
    }
    return build(self, [&](const View& t) {
        return t.resolution(static_cast<std::uint64_t>(us));
    });
}

PyObject* tv_time_scale(PyObject* self, PyObject* arg) {
    const double ratio = PyFloat_AsDouble(arg);
    if (ratio == -1.0 && PyErr_Occurred()) return nullptr;
    return build(self, [&](const View& t) { return t.time_scale(ratio); });
}

PyObject* tv_time_unit(PyObject* self, PyObject* arg) {
    namespace trace = dftracer::utils::trace;
    const char* s = as_utf8(arg);
    if (!s) return nullptr;
    const std::string t(s);
    trace::TimeMetric target;
    if (t == "ns")
        target = trace::TimeMetric::NS;
    else if (t == "us")
        target = trace::TimeMetric::US;
    else if (t == "ms")
        target = trace::TimeMetric::MS;
    else if (t == "sec" || t == "s")
        target = trace::TimeMetric::SEC;
    else {
        PyErr_SetString(PyExc_ValueError, "time_unit must be ns/us/ms/sec/s");
        return nullptr;
    }
    return build(self, [&](const View& v) {
        const double ratio =
            static_cast<double>(
                trace::time_metric_ns_per_unit(v.time_metric())) /
            static_cast<double>(trace::time_metric_ns_per_unit(target));
        return v.time_scale(ratio);
    });
}

PyObject* tv_group_by(PyObject* self, PyObject* args) {
    std::vector<GroupKey> keys;
    const Py_ssize_t n = PyTuple_Size(args);
    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* s = as_utf8(PyTuple_GetItem(args, i));
        if (!s) return nullptr;
        keys.push_back(parse_group_key(s));
    }
    return build(self,
                 [&](const View& t) { return t.group_by(std::move(keys)); });
}

PyObject* tv_agg(PyObject* self, PyObject* args) {
    std::vector<AggSpec> specs;
    if (!parse_specs(args, specs)) return nullptr;
    return build(self, [&](const View& t) { return t.agg(std::move(specs)); });
}

PyObject* tv_agg_numeric_args(PyObject* self, PyObject* args) {
    std::vector<AggSpec> specs;
    if (!parse_specs(args, specs)) return nullptr;
    return build(self, [&](const View& t) {
        return specs.empty() ? t.agg_numeric_args()
                             : t.agg_numeric_args(std::move(specs));
    });
}

PyObject* tv_metadata(PyObject* self, PyObject* arg) {
    const int on = PyObject_IsTrue(arg);
    if (on < 0) return nullptr;
    return build(self, [&](const View& t) { return t.metadata(on != 0); });
}

PyObject* tv_rollup_root(PyObject* self, PyObject* arg) {
    const char* dir = as_utf8(arg);
    if (!dir) return nullptr;
    return build(self, [&](const View& t) { return t.rollup_root(dir); });
}

PyObject* tv_views_root(PyObject* self, PyObject* arg) {
    const char* dir = as_utf8(arg);
    if (!dir) return nullptr;
    return build(self, [&](const View& t) { return t.views_root(dir); });
}

PyObject* tv_memory_budget(PyObject* self, PyObject* arg) {
    const long long b = PyLong_AsLongLong(arg);
    if (b == -1 && PyErr_Occurred()) return nullptr;
    if (b < 0) {
        PyErr_SetString(PyExc_ValueError, "memory_budget must be >= 0");
        return nullptr;
    }
    return build(self, [&](const View& t) {
        return t.memory_budget(static_cast<std::uint64_t>(b));
    });
}

PyObject* tv_columns(PyObject* self, PyObject*) {
    std::vector<std::string> cols;
    if (!run_blocking([&] { cols = tv_of(self).columns(); })) return nullptr;
    return str_list_from(cols);
}

PyObject* tv_column_info(PyObject* self, PyObject*) {
    std::vector<views::ColumnInfo> info;
    if (!run_blocking([&] { info = tv_of(self).column_info(); }))
        return nullptr;
    PyObject* d = PyDict_New();
    if (!d) return nullptr;
    for (const auto& c : info) {
        PyObject* v = PyUnicode_FromString(c.type.c_str());
        if (!v || PyDict_SetItemString(d, c.name.c_str(), v) < 0) {
            Py_XDECREF(v);
            Py_DECREF(d);
            return nullptr;
        }
        Py_DECREF(v);
    }
    return d;
}

PyObject* tv_time_metric(PyObject* self, PyObject*) {
    std::string s(dftracer::utils::trace::time_metric_to_string(
        tv_of(self).time_metric()));
    for (char& c : s) c = static_cast<char>(std::tolower(c));
    return PyUnicode_FromString(s.c_str());
}

PyObject* tv_aggregates(PyObject* self, PyObject*) {
    return guarded([&] { return PyBool_FromLong(tv_of(self).aggregates()); });
}

// True while a filter still selects raw events: no trace aggregation and no
// op but filters on the plan. Reads no index.
PyObject* tv_filters_events(PyObject* self, PyObject*) {
    return PyBool_FromLong(tv_of(self).filters_events());
}

PyObject* tv_call_tree(PyObject* self, PyObject* args, PyObject* kwds) {
    views::ContainmentArgs a;
    if (!parse_containment(args, kwds, false, a)) return nullptr;
    return guarded([&] {
        return py::wrap_lazyframe(
            tv_of(self).call_tree(a.partition, a.ts, a.dur, a.name));
    });
}

PyObject* tv_flamegraph(PyObject* self, PyObject* args, PyObject* kwds) {
    views::ContainmentArgs a;
    if (!parse_containment(args, kwds, true, a)) return nullptr;
    return guarded([&] {
        return py::wrap_lazyframe(
            tv_of(self).flamegraph(a.partition, a.ts, a.dur, a.name, a.group));
    });
}

PyObject* tv_containment(PyObject* self, PyObject* args, PyObject* kwds) {
    views::ContainmentArgs a;
    if (!parse_containment(args, kwds, true, a)) return nullptr;
    return guarded([&]() -> PyObject* {
        auto r =
            tv_of(self).containment(a.partition, a.ts, a.dur, a.name, a.group);
        PyObject* ct = py::wrap_lazyframe(LazyFrame(r.plans()[0]));
        if (!ct) return nullptr;
        PyObject* fg = py::wrap_lazyframe(LazyFrame(r.plans()[1]));
        if (!fg) {
            Py_DECREF(ct);
            return nullptr;
        }
        return Py_BuildValue("(NN)", ct, fg);
    });
}

PyObject* tv_flamegraph_partial(PyObject* self, PyObject* args,
                                PyObject* kwds) {
    views::ContainmentArgs a;
    if (!parse_containment(args, kwds, true, a)) return nullptr;
    return guarded([&] {
        return py::wrap_lazyframe(LazyFrame(
            tv_of(self)
                .flamegraph_partial(a.partition, a.ts, a.dur, a.name, a.group)
                .plans()
                .front()));
    });
}

PyObject* tv_aggregate_partial(PyObject* self, PyObject*) {
    return guarded([&] {
        return py::wrap_lazyframe(
            LazyFrame(tv_of(self).aggregate_partial().plans().front()));
    });
}

PyObject* tv_sink_json(PyObject* self, PyObject* arg) {
    const char* path = as_utf8(arg);
    if (!path) return nullptr;
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        return nullptr;
    }
    auto sink = std::make_shared<FileSink>(f);
    return guarded([&] {
        return py::wrap_lazyframe(LazyFrame(
            tv_of(self).sink_json(sink, views::LAZY).plans().front()));
    });
}

PyObject* tv_typed(PyObject* self, PyObject* args, PyObject* kwds) {
    int shard_begin = 0, shard_end = 0;
    PyObject* progress = nullptr;
    PyObject* runtime_arg = nullptr;
    static const char* kwlist[] = {"shard_begin", "shard_end", "progress",
                                   "runtime", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iiOO",
                                     const_cast<char**>(kwlist), &shard_begin,
                                     &shard_end, &progress, &runtime_arg))
        return nullptr;
    views::TypedResult typed;
    views::ProgressFn fn = progress_from(progress);
    if (!run_on(
            runtime_arg,
            [&] {
                return tv_of(self).collect_typed(shard_begin, shard_end, fn);
            },
            typed))
        return nullptr;
    PyObject* d = PyDict_New();
    if (!d) return nullptr;
    const std::pair<const char*, DataFrame*> parts[] = {
        {"regular", &typed.regular},
        {"aggregated", &typed.aggregated},
        {"counters", &typed.counters}};
    for (const auto& [key, frame] : parts) {
        PyObject* v = py::wrap_dataframe(std::move(*frame));
        if (!v || PyDict_SetItemString(d, key, v) < 0) {
            Py_XDECREF(v);
            Py_DECREF(d);
            return nullptr;
        }
        Py_DECREF(v);
    }
    return d;
}

PyObject* tv_materialize(PyObject* self, PyObject* args, PyObject* kwds) {
    long long checkpoint_size = 0, part_size = 0;
    PyObject* progress = nullptr;
    PyObject* runtime_arg = nullptr;
    static const char* kwlist[] = {"checkpoint_size", "part_size", "progress",
                                   "runtime", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|LLOO", const_cast<char**>(kwlist), &checkpoint_size,
            &part_size, &progress, &runtime_arg))
        return nullptr;
    ExportStats stats;
    views::ProgressFn fn = progress_from(progress);
    if (!run_on(
            runtime_arg,
            [&] {
                return tv_of(self).materialize(
                    static_cast<std::uint64_t>(checkpoint_size),
                    static_cast<std::uint64_t>(part_size), fn);
            },
            stats))
        return nullptr;
    return stats_dict(stats);
}

// A trace of the aggregation (counter events) when the viewer aggregates, else
// of the selected events.
PyObject* tv_export_trace(PyObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"path",        "compress", "index",
                                   "member_size", "level",    "part_size",
                                   "runtime",     nullptr};
    const char* path = nullptr;
    int compress = 1;
    int index = 0;
    long long member_size = 0;
    int level = 6;
    long long part_size = 0;
    PyObject* runtime_arg = nullptr;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "s|ppLiLO", const_cast<char**>(kwlist), &path,
            &compress, &index, &member_size, &level, &part_size, &runtime_arg))
        return nullptr;
    const View& t = tv_of(self);
    ExportStats stats;
    bool aggregates = false;
    try {
        aggregates = t.aggregates();
    } catch (const std::exception& e) {
        py::set_typed_py_error(e);
        return nullptr;
    }
    if (aggregates) {
        FILE* f = std::fopen(path, "wb");
        if (!f) {
            PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
            return nullptr;
        }
        std::unique_ptr<views::ExportSink> sink;
        if (compress)
            sink = std::make_unique<GzipSink>(f, level);
        else
            sink = std::make_unique<FileSink>(f);
        const bool ok =
            run_on(runtime_arg, [&] { return t.sink_counters(*sink); }, stats);
        sink.reset();
        if (!ok) return nullptr;
        return stats_dict(stats);
    }
    views::TraceWriteOptions opts;
    opts.output_path = path;
    opts.member_size = static_cast<std::size_t>(member_size);
    opts.compress = compress != 0;
    opts.level = level;
    opts.build_index = index != 0;
    opts.part_size = static_cast<std::size_t>(part_size);
    if (!run_on(
            runtime_arg, [&] { return t.sink_trace(std::move(opts)); }, stats))
        return nullptr;
    return stats_dict(stats);
}

PyObject* tv_merge_partials(PyObject* self, PyObject* arg) {
    std::vector<std::string> owned;
    if (!py::parse_bytes_seq(arg, "merge_partials expects a sequence", owned))
        return nullptr;
    std::vector<std::string_view> parts(owned.begin(), owned.end());
    DataFrame out;
    if (!run_blocking([&] { out = tv_of(self).merge_partials(parts); }))
        return nullptr;
    return py::wrap_dataframe(std::move(out));
}

PyObject* tv_materialize_partials(PyObject* self, PyObject* args,
                                  PyObject* kwds) {
    PyObject* arg = nullptr;
    PyObject* runtime_arg = nullptr;
    static const char* kwlist[] = {"partials", "runtime", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "O|O", const_cast<char**>(kwlist), &arg, &runtime_arg))
        return nullptr;
    std::vector<std::string> owned;
    if (!py::parse_bytes_seq(arg, "materialize_partials expects a sequence",
                             owned))
        return nullptr;
    std::vector<std::string_view> parts(owned.begin(), owned.end());
    std::shared_ptr<dftracer::utils::Runtime> rt =
        runtime_from_arg(runtime_arg);
    if (!rt) return nullptr;
    if (!run_blocking(
            [&] { rt->submit(tv_of(self).materialize_partials(parts)).get(); }))
        return nullptr;
    Py_RETURN_NONE;
}

PyObject* tv_reconstruct_if_cached(PyObject* self, PyObject*) {
    std::optional<DataFrame> out;
    if (!run_blocking([&] { out = tv_of(self).reconstruct_if_cached(); }))
        return nullptr;
    if (!out) Py_RETURN_NONE;
    return py::wrap_dataframe(std::move(*out));
}

PyObject* tv_mv_source(PyObject* self, PyObject*) {
    std::vector<std::string> out;
    if (!run_blocking([&] { out = tv_of(self).mv_source(); })) return nullptr;
    return str_list_from(out);
}

PyObject* tv_materialize_dir(PyObject* self, PyObject*) {
    std::string out;
    if (!run_blocking([&] { out = tv_of(self).materialize_dir(); }))
        return nullptr;
    return PyUnicode_FromStringAndSize(out.data(),
                                       static_cast<Py_ssize_t>(out.size()));
}

PyObject* tv_register_materialized(PyObject* self, PyObject* arg) {
    const char* dir = as_utf8(arg);
    if (!dir) return nullptr;
    const std::string d(dir);
    if (!run_blocking([&] { tv_of(self).register_materialized(d); }))
        return nullptr;
    Py_RETURN_NONE;
}

PyObject* tv_compare(PyObject* self, PyObject* arg) {
    if (!PyObject_TypeCheck(arg, &TraceViewerType)) {
        PyErr_SetString(PyExc_TypeError, "compare() needs a _TraceViewer");
        return nullptr;
    }
    return guarded(
        [&] { return py::wrap_lazyframe(tv_of(self).compare(tv_of(arg))); });
}

// A branch that folds `host`'s plugin set over the scan and leaves the named
// results in the host, where plugin_results() reads them after the collect.
PyObject* tv_plugins(PyObject* self, PyObject* host) {
    if (!PyObject_TypeCheck(host, &PluginHostType)) {
        PyErr_SetString(PyExc_TypeError, "plugins() needs a Plugins instance");
        return nullptr;
    }
    const dftracer::utils::plugins::Plugins* set =
        py::plugin_host_plugins(host);
    if (!set) return nullptr;
    dftracer::utils::plugins::NamedResultRegistry* results =
        py::plugin_host_results(host);
    if (!results) return nullptr;
    Py_INCREF(host);
    std::shared_ptr<PyObject> keep(host, [](PyObject* p) {
        PyGILState_STATE g = PyGILState_Ensure();
        Py_DECREF(p);
        PyGILState_Release(g);
    });
    views::SessionBranch attach = [set, results, keep](views::ViewSession& s) {
        views::Deferred<dftracer::utils::plugins::PluginRun> h = set->attach(s);
        return std::function<void(const ExportStats&)>(
            [h, results, keep](const ExportStats&) mutable {
                *results = std::move(h.get().results);
            });
    };
    return guarded([&] {
        return py::wrap_lazyframe(tv_of(self).branch(std::move(attach)));
    });
}

PyObject* merge_flamegraph_partials_py(PyObject*, PyObject* arg) {
    std::vector<std::string> owned;
    if (!py::parse_bytes_seq(
            arg, "merge_flamegraph_partials expects a sequence", owned))
        return nullptr;
    std::vector<std::string_view> parts(owned.begin(), owned.end());
    return guarded([&] {
        return py::wrap_dataframe(View::merge_flamegraph_partials(parts));
    });
}

PyObject* plugin_results_py(PyObject*, PyObject* host) {
    if (!PyObject_TypeCheck(host, &PluginHostType)) {
        PyErr_SetString(PyExc_TypeError,
                        "plugin_results() needs a Plugins instance");
        return nullptr;
    }
    return py::plugin_host_results_dict(host);
}

PyMethodDef tv_methods[] = {
    {"lazy", tv_lazy, METH_NOARGS, "The plan as a _LazyFrame."},
    {"with_lazy", tv_with_lazy, METH_O,
     "This scan with `plan` (a _LazyFrame over it) as its plan."},
    {"filter", tv_filter, METH_O,
     "Keep events matching a query-DSL predicate."},
    {"select", tv_select, METH_O,
     "Fields the scan reads (raw events), or a projection of the plan."},
    {"phase", tv_phase, METH_O,
     "Select 'events', 'counters', 'aggregated', 'metadata', or 'any'."},
    {"time_range", tv_time_range, METH_VARARGS,
     "Restrict to a [begin, end) timestamp window."},
    {"time_bucket", DFTU_PYCFUNCTION(tv_time_bucket),
     METH_VARARGS | METH_KEYWORDS,
     "time_bucket(interval_us, normalize_to=None); normalize_to is an int "
     "origin or 'min'."},
    {"resolution", tv_resolution, METH_O,
     "Grid in microseconds the occupancy aggregates snap to; 0 is exact."},
    {"time_scale", tv_time_scale, METH_O, "Multiply ts/dur by this ratio."},
    {"time_unit", tv_time_unit, METH_O,
     "Normalize ts/dur to ns/us/ms/sec from the trace's own unit."},
    {"group_by", tv_group_by, METH_VARARGS, "Trace group keys."},
    {"agg", tv_agg, METH_VARARGS, "Trace aggregate specs."},
    {"agg_numeric_args", tv_agg_numeric_args, METH_VARARGS,
     "Aggregate every discovered numeric arg."},
    {"metadata", tv_metadata, METH_O, "Include metadata records."},
    {"rollup_root", tv_rollup_root, METH_O, "Rollup root directory."},
    {"views_root", tv_views_root, METH_O, "Materialized-view root directory."},
    {"memory_budget", tv_memory_budget, METH_O, "Spill budget in bytes."},
    {"columns", tv_columns, METH_NOARGS,
     "Columns discoverable from the index (no scan)."},
    {"column_info", tv_column_info, METH_NOARGS,
     "Index columns mapped to their type name (no scan)."},
    {"time_metric", tv_time_metric, METH_NOARGS, "The trace's time unit."},
    {"aggregates", tv_aggregates, METH_NOARGS,
     "True when the plan absorbs into an aggregating scan."},
    {"filters_events", tv_filters_events, METH_NOARGS,
     "True while a filter still selects raw events (reads no index)."},
    {"call_tree", DFTU_PYCFUNCTION(tv_call_tree), METH_VARARGS | METH_KEYWORDS,
     "Plan of the events plus level/parent_id."},
    {"flamegraph", DFTU_PYCFUNCTION(tv_flamegraph),
     METH_VARARGS | METH_KEYWORDS, "Plan of the folded node frame."},
    {"containment", DFTU_PYCFUNCTION(tv_containment),
     METH_VARARGS | METH_KEYWORDS,
     "(call_tree plan, flamegraph plan) sharing one buffered fold."},
    {"flamegraph_partial", DFTU_PYCFUNCTION(tv_flamegraph_partial),
     METH_VARARGS | METH_KEYWORDS, "Plan of a one-row 'partial' frame."},
    {"aggregate_partial", tv_aggregate_partial, METH_NOARGS,
     "Plan of a one-row 'partial' frame."},
    {"sink_json", tv_sink_json, METH_O,
     "Plan writing the selected events to `path`; yields one stats row."},
    {"typed", DFTU_PYCFUNCTION(tv_typed), METH_VARARGS | METH_KEYWORDS,
     "The aggregation index's record families (runs now)."},
    {"materialize", DFTU_PYCFUNCTION(tv_materialize),
     METH_VARARGS | METH_KEYWORDS, "Persist this query (runs now)."},
    {"export_trace", DFTU_PYCFUNCTION(tv_export_trace),
     METH_VARARGS | METH_KEYWORDS, "Write a trace file (runs now)."},
    {"merge_partials", tv_merge_partials, METH_O,
     "Merge aggregate partials with this aggregation."},
    {"materialize_partials", DFTU_PYCFUNCTION(tv_materialize_partials),
     METH_VARARGS | METH_KEYWORDS, "Write the rollup from partials."},
    {"reconstruct_if_cached", tv_reconstruct_if_cached, METH_NOARGS,
     "The rollup as a DataFrame, or None on a miss."},
    {"mv_source", tv_mv_source, METH_NOARGS,
     "Materialized-view files that would serve this query."},
    {"materialize_dir", tv_materialize_dir, METH_NOARGS,
     "Create and return the shared materialized-view directory."},
    {"register_materialized", tv_register_materialized, METH_O,
     "Write the materialized-view manifest at `dir`."},
    {"compare", tv_compare, METH_O,
     "Plan comparing this aggregation against another viewer's events."},
    {"plugins", tv_plugins, METH_O,
     "Plan folding a Plugins set over the scan."},
    {nullptr, nullptr, 0, nullptr}};

PyMethodDef module_methods[] = {
    {"merge_flamegraph_partials", merge_flamegraph_partials_py, METH_O,
     "merge_flamegraph_partials(partials) -> node DataFrame (no scan)."},
    {"plugin_results", plugin_results_py, METH_O,
     "plugin_results(plugins) -> {name: result} of its last run."},
    {nullptr, nullptr, 0, nullptr}};

}  // namespace

PyTypeObject TraceViewerType = [] {
    PyTypeObject t{PyVarObject_HEAD_INIT(nullptr, 0)};
    t.tp_name = "dftracer_utils_ext._TraceViewer";
    t.tp_basicsize = sizeof(TraceViewerObject);
    t.tp_dealloc = reinterpret_cast<destructor>(tv_dealloc);
    t.tp_flags = Py_TPFLAGS_DEFAULT;
    t.tp_doc = "A trace scan and the plan over it.";
    t.tp_methods = tv_methods;
    t.tp_init = reinterpret_cast<initproc>(tv_init);
    t.tp_new = tv_new;
    return t;
}();

int dftracer::utils::python::init_trace_viewer(PyObject* m) {
    if (register_type(m, &TraceViewerType, "_TraceViewer") < 0) return -1;
    return PyModule_AddFunctions(m, module_methods);
}
