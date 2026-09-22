#include <dftracer/utils/core/common/config.h>  // DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/plugin_host.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/trace_viewer_detail.h>
#include <dftracer/utils/trace/views/result_join.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::python::trace_viewer_detail {

namespace {

// A file-backed NDJSON sink for a session export branch; closes its file on
// destruction.
class SessionFileSink : public dftracer::utils::trace::views::ExportSink {
   public:
    explicit SessionFileSink(FILE* f) : f_(f) {}
    ~SessionFileSink() override {
        if (f_) std::fclose(f_);
    }
    void write(std::string_view data) override {
        if (f_) std::fwrite(data.data(), 1, data.size(), f_);
    }

   private:
    FILE* f_;
};

}  // namespace

// One shared scan driving several ViewSession branches. `branches` is a list of
// (kind:str, viewer:_TraceViewer, sink:str|None) tuples; each viewer carries
// the branch's full plan. Returns (results, stats): results come back in
// branch order, stats is the shared scan's own ExportStats (events/chunks
// counters, including chunks_skipped when a base prune applied). The base
// viewer's files/phase/time settings scope the shared scan.
PyObject* tv_session_run(TraceViewerObject* self, PyObject* branches) {
#ifndef DFTRACER_UTILS_ENABLE_ARROW
    PyErr_SetString(PyExc_RuntimeError,
                    "session() requires the arrow-enabled build");
    return nullptr;
#else
    if (!PyList_Check(branches)) {
        PyErr_SetString(PyExc_TypeError, "session branches must be a list");
        return nullptr;
    }
    const Py_ssize_t nb = PyList_Size(branches);

    enum class Kind {
        Collect,
        Materialize,
        Export,
        Events,
        Plugin,
        Partial,
        Join,
        Compare,
        CallTree,
        Flamegraph,
        Containment
    };
    struct BranchData {
        Kind kind = Kind::Collect;
        std::vector<std::string> files;
        std::string index_dir;
        ViewerPlan plan;
        std::string sink;
        // Plugin branch only: the built set attached to the session, the
        // registry its folds fill, and the Python object the results are read
        // back from after execute.
        const dftracer::utils::plugins::Plugins* plugins = nullptr;
        dftracer::utils::plugins::NamedResultRegistry* plugin_results = nullptr;
        PyObject* host_obj = nullptr;  // borrowed; the branches list holds it
        // Combine branch (Join/Compare) only: the two source branch indices and
        // the join type; n_key is inferred natively from the source branch.
        Py_ssize_t left_idx = -1;
        Py_ssize_t right_idx = -1;
        dftracer::utils::trace::views::JoinType how =
            dftracer::utils::trace::views::JoinType::INNER;
    };
    std::vector<BranchData> bdata(nb);

    // Snapshot each branch viewer's plan while the GIL is held.
    for (Py_ssize_t i = 0; i < nb; ++i) {
        PyObject* t = PyList_GetItem(branches, i);
        if (!PyTuple_Check(t) || PyTuple_Size(t) != 3) {
            PyErr_SetString(
                PyExc_TypeError,
                "each branch must be a 3-tuple (kind, viewer, sink)");
            return nullptr;
        }
        const char* kind = as_utf8(PyTuple_GetItem(t, 0));
        if (!kind) return nullptr;
        PyObject* vobj = PyTuple_GetItem(t, 1);
        PyObject* sk = PyTuple_GetItem(t, 2);

        const std::string k(kind);
        if (k == "collect")
            bdata[i].kind = Kind::Collect;
        else if (k == "materialize")
            bdata[i].kind = Kind::Materialize;
        else if (k == "export")
            bdata[i].kind = Kind::Export;
        else if (k == "events")
            bdata[i].kind = Kind::Events;
        else if (k == "partial")
            bdata[i].kind = Kind::Partial;
        else if (k == "plugin")
            bdata[i].kind = Kind::Plugin;
        else if (k == "join")
            bdata[i].kind = Kind::Join;
        else if (k == "compare")
            bdata[i].kind = Kind::Compare;
        else if (k == "call_tree")
            bdata[i].kind = Kind::CallTree;
        else if (k == "flamegraph")
            bdata[i].kind = Kind::Flamegraph;
        else if (k == "containment")
            bdata[i].kind = Kind::Containment;
        else {
            PyErr_Format(PyExc_ValueError, "unknown session branch kind: %s",
                         kind);
            return nullptr;
        }

        if (bdata[i].kind == Kind::Join || bdata[i].kind == Kind::Compare) {
            // vobj is (left_idx, right_idx[, how]); both indices must refer to
            // earlier collect branches. n_key is inferred natively.
            if (!PyTuple_Check(vobj) || PyTuple_Size(vobj) < 2) {
                PyErr_SetString(PyExc_TypeError,
                                "a join/compare branch needs (left, right[, "
                                "how]) branch indices");
                return nullptr;
            }
            bdata[i].left_idx = PyLong_AsSsize_t(PyTuple_GetItem(vobj, 0));
            bdata[i].right_idx = PyLong_AsSsize_t(PyTuple_GetItem(vobj, 1));
            if (PyErr_Occurred()) return nullptr;
            if (bdata[i].left_idx < 0 || bdata[i].left_idx >= i ||
                bdata[i].right_idx < 0 || bdata[i].right_idx >= i) {
                PyErr_SetString(PyExc_ValueError,
                                "join/compare indices must refer to earlier "
                                "branches");
                return nullptr;
            }
            if (bdata[i].kind == Kind::Join && PyTuple_Size(vobj) >= 3) {
                const char* h = as_utf8(PyTuple_GetItem(vobj, 2));
                if (!h) return nullptr;
                if (!parse_join_type(h, &bdata[i].how)) {
                    PyErr_Format(PyExc_ValueError, "unknown join type: %s", h);
                    return nullptr;
                }
            }
            continue;
        }

        if (bdata[i].kind == Kind::Plugin) {
            if (!PyObject_TypeCheck(vobj, &PluginHostType)) {
                PyErr_SetString(PyExc_TypeError,
                                "a plugin branch needs a Plugins instance");
                return nullptr;
            }
            bdata[i].host_obj = vobj;  // borrowed; branches list holds it
            bdata[i].plugins =
                dftracer::utils::python::plugin_host_plugins(vobj);
            if (!bdata[i].plugins) return nullptr;
            bdata[i].plugin_results =
                dftracer::utils::python::plugin_host_results(vobj);
            continue;
        }

        if (!PyObject_TypeCheck(vobj, &TraceViewerType)) {
            PyErr_SetString(PyExc_TypeError,
                            "session branch viewer must be a TraceViewer");
            return nullptr;
        }
        auto* v = (TraceViewerObject*)vobj;
        bdata[i].files = extract_files(v);
        bdata[i].index_dir = extract_index_dir(v);
        bdata[i].plan = *plan_of(v);
        if (sk != Py_None) {
            const char* s = as_utf8(sk);
            if (!s) return nullptr;
            bdata[i].sink = s;
        }
        if (bdata[i].kind == Kind::Export && bdata[i].sink.empty()) {
            PyErr_SetString(PyExc_ValueError,
                            "an export branch needs a sink path");
            return nullptr;
        }
    }

    Runtime* rt = resolve_runtime(self);
    auto base_files = extract_files(self);
    auto base_index = extract_index_dir(self);
    ViewerPlan base_plan = *plan_of(self);

    // AND-combine a branch's DSL filters into one export predicate ("" = all).
    auto combine_filters = [](const std::vector<std::string>& fs) {
        std::string out;
        for (std::size_t i = 0; i < fs.size(); ++i) {
            if (i) out += " and ";
            out += "(" + fs[i] + ")";
        }
        return out;
    };

    std::vector<DataFrame> results(nb);
    std::vector<DataFrame> results2(nb);  // containment's second (flamegraph)
    std::vector<ExportStats> export_stats(nb);
    std::vector<std::string> partial_results(nb);
    ExportStats session_stats{};
    std::string err;
    if (!run_blocking([&] {
            View base = build_view_from_data(base_files, base_index, base_plan,
                                             /*aggregate=*/false);
            ViewSession sess = base.session();
            std::vector<Deferred<dftracer::utils::plugins::PluginRun> >
                plugin_handles(nb);
            std::vector<Deferred<DataFrame> > agg_handles(nb);
            std::vector<Deferred<DataFrame> > agg_handles2(nb);
            std::vector<Deferred<ExportStats> > exp_handles(nb);
            std::vector<Deferred<std::string> > partial_handles(nb);
            std::vector<std::unique_ptr<SessionFileSink> > sinks;
            for (Py_ssize_t i = 0; i < nb; ++i) {
                switch (bdata[i].kind) {
                    case Kind::Collect: {
                        View bview = build_view_from_data(
                            bdata[i].files, bdata[i].index_dir, bdata[i].plan,
                            /*aggregate=*/true);
                        agg_handles[i] = sess.collect(bview);
                        break;
                    }
                    case Kind::Events: {
                        View bview = build_view_from_data(
                            bdata[i].files, bdata[i].index_dir, bdata[i].plan,
                            /*aggregate=*/true);
                        agg_handles[i] = sess.collect_events(bview);
                        break;
                    }
                    case Kind::CallTree:
                    case Kind::Flamegraph:
                    case Kind::Containment: {
                        View bview = build_view_from_data(
                            bdata[i].files, bdata[i].index_dir, bdata[i].plan,
                            /*aggregate=*/true);
                        std::vector<std::string> partition;
                        std::string ts, dur, nm;
                        parse_containment_cfg(bdata[i].sink, partition, ts, dur,
                                              nm);
                        if (bdata[i].kind == Kind::Containment) {
                            dftracer::utils::trace::views::ContainmentHandles
                                ch = sess.containment(bview, partition, ts, dur,
                                                      nm);
                            agg_handles[i] = ch.call_tree;
                            agg_handles2[i] = ch.flamegraph;
                        } else if (bdata[i].kind == Kind::Flamegraph) {
                            agg_handles[i] =
                                sess.flamegraph(bview, partition, ts, dur, nm);
                        } else {
                            agg_handles[i] =
                                sess.call_tree(bview, partition, ts, dur, nm);
                        }
                        break;
                    }
                    case Kind::Partial: {
                        View bview = build_view_from_data(
                            bdata[i].files, bdata[i].index_dir, bdata[i].plan,
                            /*aggregate=*/true);
                        partial_handles[i] = sess.aggregate_partial(bview);
                        break;
                    }
                    case Kind::Materialize:
                        sess.materialize(bdata[i].plan.group_by,
                                         bdata[i].plan.agg);
                        break;
                    case Kind::Export: {
                        std::optional<Query> q;
                        const std::string f =
                            combine_filters(bdata[i].plan.filters);
                        if (!f.empty()) {
                            auto parsed = Query::from_string(f);
                            if (!parsed) {
                                err = "invalid filter query: " + f;
                                return;
                            }
                            q = std::move(parsed.value());
                        }
                        FILE* fp = std::fopen(bdata[i].sink.c_str(), "wb");
                        if (!fp) {
                            err = "cannot open export sink: " + bdata[i].sink;
                            return;
                        }
                        sinks.push_back(std::make_unique<SessionFileSink>(fp));
                        exp_handles[i] =
                            q ? sess.export_json(std::move(*q), *sinks.back())
                              : sess.export_json(*sinks.back());
                        break;
                    }
                    case Kind::Plugin:
                        // C++-only, safe with the GIL released; named results
                        // are read back from the handle after execute.
                        plugin_handles[i] = bdata[i].plugins->attach(sess);
                        break;
                    case Kind::Join:
                        agg_handles[i] = sess.join(
                            agg_handles[bdata[i].left_idx],
                            agg_handles[bdata[i].right_idx], bdata[i].how);
                        break;
                    case Kind::Compare:
                        agg_handles[i] =
                            sess.compare(agg_handles[bdata[i].left_idx],
                                         agg_handles[bdata[i].right_idx]);
                        break;
                }
            }
            session_stats = rt->submit(sess.execute()).get();
            for (Py_ssize_t i = 0; i < nb; ++i) {
                if (bdata[i].kind == Kind::Collect ||
                    bdata[i].kind == Kind::Events ||
                    bdata[i].kind == Kind::Join ||
                    bdata[i].kind == Kind::Compare ||
                    bdata[i].kind == Kind::CallTree ||
                    bdata[i].kind == Kind::Flamegraph)
                    results[i] = std::move(agg_handles[i].get());
                else if (bdata[i].kind == Kind::Containment) {
                    results[i] = std::move(agg_handles[i].get());
                    results2[i] = std::move(agg_handles2[i].get());
                } else if (bdata[i].kind == Kind::Export)
                    export_stats[i] = exp_handles[i].get();
                else if (bdata[i].kind == Kind::Partial)
                    partial_results[i] = std::move(partial_handles[i].get());
                else if (bdata[i].kind == Kind::Plugin)
                    *bdata[i].plugin_results =
                        std::move(plugin_handles[i]->results);
            }
        }))
        return nullptr;
    if (!err.empty()) {
        PyErr_SetString(PyExc_ValueError, err.c_str());
        return nullptr;
    }

    PyObject* out = PyList_New(nb);
    if (!out) return nullptr;
    for (Py_ssize_t i = 0; i < nb; ++i) {
        PyObject* item = nullptr;
        if (bdata[i].kind == Kind::Collect || bdata[i].kind == Kind::Events ||
            bdata[i].kind == Kind::Join || bdata[i].kind == Kind::Compare ||
            bdata[i].kind == Kind::CallTree ||
            bdata[i].kind == Kind::Flamegraph) {
            item =
                dftracer::utils::python::wrap_dataframe(std::move(results[i]));
        } else if (bdata[i].kind == Kind::Containment) {
            PyObject* ct =
                dftracer::utils::python::wrap_dataframe(std::move(results[i]));
            if (!ct) {
                Py_DECREF(out);
                return nullptr;
            }
            PyObject* fg =
                dftracer::utils::python::wrap_dataframe(std::move(results2[i]));
            if (!fg) {
                Py_DECREF(ct);
                Py_DECREF(out);
                return nullptr;
            }
            item = PyTuple_Pack(2, ct, fg);
            Py_DECREF(ct);
            Py_DECREF(fg);
        } else if (bdata[i].kind == Kind::Export) {
            const ExportStats& st = export_stats[i];
            item = Py_BuildValue(
                "{s:K,s:K,s:K}", "events_matched",
                (unsigned long long)st.events_matched, "events_scanned",
                (unsigned long long)st.events_scanned, "chunks_scanned",
                (unsigned long long)st.chunks_scanned);
        } else if (bdata[i].kind == Kind::Partial) {
            item = PyBytes_FromStringAndSize(
                partial_results[i].data(),
                (Py_ssize_t)partial_results[i].size());
        } else if (bdata[i].kind == Kind::Plugin) {
            // {name: pyarrow|bytes}; the Python Session shapes it to DataFrame.
            item = dftracer::utils::python::plugin_host_results_dict(
                bdata[i].host_obj);
        } else {
            Py_INCREF(Py_None);
            item = Py_None;
        }
        if (!item) {
            Py_DECREF(out);
            return nullptr;
        }
        PyList_SET_ITEM(out, i, item);
    }
    PyObject* stats_dict = Py_BuildValue(
        "{s:K,s:K,s:K,s:K}", "events_matched",
        (unsigned long long)session_stats.events_matched, "events_scanned",
        (unsigned long long)session_stats.events_scanned, "chunks_scanned",
        (unsigned long long)session_stats.chunks_scanned, "chunks_skipped",
        (unsigned long long)session_stats.chunks_skipped);
    if (!stats_dict) {
        Py_DECREF(out);
        return nullptr;
    }
    PyObject* ret = PyTuple_Pack(2, out, stats_dict);
    Py_DECREF(out);
    Py_DECREF(stats_dict);
    return ret;
#endif
}

}  // namespace dftracer::utils::python::trace_viewer_detail
