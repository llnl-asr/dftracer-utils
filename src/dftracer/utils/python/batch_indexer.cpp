#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/hash/hash_combine.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_serialization.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregator_types.h>
#include <dftracer/utils/index/schemas/dft/agg/event_aggregator.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_serialization.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/python/batch_indexer.h>
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/indexer.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_list_helpers.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_seq_helpers.h>
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::index::schemas::dft::agg;

// ---------------------------------------------------------------------------
// BatchIndexer - directory-level indexer with resolve/build pattern
// ---------------------------------------------------------------------------

static void Indexer_dealloc(IndexerObject* self) {
    Py_XDECREF(self->runtime_obj);
    Py_XDECREF(self->directory);
    Py_XDECREF(self->files);
    Py_XDECREF(self->index_dir);
    Py_XDECREF(self->group_keys);
    Py_XDECREF(self->custom_metric_fields);
    Py_XDECREF(self->bloom_fields);
    Py_XDECREF(self->extensions);
    Py_XDECREF(self->schema);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* Indexer_new(PyTypeObject* type, PyObject*, PyObject*) {
    IndexerObject* self = (IndexerObject*)type->tp_alloc(type, 0);
    if (self) {
        self->runtime_obj = nullptr;
        self->directory = nullptr;
        self->files = nullptr;
        self->index_dir = nullptr;
        self->require_checkpoint = 1;
        self->require_bloom = 1;
        self->build_bloom = 1;
        self->require_aggregation = 0;
        self->bloom_fields = nullptr;
        self->extensions = nullptr;
        self->memory_budget = 0;
        self->schema = nullptr;
        self->false_positive_rate = ChunkIndexerConfig{}.false_positive_rate;
        self->expected_entries =
            ChunkIndexerConfig{}.expected_entries_per_chunk;
        self->path_budget = ChunkIndexerConfig{}.path_budget;
        self->stats_share = ChunkIndexerConfig{}.stats_share;
        self->auto_max_distinct = ChunkIndexerConfig{}.auto_max_distinct;
        self->time_interval_ms = 5000.0;
        self->group_keys = nullptr;
        self->custom_metric_fields = nullptr;
        self->compute_percentiles = 0;
        self->group_by_file = 1;
        self->checkpoint_size =
            dftracer::utils::constants::indexer::DEFAULT_CHECKPOINT_SIZE;
        self->parallelism = 0;
        self->force_rebuild = 0;
    }
    return (PyObject*)self;
}

static int Indexer_init(IndexerObject* self, PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {"directory",
                                   "files",
                                   "index_dir",
                                   "require_checkpoint",
                                   "require_bloom",
                                   "build_bloom",
                                   "require_aggregation",
                                   "time_interval_ms",
                                   "group_keys",
                                   "custom_metric_fields",
                                   "compute_percentiles",
                                   "group_by_file",
                                   "checkpoint_size",
                                   "parallelism",
                                   "force_rebuild",
                                   "runtime",
                                   "bloom_fields",
                                   "false_positive_rate",
                                   "expected_entries",
                                   "path_budget",
                                   "stats_share",
                                   "auto_max_distinct",
                                   "extensions",
                                   "memory_budget",
                                   "schema",
                                   nullptr};

    const char* directory = "";
    PyObject* files_obj = Py_None;
    const char* index_dir = "";
    int require_checkpoint = 1;
    int require_bloom = 1;
    int build_bloom = 1;
    int require_aggregation = 0;
    double time_interval_ms = 5000.0;
    PyObject* group_keys_obj = Py_None;
    PyObject* custom_metrics_obj = Py_None;
    int compute_percentiles = 0;
    int group_by_file = 1;
    Py_ssize_t checkpoint_size = static_cast<Py_ssize_t>(
        dftracer::utils::constants::indexer::DEFAULT_CHECKPOINT_SIZE);
    Py_ssize_t parallelism = 0;
    int force_rebuild = 0;
    PyObject* runtime_arg = nullptr;
    PyObject* bloom_fields_obj = Py_None;
    double false_positive_rate = self->false_positive_rate;
    Py_ssize_t expected_entries =
        static_cast<Py_ssize_t>(self->expected_entries);
    Py_ssize_t path_budget = static_cast<Py_ssize_t>(self->path_budget);
    double stats_share = self->stats_share;
    Py_ssize_t auto_max_distinct =
        static_cast<Py_ssize_t>(self->auto_max_distinct);
    PyObject* extensions_obj = Py_None;
    unsigned long long memory_budget = 0;
    PyObject* schema_obj = Py_None;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|sOsppppdOOppnnpOOdnndnOKO",
            const_cast<char**>(kwlist), &directory, &files_obj, &index_dir,
            &require_checkpoint, &require_bloom, &build_bloom,
            &require_aggregation, &time_interval_ms, &group_keys_obj,
            &custom_metrics_obj, &compute_percentiles, &group_by_file,
            &checkpoint_size, &parallelism, &force_rebuild, &runtime_arg,
            &bloom_fields_obj, &false_positive_rate, &expected_entries,
            &path_budget, &stats_share, &auto_max_distinct, &extensions_obj,
            &memory_budget, &schema_obj)) {
        return -1;
    }
    if (!(false_positive_rate > 0.0 && false_positive_rate < 1.0)) {
        PyErr_SetString(PyExc_ValueError,
                        "false_positive_rate must be in (0, 1)");
        return -1;
    }
    if (expected_entries <= 0) {
        PyErr_SetString(PyExc_ValueError, "expected_entries must be > 0");
        return -1;
    }
    if (auto_max_distinct <= 0) {
        PyErr_SetString(PyExc_ValueError, "auto_max_distinct must be > 0");
        return -1;
    }
    if (path_budget < 0) {
        PyErr_SetString(PyExc_ValueError, "path_budget must be >= 0");
        return -1;
    }
    if (!(stats_share > 0.0 && stats_share <= 1.0)) {
        PyErr_SetString(PyExc_ValueError, "stats_share must be in (0, 1]");
        return -1;
    }
    self->path_budget = static_cast<std::size_t>(path_budget);
    self->stats_share = stats_share;
    self->auto_max_distinct = static_cast<std::size_t>(auto_max_distinct);
    if (bloom_fields_obj != Py_None) {
        std::vector<std::string> check;
        if (!dftracer::utils::python::parse_string_seq(
                bloom_fields_obj, "bloom_fields must be a sequence of str",
                check))
            return -1;
        Py_INCREF(bloom_fields_obj);
        self->bloom_fields = bloom_fields_obj;
    }
    self->memory_budget = memory_budget;
    if (schema_obj != Py_None) {
        if (!PyUnicode_Check(schema_obj)) {
            PyErr_SetString(PyExc_TypeError, "schema must be a str or None");
            return -1;
        }
        Py_INCREF(schema_obj);
        Py_XSETREF(self->schema, schema_obj);
    }
    self->false_positive_rate = false_positive_rate;
    self->expected_entries = static_cast<std::size_t>(expected_entries);
    if (extensions_obj != Py_None) {
        std::vector<std::string> check;
        if (!dftracer::utils::python::parse_string_seq(
                extensions_obj, "extensions must be a sequence of str", check))
            return -1;
        Py_INCREF(extensions_obj);
        Py_XSETREF(self->extensions, extensions_obj);
    }

    // Validate: at least one of directory or files must be provided
    bool has_directory = directory && directory[0] != '\0';
    bool has_files = files_obj && files_obj != Py_None &&
                     PyList_Check(files_obj) && PyList_Size(files_obj) > 0;

    if (!has_directory && !has_files) {
        PyErr_SetString(PyExc_ValueError,
                        "At least one of 'directory' or 'files' must be "
                        "provided");
        return -1;
    }

    // Store runtime
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

    self->directory = PyUnicode_FromString(directory);
    self->index_dir = PyUnicode_FromString(index_dir);
    self->require_checkpoint = require_checkpoint;
    self->require_bloom = require_bloom;
    self->build_bloom = build_bloom;
    self->require_aggregation = require_aggregation;
    self->time_interval_ms = time_interval_ms;
    self->compute_percentiles = compute_percentiles;
    self->group_by_file = group_by_file;
    self->checkpoint_size = static_cast<std::size_t>(checkpoint_size);
    self->parallelism = static_cast<std::size_t>(parallelism);
    self->force_rebuild = force_rebuild;

    // Store files list
    if (has_files) {
        Py_INCREF(files_obj);
        self->files = files_obj;
    } else {
        self->files = nullptr;
    }

    // Store group_keys
    if (group_keys_obj && group_keys_obj != Py_None) {
        Py_INCREF(group_keys_obj);
        self->group_keys = group_keys_obj;
    } else {
        self->group_keys = nullptr;
    }

    // Store custom_metric_fields
    if (custom_metrics_obj && custom_metrics_obj != Py_None) {
        Py_INCREF(custom_metrics_obj);
        self->custom_metric_fields = custom_metrics_obj;
    } else {
        self->custom_metric_fields = nullptr;
    }

    return 0;
}

