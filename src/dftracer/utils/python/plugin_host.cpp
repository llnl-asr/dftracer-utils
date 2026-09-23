#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/plugins/config.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/plugins/result_registry.h>
#include <dftracer/utils/python/plugin_host.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_seq_helpers.h>
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/trace/indexing/resolve_and_build.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
// Unconditional: the native-frame result path (OwnedDataFrame) needs the
// DataFrame wrapper regardless of the Arrow build option.
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/lazyframe_handle.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/lazyframe.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/arrow_helpers.h>
#include <dftracer/utils/utilities/common/arrow/gapfill.h>
#include <dftracer/utils/utilities/common/arrow/join.h>
#include <dftracer/utils/utilities/common/arrow/window.h>
#include <nanoarrow/nanoarrow.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using dftracer::utils::plugins::ConfigTree;
using dftracer::utils::plugins::NamedResult;
using dftracer::utils::plugins::NamedResultRegistry;
using dftracer::utils::plugins::OwnedArrow;
using dftracer::utils::plugins::OwnedDataFrame;
using dftracer::utils::plugins::OwnedLazyFrame;
using dftracer::utils::plugins::PluginRun;
using dftracer::utils::plugins::Plugins;
using dftracer::utils::python::parse_seq;
using dftracer::utils::python::parse_string_seq;
namespace indexing = dftracer::utils::trace::indexing;
namespace internal = dftracer::utils::trace::internal;
namespace views = dftracer::utils::trace::views;
namespace filesystem = dftracer::utils::utilities::filesystem;

}  // namespace

namespace dftracer::utils::python {

// A host object's mutable side: the built set (immutable once constructed) and
// the named results of the most recent run or session.
struct PluginHostState {
    std::optional<Plugins> set;
    NamedResultRegistry results;
    // wire id -> attribute name, so a caller indexes results[...] with the
    // plugin's declared name rather than its package-qualified wire identity.
    std::unordered_map<std::string, std::string> result_names;
};

}  // namespace dftracer::utils::python

namespace {

using dftracer::utils::python::PluginHostState;

PluginHostState* state_of(PluginHostObject* self) {
    return static_cast<PluginHostState*>(self->host_ptr);
}

// The set built at construction. NULL with a Python error set if construction
// did not leave a built set (should not happen: __init__ raises on failure).
const Plugins* built_set(PluginHostObject* self) {
    PluginHostState* st = state_of(self);
    if (!st->set) {
        PyErr_SetString(PyExc_RuntimeError, "Plugins set was not built");
        return nullptr;
    }
    return &*st->set;
}

// Expand any directory in `inputs` to its .pfw/.pfw.gz files, index (unless
// disabled), then drive every loaded plugin as a fold over one fused scan.
CoroTask<void> run_host_scan(CoroScope& scope, std::vector<std::string> inputs,
                             std::string index_dir, const Plugins* plugins,
                             bool auto_index, PluginRun* out) {
    std::vector<std::string> files;
    for (const auto& p : inputs) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            filesystem::PatternDirectoryScannerUtilityInput in(
                p, {".pfw", ".pfw.gz"}, /*recursive=*/true,
                /*populate_size=*/false);
            filesystem::PatternDirectoryScannerUtility scanner;
            auto entries = co_await scanner(scope, in);
            for (auto& e : entries) files.push_back(e.path.string());
        } else {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty())
        throw dftracer::utils::DFTUtilsException(
            dftracer::utils::ErrorCode::NOT_FOUND,
            "no .pfw or .pfw.gz trace files found");

    if (auto_index) {
        auto norm = co_await indexing::normalize_members_for_ingest(files, 0);
        files = std::move(norm.files);
        co_await indexing::ensure_indexes_fresh(&scope, "", files, index_dir);
    }

    std::vector<views::ViewFile> view_files;
    view_files.reserve(files.size());
    for (const auto& f : files) {
        views::ViewFile vf;
        vf.file_path = f;
        vf.index_path = internal::determine_index_path(f, index_dir);
        view_files.push_back(std::move(vf));
    }
    views::View view = views::View::from_files(std::move(view_files));
    auto run = co_await plugins->run(view);
    if (!run)
        throw dftracer::utils::DFTUtilsException(run.error().code,
                                                 run.error().message);
    *out = std::move(*run);
}

bool collect_inputs(PyObject* traces, std::vector<std::string>& out) {
    if (PyUnicode_Check(traces)) {
        const char* s = PyUnicode_AsUTF8(traces);
        if (!s) return false;
        out.emplace_back(s);
        return true;
    }
    return parse_string_seq(traces, "traces must be a str or list[str]", out);
}