// The args fields the bloom tier covers, as the index names them. False with
// a Python error set when bloom_fields holds a non-str.
static bool requested_bloom_fields(IndexerObject* self,
                                   std::vector<std::string>& out) {
    if (!self->bloom_fields) {
        out.clear();
        return true;
    }
    out.clear();
    if (!dftracer::utils::python::parse_string_seq(
            self->bloom_fields, "bloom_fields must be a sequence of str", out))
        return false;
    for (std::string& f : out) f = extra_dimension_name(f);
    return true;
}

static Runtime* get_batch_indexer_runtime(IndexerObject* self) {
    if (self->runtime_obj) {
        return ((RuntimeObject*)self->runtime_obj)->runtime.get();
    }
    return dftracer::utils::python::get_default_runtime();
}

static std::optional<AggregationConfig> build_aggregation_config(
    IndexerObject* self) {
    if (!self->require_aggregation) {
        return std::nullopt;
    }

    AggregationConfig config;
    config.time_interval_us =
        static_cast<std::uint64_t>(self->time_interval_ms * 1000.0);

    if (self->group_keys && PyList_Check(self->group_keys)) {
        Py_ssize_t n = PyList_Size(self->group_keys);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char* s = as_utf8(PyList_GetItem(self->group_keys, i));
            if (s) config.extra_group_keys.emplace_back(s);
        }
    }
    if (self->custom_metric_fields &&
        PyList_Check(self->custom_metric_fields)) {
        Py_ssize_t n = PyList_Size(self->custom_metric_fields);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char* s =
                as_utf8(PyList_GetItem(self->custom_metric_fields, i));
            if (s) config.custom_metric_fields.emplace_back(s);
        }
    }

    config.compute_percentiles = self->compute_percentiles != 0;
    config.group_by_file = self->group_by_file != 0;
    return config;
}

static std::optional<dftracer::utils::index::IndexerOptions> indexer_options(
    IndexerObject* self) {
    dftracer::utils::index::IndexerOptions o;
    const char* index_dir = as_utf8(self->index_dir);
    o.index_dir = index_dir ? index_dir : "";
    o.checkpoint_size = self->checkpoint_size;
    o.parallelism = self->parallelism;
    o.checkpoints = self->require_checkpoint;
    o.aggregation = build_aggregation_config(self);
    o.runtime = get_batch_indexer_runtime(self);
    o.memory_budget = self->memory_budget;
    if (self->schema) {
        const char* p = as_utf8(self->schema);
        o.schema = p ? p : "";
    }
    if (!self->build_bloom) {
        o.bloom.reset();
        return o;
    }
    auto& b = *o.bloom;
    if (!requested_bloom_fields(self, b.fields)) return std::nullopt;
    b.required = self->require_bloom;
    b.false_positive_rate = self->false_positive_rate;
    b.path_budget = self->path_budget;
    b.stats_share = self->stats_share;
    b.auto_max_distinct = self->auto_max_distinct;
    b.expected_entries_per_chunk = self->expected_entries;
    if (self->extensions) o.extensions.clear();
    if (self->extensions &&
        !dftracer::utils::python::parse_string_seq(
            self->extensions, "extensions must be a sequence of str",
            o.extensions))
        return std::nullopt;
    return o;
}