// Convert one emitted result to Python: an opaque blob to bytes, a single
// user-schema Arrow array to a pyarrow-compatible table, and a streamed
// multi-batch map result to a pull-based pyarrow.RecordBatchReader. Returns a
// new reference, or nullptr with a Python error set.
PyObject* result_to_py(NamedResult& result) {
    if (auto* blob = std::get_if<std::vector<std::byte>>(&result))
        return PyBytes_FromStringAndSize(
            reinterpret_cast<const char*>(blob->data()),
            static_cast<Py_ssize_t>(blob->size()));
    if (auto* frame = std::get_if<OwnedDataFrame>(&result)) {
        // Adopt the handle's columns into a native DataFrame (shared buffers,
        // zero-copy) and hand it back as a _DataFrame; the OwnedDataFrame frees
        // the emptied handle.
        namespace df_ns = dftracer::utils::dataframe;
        dftu_dataframe* h = frame->handle;
        df_ns::DataFrame df;
        std::int32_t n = dftu_dataframe_num_columns(h);
        df.names.reserve(static_cast<std::size_t>(n));
        df.columns.reserve(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i) {
            const char* nm = dftu_dataframe_column_name(h, i);
            df.names.emplace_back(nm ? nm : "");
            df.columns.emplace_back(
                df_ns::Series{dftu_dataframe_column(h, nm)});
        }
        return dftracer::utils::python::wrap_dataframe(std::move(df));
    }
    if (auto* lazy = std::get_if<OwnedLazyFrame>(&result)) {
        // Move the plan out into the native Python LazyFrame wrapper; the
        // OwnedLazyFrame frees the emptied handle. The plan must be
        // self-contained (see the emit_lazyframe contract).
        namespace df_ns = dftracer::utils::dataframe;
        df_ns::LazyFrame lf =
            std::move(df_ns::lazyframe_handle_unwrap(lazy->handle));
        return dftracer::utils::python::wrap_lazyframe(std::move(lf));
    }
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    namespace arr = dftracer::utils::utilities::common::arrow;
    auto* owned = std::get_if<OwnedArrow>(&result);
    nanoarrow::UniqueSchema schema;
    nanoarrow::UniqueArray array;
    ArrowSchemaMove(&owned->schema, schema.get());
    ArrowArrayMove(&owned->array, array.get());
    return dftracer::utils::python::arrow_result_to_table(
        arr::ArrowExportResult(std::move(schema), std::move(array)));
#else
    PyErr_SetString(PyExc_RuntimeError,
                    "plugin emitted an Arrow result but the extension was "
                    "built without Arrow support");
    return nullptr;
#endif
}

PyObject* results_to_dict(PluginHostState* st) {
    PyObject* d = PyDict_New();
    if (!d) return nullptr;
    for (auto& [wire_name, result] : st->results.results()) {
        PyObject* val = result_to_py(result);
        if (!val) {
            Py_DECREF(d);
            return nullptr;
        }
        auto it = st->result_names.find(wire_name);
        const std::string& key =
            it != st->result_names.end() ? it->second : wire_name;
        if (PyDict_SetItemString(d, key.c_str(), val) < 0) {
            Py_DECREF(val);
            Py_DECREF(d);
            return nullptr;
        }
        Py_DECREF(val);
    }
    return d;
}

PyObject* ph_new(PyTypeObject* type, PyObject*, PyObject*) {
    PluginHostObject* self = (PluginHostObject*)type->tp_alloc(type, 0);
    if (!self) return nullptr;
    self->runtime_obj = nullptr;
    self->host_ptr = new PluginHostState();
    return (PyObject*)self;
}

void ph_dealloc(PluginHostObject* self) {
    delete state_of(self);
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

bool parse_result_names(PyObject* obj,
                        std::unordered_map<std::string, std::string>& out) {
    if (!obj || obj == Py_None) return true;
    if (!PyDict_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "result_names must be a dict or None");
        return false;
    }
    PyObject *key = nullptr, *value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(obj, &pos, &key, &value)) {
        const char* k = as_utf8(key);
        const char* v = as_utf8(value);
        if (!k || !v) {
            PyErr_SetString(PyExc_TypeError,
                            "result_names keys and values must be str");
            return false;
        }
        out.emplace(k, v);
    }
    return true;
}

// __init__(specs, result_names=None, runtime=None): specs is a sequence of
// (path, config_json_or_None) pairs; config_json is a JSON object string (the
// Python wrapper serializes a dict). Builds the set now - dlopen, the ABI
// gate, and capability resolution all run here, so a load/symbol/ABI/
// capability failure raises ImportError from the constructor.
int ph_init(PluginHostObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"specs", "result_names", "runtime", nullptr};
    PyObject* specs = nullptr;
    PyObject* result_names_obj = nullptr;
    PyObject* runtime_arg = nullptr;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|OO",
                                     const_cast<char**>(kwlist), &specs,
                                     &result_names_obj, &runtime_arg))
        return -1;
    if (bind_runtime_arg(self, runtime_arg) != 0) return -1;

    PluginHostState* st = state_of(self);
    if (!parse_result_names(result_names_obj, st->result_names)) return -1;

    Plugins::Builder builder;
    PyObject* iter = PyObject_GetIter(specs);
    if (!iter) return -1;
    PyObject* item;
    while ((item = PyIter_Next(iter)) != nullptr) {
        const char* path = nullptr;
        const char* config_json = nullptr;
        bool ok = PyArg_ParseTuple(item, "s|z", &path, &config_json) != 0;
        if (ok) {
            try {
                if (config_json)
                    builder.add(path,
                                ConfigTree::from_json_string(config_json));
                else
                    builder.add(path);
            } catch (const std::exception& e) {
                PyErr_SetString(dftracer::utils::python::g_dft_value_error
                                    ? dftracer::utils::python::g_dft_value_error
                                    : PyExc_ValueError,
                                e.what());
                ok = false;
            }
        }
        Py_DECREF(item);
        if (!ok) {
            Py_DECREF(iter);
            return -1;
        }
    }
    Py_DECREF(iter);
    if (PyErr_Occurred()) return -1;

    auto built = builder.build();
    if (!built) {
        PyErr_SetString(PyExc_ImportError, built.error().message.c_str());
        return -1;
    }
    st->set.emplace(std::move(*built));
    return 0;
}

// run(traces, index_dir=None, auto_index=True) -> (results, stats). Drives the
// fused scan with the GIL released.
PyObject* ph_run(PluginHostObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"traces", "index_dir", "auto_index",
                                   nullptr};
    PyObject* traces = nullptr;
    const char* index_dir = nullptr;
    int auto_index = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|zp",
                                     const_cast<char**>(kwlist), &traces,
                                     &index_dir, &auto_index))
        return nullptr;

    std::vector<std::string> inputs;
    if (!collect_inputs(traces, inputs)) return nullptr;

    Runtime* rt = resolve_runtime(self);
    const Plugins* plugins = built_set(self);
    if (!plugins) return nullptr;
    std::string index_dir_s = index_dir ? index_dir : "";
    PluginRun run;
    if (!run_blocking([&] {
            rt->submit(
                  dftracer::utils::run_coro_scope(
                      rt->executor(), run_host_scan, std::move(inputs),
                      std::move(index_dir_s), plugins, auto_index != 0, &run),
                  "plugin-host-run")
                .get();
        }))
        return nullptr;

    PyObject* stats_dict = PyDict_New();
    if (!stats_dict) return nullptr;
    dict_set_i64(stats_dict, "events_scanned",
                 (long long)run.stats.events_scanned);
    dict_set_i64(stats_dict, "events_matched",
                 (long long)run.stats.events_matched);

    PluginHostState* st = state_of(self);
    st->results = std::move(run.results);
    PyObject* results_dict = results_to_dict(st);
    if (!results_dict) {
        Py_DECREF(stats_dict);
        return nullptr;
    }
    PyObject* out = PyTuple_Pack(2, results_dict, stats_dict);
    Py_DECREF(results_dict);
    Py_DECREF(stats_dict);
    return out;
}

PyMethodDef ph_methods[] = {
    {"run", DFTU_PYCFUNCTION(ph_run), METH_VARARGS | METH_KEYWORDS,
     "run(traces, index_dir=None, auto_index=True) -> (dict, dict): fold every "
     "plugin over one fused scan. Returns ({name: bytes | pyarrow table}, "
     "{events_scanned, events_matched})."},
    {nullptr, nullptr, 0, nullptr}};

}  // namespace

PyTypeObject PluginHostType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "dftracer_utils_ext.Plugins",
    sizeof(PluginHostObject),
    0,
    (destructor)ph_dealloc,
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
    "Load and run compiled DFTracer plugins over trace files.",
    0,
    0,
    0,
    0,
    0,
    0,
    ph_methods,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    (initproc)ph_init,
    0,
    ph_new,
};

int dftracer::utils::python::init_plugin_host(PyObject* m) {
    return register_type(m, &PluginHostType, "Plugins");
}

PyObject* dftracer::utils::python::plugin_host_results_dict(PyObject* host) {
    if (!PyObject_TypeCheck(host, &PluginHostType)) {
        PyErr_SetString(PyExc_TypeError, "expected a Plugins instance");
        return nullptr;
    }
    return results_to_dict(state_of((PluginHostObject*)host));
}

const dftracer::utils::plugins::Plugins*
dftracer::utils::python::plugin_host_plugins(PyObject* host) {
    if (!PyObject_TypeCheck(host, &PluginHostType)) {
        PyErr_SetString(PyExc_TypeError, "expected a Plugins instance");
        return nullptr;
    }
    return built_set((PluginHostObject*)host);
}

dftracer::utils::plugins::NamedResultRegistry*
dftracer::utils::python::plugin_host_results(PyObject* host) {
    if (!PyObject_TypeCheck(host, &PluginHostType)) {
        PyErr_SetString(PyExc_TypeError, "expected a Plugins instance");
        return nullptr;
    }
    return &state_of((PluginHostObject*)host)->results;
}