static std::vector<std::string> indexer_paths(IndexerObject* self) {
    const char* directory = as_utf8(self->directory);
    if (directory && directory[0] != '\0') return {directory};
    std::vector<std::string> paths;
    if (self->files && PyList_Check(self->files)) {
        Py_ssize_t n = PyList_Size(self->files);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char* s = as_utf8(PyList_GetItem(self->files, i));
            if (s) paths.emplace_back(s);
        }
    }
    return paths;
}

// Runs `fn` on an Indexer over this object's paths and options, GIL released.
template <class Fn>
static bool with_indexer(IndexerObject* self, Fn fn) {
    auto options = indexer_options(self);
    if (!options) return false;
    auto paths = indexer_paths(self);
    return run_blocking([&] {
        fn(dftracer::utils::index::Indexer::open(std::move(paths),
                                                 std::move(*options)));
    });
}

enum class IndexerCall { STATUS, BUILD, REBUILD, ENSURE };

static bool run_indexer(IndexerObject* self, IndexerCall call,
                        dftracer::utils::index::IndexStatus& out) {
    auto options = indexer_options(self);
    if (!options) return false;
    auto paths = indexer_paths(self);
    const bool force = self->force_rebuild != 0;
    return run_blocking([&] {
        auto ix = dftracer::utils::index::Indexer::open(std::move(paths),
                                                        std::move(*options));
        switch (call) {
            case IndexerCall::STATUS:
                out = ix.status();
                break;
            case IndexerCall::BUILD:
                out = ix.build();
                break;
            case IndexerCall::REBUILD:
                out = ix.rebuild();
                break;
            case IndexerCall::ENSURE: {
                if (!force) {
                    out = ix.build();
                    break;
                }
                auto before = ix.status();
                out = before.needs_work.empty() &&
                              !before.aggregation_needs_rebuild
                          ? std::move(before)
                          : ix.rebuild();
                break;
            }
        }
    });
}

static PyObject* string_list(const std::vector<std::string>& items) {
    PyObject* list = PyList_New(static_cast<Py_ssize_t>(items.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < items.size(); ++i) {
        PyObject* s = PyUnicode_FromString(items[i].c_str());
        if (!s) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), s);
    }
    return list;
}

static PyObject* status_dict(const dftracer::utils::index::IndexStatus& st) {
    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;
    dict_set_steal(dict, "total_files", PyLong_FromSize_t(st.total));
    dict_set_steal(dict, "index_path",
                   PyUnicode_FromString(st.index_path.c_str()));
    dict_set_steal(dict, "aggregation_interval_us",
                   PyLong_FromUnsignedLongLong(st.aggregation_interval_us));
    dict_set_steal(dict, "needs_rebuild",
                   PyBool_FromLong(st.aggregation_needs_rebuild));
    dict_set_steal(dict, "ready", string_list(st.ready));
    dict_set_steal(dict, "needs_work", string_list(st.needs_work));
    dict_set_steal(dict, "truncated", string_list(st.truncated));
    return dict;
}

// ---------------------------------------------------------------------------
// resolve() - check what exists vs needs building
// ---------------------------------------------------------------------------

static PyObject* Indexer_resolve(IndexerObject* self,
                                 PyObject* Py_UNUSED(ignored)) {
    dftracer::utils::index::IndexStatus st;
    if (!run_indexer(self, IndexerCall::STATUS, st)) return nullptr;
    return status_dict(st);
}

// ---------------------------------------------------------------------------
// build() - build missing index tiers
// ---------------------------------------------------------------------------

static PyObject* Indexer_build(IndexerObject* self,
                               PyObject* Py_UNUSED(ignored)) {
    dftracer::utils::index::IndexStatus st;
    if (!run_indexer(
            self,
            self->force_rebuild ? IndexerCall::REBUILD : IndexerCall::BUILD,
            st))
        return nullptr;
    Py_RETURN_NONE;
}

// ---------------------------------------------------------------------------
// ensure_indexed() - resolve + build if needed
// ---------------------------------------------------------------------------

static PyObject* Indexer_ensure_indexed(IndexerObject* self,
                                        PyObject* Py_UNUSED(ignored)) {
    dftracer::utils::index::IndexStatus st;
    if (!run_indexer(self, IndexerCall::ENSURE, st)) return nullptr;
    return status_dict(st);
}

static PyObject* Indexer_manifest(IndexerObject* self,
                                  PyObject* Py_UNUSED(ignored)) {
    std::string json;
    if (!with_indexer(self, [&](dftracer::utils::index::Indexer ix) {
            json = dftracer::utils::index::to_json(ix.manifest());
        }))
        return nullptr;
    return PyUnicode_FromStringAndSize(json.data(),
                                       static_cast<Py_ssize_t>(json.size()));
}

static PyObject* Indexer_explain(IndexerObject* self, PyObject* args) {
    const char* query = nullptr;
    if (!PyArg_ParseTuple(args, "s", &query)) return nullptr;
    std::string json;
    if (!with_indexer(self, [&](dftracer::utils::index::Indexer ix) {
            json = dftracer::utils::index::to_json(ix.explain(query));
        }))
        return nullptr;
    return PyUnicode_FromStringAndSize(json.data(),
                                       static_cast<Py_ssize_t>(json.size()));
}

static PyObject* Indexer_rebuild_extension(IndexerObject* self,
                                           PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    dftracer::utils::index::IndexStatus st;
    if (!with_indexer(self, [&](dftracer::utils::index::Indexer ix) {
            st = ix.rebuild_extension(name);
        }))
        return nullptr;
    return status_dict(st);
}

static PyObject* Indexer_drop_extension(IndexerObject* self, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    dftracer::utils::index::IndexStatus st;
    if (!with_indexer(self, [&](dftracer::utils::index::Indexer ix) {
            st = ix.drop_extension(name);
        }))
        return nullptr;
    return status_dict(st);
}

static PyObject* Indexer_get_checkpoint_indexer(IndexerObject* self,
                                                PyObject* args) {
    const char* file_path = nullptr;
    if (!PyArg_ParseTuple(args, "s", &file_path)) {
        return nullptr;
    }

    // Determine index path using BatchIndexer's index_dir setting
    const char* index_dir = as_utf8(self->index_dir);
    std::string index_path =
        dftracer::utils::trace::internal::determine_index_path(
            file_path, index_dir ? index_dir : "");

    // Create IndexerObject
    CheckpointIndexerObject* indexer =
        (CheckpointIndexerObject*)CheckpointIndexerType.tp_alloc(
            &CheckpointIndexerType, 0);
    if (!indexer) {
        return nullptr;
    }

    indexer->handle = nullptr;
    indexer->gz_path = PyUnicode_FromString(file_path);
    indexer->index_path = PyUnicode_FromString(index_path.c_str());
    indexer->checkpoint_size = self->checkpoint_size;
    indexer->build_bloom = 0;

    // Share runtime reference
    if (self->runtime_obj) {
        Py_INCREF(self->runtime_obj);
        indexer->runtime_obj = self->runtime_obj;
    } else {
        indexer->runtime_obj = nullptr;
    }

    // Create the native handle
    try {
        auto native =
            dftracer::utils::index::gzip::CheckpointIndexerFactory::create(
                file_path, index_path, self->checkpoint_size, false);
        if (!native) {
            Py_DECREF((PyObject*)indexer);
            PyErr_SetString(PyExc_RuntimeError, "Unsupported archive format");
            return nullptr;
        }
        indexer->handle = new std::shared_ptr<
            dftracer::utils::index::gzip::CheckpointIndexer>(std::move(native));
    } catch (const std::exception& e) {
        Py_DECREF((PyObject*)indexer);
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }

    return (PyObject*)indexer;
}

static std::optional<std::string> resolve_index_path(IndexerObject* self) {
    PyObject* status = Indexer_resolve(self, nullptr);
    if (!status) return std::nullopt;
    PyObject* obj = PyDict_GetItemString(status, "index_path");
    const char* path = obj ? as_utf8(obj) : nullptr;
    if (!path || path[0] == '\0') {
        Py_DECREF(status);
        PyErr_SetString(PyExc_RuntimeError, "No index path available");
        return std::nullopt;
    }
    std::string result(path);
    Py_DECREF(status);
    return result;
}

static PyObject* Indexer_rowset(IndexerObject* self, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    dftracer::utils::dataframe::DataFrame frame;
    if (!with_indexer(self, [&](dftracer::utils::index::Indexer ix) {
            frame = ix.rowset(name);
        }))
        return nullptr;
    return dftracer::utils::python::wrap_dataframe(std::move(frame));
}

static PyObject* Indexer_query_file_pids(IndexerObject* self, PyObject* args) {
    int file_id;
    if (!PyArg_ParseTuple(args, "i", &file_id)) {
        return nullptr;
    }

    using dftracer::utils::index::store::IndexDatabase;

    auto idx_opt = resolve_index_path(self);
    if (!idx_opt) return nullptr;
    std::string index_path = std::move(*idx_opt);

    ankerl::unordered_dense::set<std::uint64_t> pids;
    if (!run_blocking_r(
            [&] {
                IndexDatabase db(
                    index_path,
                    dftracer::utils::index::store::IndexOpenMode::ReadOnly);
                return db.query_file_pids(file_id);
            },
            pids)) {
        return nullptr;
    }

    PyObject* set = PySet_New(nullptr);
    if (!set) return nullptr;

    for (auto pid : pids) {
        PyObject* val = PyLong_FromUnsignedLongLong(pid);
        PySet_Add(set, val);
        Py_DECREF(val);
    }

    return set;
}

static PyObject* Indexer_query_all_file_pids(IndexerObject* self,
                                             PyObject* Py_UNUSED(ignored)) {
    using dftracer::utils::index::store::IndexDatabase;

    auto idx_opt = resolve_index_path(self);
    if (!idx_opt) return nullptr;
    std::string index_path = std::move(*idx_opt);

    ankerl::unordered_dense::map<int,
                                 ankerl::unordered_dense::set<std::uint64_t>>
        all_pids;
    if (!run_blocking_r(
            [&] {
                IndexDatabase db(
                    index_path,
                    dftracer::utils::index::store::IndexOpenMode::ReadOnly);
                return db.query_all_file_pids();
            },
            all_pids)) {
        return nullptr;
    }

    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;

    for (const auto& [file_id, pids] : all_pids) {
        PyObject* key = PyLong_FromLong(file_id);
        PyObject* set = PySet_New(nullptr);
        for (auto pid : pids) {
            PyObject* val = PyLong_FromUnsignedLongLong(pid);
            PySet_Add(set, val);
            Py_DECREF(val);
        }
        PyDict_SetItem(dict, key, set);
        Py_DECREF(key);
        Py_DECREF(set);
    }

    return dict;
}

static PyObject* Indexer_query_file_info(IndexerObject* self,
                                         PyObject* Py_UNUSED(ignored)) {
    using dftracer::utils::index::store::IndexDatabase;

    auto idx_opt = resolve_index_path(self);
    if (!idx_opt) return nullptr;
    std::string index_path = std::move(*idx_opt);

    dftracer::utils::StringViewMap<int> file_ids;
    ankerl::unordered_dense::map<int,
                                 ankerl::unordered_dense::set<std::uint64_t>>
        all_pids;

    if (!run_blocking([&] {
            IndexDatabase db(
                index_path,
                dftracer::utils::index::store::IndexOpenMode::ReadOnly);
            file_ids = db.query_all_file_info_ids();
            all_pids = db.query_all_file_pids();
        })) {
        return nullptr;
    }

    auto data_dir = fs::weakly_canonical(fs::path(index_path)).parent_path();

    PyObject* id_to_path = PyDict_New();
    if (!id_to_path) return nullptr;
    for (const auto& [logical_name, fid] : file_ids) {
        auto resolved = (data_dir / logical_name).string();
        PyObject* key = PyLong_FromLong(fid);
        PyObject* val = PyUnicode_FromStringAndSize(
            resolved.data(), static_cast<Py_ssize_t>(resolved.size()));
        PyDict_SetItem(id_to_path, key, val);
        Py_DECREF(key);
        Py_DECREF(val);
    }

    PyObject* pid_dict = PyDict_New();
    if (!pid_dict) {
        Py_DECREF(id_to_path);
        return nullptr;
    }
    for (const auto& [file_id, pids] : all_pids) {
        PyObject* key = PyLong_FromLong(file_id);
        PyObject* set = PySet_New(nullptr);
        for (auto pid : pids) {
            PyObject* val = PyLong_FromUnsignedLongLong(pid);
            PySet_Add(set, val);
            Py_DECREF(val);
        }
        PyDict_SetItem(pid_dict, key, set);
        Py_DECREF(key);
        Py_DECREF(set);
    }

    PyObject* result = PyTuple_Pack(2, id_to_path, pid_dict);
    Py_DECREF(id_to_path);
    Py_DECREF(pid_dict);
    return result;
}

static PyMethodDef Indexer_methods[] = {
    {"get_checkpoint_indexer", DFTU_PYCFUNCTION(Indexer_get_checkpoint_indexer),
     METH_VARARGS,
     "get_checkpoint_indexer(file_path)\n"
     "--\n\n"
     "Get a checkpoint indexer for a specific file.\n\n"
     "Args:\n"
     "    file_path: Path to the trace file (.pfw/.pfw.gz)\n\n"
     "Returns:\n"
     "    Indexer instance for checkpoint-level operations.\n"},
    {"resolve", DFTU_PYCFUNCTION(Indexer_resolve), METH_NOARGS,
     "resolve()\n"
     "--\n\n"
     "Check what files exist vs need indexing.\n\n"
     "Returns:\n"
     "    dict with 'total_files', 'ready', 'needs_work', 'index_path'\n"},
    {"build", DFTU_PYCFUNCTION(Indexer_build), METH_NOARGS,
     "build()\n"
     "--\n\n"
     "Build all missing index tiers based on require_* flags.\n"},
    {"ensure_indexed", DFTU_PYCFUNCTION(Indexer_ensure_indexed), METH_NOARGS,
     "ensure_indexed()\n"
     "--\n\n"
     "Resolve and build if needed.\n\n"
     "Returns:\n"
     "    dict with index status after building.\n"},
    {"manifest", DFTU_PYCFUNCTION(Indexer_manifest), METH_NOARGS,
     "manifest()\n"
     "--\n\n"
     "The manifest of every indexed file, as a JSON string.\n"},
    {"explain", DFTU_PYCFUNCTION(Indexer_explain), METH_VARARGS,
     "explain(query)\n"
     "--\n\n"
     "Per file, the chunks the query reads and what each pruning extension\n"
     "rules out alone, as a JSON string.\n"},
    {"rebuild_extension", DFTU_PYCFUNCTION(Indexer_rebuild_extension),
     METH_VARARGS,
     "rebuild_extension(name)\n"
     "--\n\n"
     "Rewrite one tier extension of every file; returns the status dict.\n"},
    {"drop_extension", DFTU_PYCFUNCTION(Indexer_drop_extension), METH_VARARGS,
     "drop_extension(name)\n"
     "--\n\n"
     "Remove one tier extension from every file; returns the status dict.\n"},
    {"rowset", DFTU_PYCFUNCTION(Indexer_rowset), METH_VARARGS,
     "rowset(name)\n"
     "--\n\n"
     "The rows the index build stored for row set `name` of the source\n"
     "(for example 'files'), as a DataFrame.\n"},
    {"query_file_pids", DFTU_PYCFUNCTION(Indexer_query_file_pids), METH_VARARGS,
     "query_file_pids(file_id)\n"
     "--\n\n"
     "Query PIDs observed in a specific file.\n\n"
     "Args:\n"
     "    file_id: Integer file ID from index.\n\n"
     "Returns:\n"
     "    set of PIDs.\n"},
    {"query_all_file_pids", DFTU_PYCFUNCTION(Indexer_query_all_file_pids),
     METH_NOARGS,
     "query_all_file_pids()\n"
     "--\n\n"
     "Query PIDs for all indexed files.\n\n"
     "Returns:\n"
     "    dict mapping file_id to set of PIDs.\n"},
    {"query_file_info", DFTU_PYCFUNCTION(Indexer_query_file_info), METH_NOARGS,
     "query_file_info()\n"
     "--\n\n"
     "Query file ID to path mapping and per-file PIDs in one call.\n\n"
     "Returns:\n"
     "    tuple of (dict[int, str], dict[int, set[int]]).\n"},
    {nullptr}};

static PyGetSetDef Indexer_getsetters[] = {{nullptr}};

PyTypeObject IndexerType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "dftracer_utils_ext.Indexer",
    sizeof(IndexerObject),
    0,
    (destructor)Indexer_dealloc,
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
    "BatchIndexer(directory='', files=None, index_dir='',\n"
    "             require_checkpoint=True, require_bloom=True,\n"
    "             time_interval_ms=5000.0, group_keys=None,\n"
    "             custom_metric_fields=None, compute_percentiles=False,\n"
    "             parallelism=0, force_rebuild=False, runtime=None)\n"
    "--\n\n"
    "Indexer with tiered index building.\n\n"
    "At least one of 'directory' or 'files' must be provided.\n"
    "- directory: scan for .pfw/.jsonl/.ndjson/.json files, plain or gzip\n"
    "- files: list of specific file paths\n\n"
    "Supports:\n"
    "- Tier 1: Checkpoints (require_checkpoint)\n"
    "- Tier 2: Bloom filters (require_bloom)\n"
    "- Tier 3: Aggregation (require_aggregation + config params)\n",
    0,
    0,
    0,
    0,
    0,
    0,
    Indexer_methods,
    0,
    Indexer_getsetters,
    0,
    0,
    0,
    0,
    0,
    (initproc)Indexer_init,
    0,
    Indexer_new,
};

namespace {

template <class Fn>
PyObject* schema_str(Fn fn) {
    try {
        const std::string s = fn();
        return PyUnicode_FromStringAndSize(s.data(),
                                           static_cast<Py_ssize_t>(s.size()));
    } catch (const std::exception& e) {
        dftracer::utils::python::set_typed_py_error(e);
        return nullptr;
    }
}

PyObject* schema_register_fn(PyObject*, PyObject* args) {
    const char* text = nullptr;
    const char* source = "<text>";
    if (!PyArg_ParseTuple(args, "s|s", &text, &source)) return nullptr;
    return schema_str([&] {
        return dftracer::utils::index::register_schema(text, source).id;
    });
}

PyObject* schema_load_fn(PyObject*, PyObject* args) {
    const char* path = nullptr;
    if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;
    return schema_str([&] {
        dftracer::utils::index::load_schemas(path);
        return dftracer::utils::index::schemas_json();
    });
}

PyObject* schema_list_fn(PyObject*, PyObject*) {
    return schema_str([] { return dftracer::utils::index::schemas_json(); });
}

PyObject* schema_detect_fn(PyObject*, PyObject* args) {
    const char* path = nullptr;
    if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;
    return schema_str(
        [&] { return dftracer::utils::index::detect_file_schema(path).id; });
}

PyObject* schema_explain_fn(PyObject*, PyObject* args) {
    const char* path = nullptr;
    if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;
    return schema_str([&] {
        return dftracer::utils::index::to_json(
            dftracer::utils::index::explain_file_schema(path));
    });
}

PyMethodDef SCHEMA_METHODS[] = {
    {"_schema_register", schema_register_fn, METH_VARARGS,
     "_schema_register(text, source='<text>')\n--\n\nRegister a YAML or "
     "JSON schema spec; returns its id.\n"},
    {"_schema_load", schema_load_fn, METH_VARARGS,
     "_schema_load(path)\n--\n\nRegister every spec at a file or "
     "directory; returns the registered schemas as JSON.\n"},
    {"_schema_list", schema_list_fn, METH_NOARGS,
     "_schema_list()\n--\n\nThe registered schemas as JSON.\n"},
    {"_schema_detect", schema_detect_fn, METH_VARARGS,
     "_schema_detect(path)\n--\n\nThe schema detected for a trace.\n"},
    {"_schema_explain", schema_explain_fn, METH_VARARGS,
     "_schema_explain(path)\n--\n\nEvery schema's detection share for a "
     "trace and the chosen one, as JSON.\n"},
    {nullptr, nullptr, 0, nullptr}};

}  // namespace

int dftracer::utils::python::init_indexer(PyObject* m) {
    if (register_type(m, &IndexerType, "Indexer") < 0) return -1;
    if (PyModule_AddFunctions(m, SCHEMA_METHODS) < 0) return -1;

    return 0;
}
