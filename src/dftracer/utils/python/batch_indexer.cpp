#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/hash/hash_combine.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/python/batch_indexer.h>
#include <dftracer/utils/python/indexer.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_list_helpers.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/aggregators/aggregation_config.h>
#include <dftracer/utils/trace/aggregators/aggregation_serialization.h>
#include <dftracer/utils/trace/aggregators/aggregator_types.h>
#include <dftracer/utils/trace/aggregators/event_aggregator.h>
#include <dftracer/utils/trace/aggregators/system_metrics.h>
#include <dftracer/utils/trace/aggregators/system_metrics_serialization.h>
#include <dftracer/utils/trace/indexing/index_resolver_utility.h>
#include <dftracer/utils/trace/indexing/resolve_and_build.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::trace::indexing;
using namespace dftracer::utils::trace::aggregators;

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

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|sOsppppdOOppnnpO", const_cast<char**>(kwlist),
            &directory, &files_obj, &index_dir, &require_checkpoint,
            &require_bloom, &build_bloom, &require_aggregation,
            &time_interval_ms, &group_keys_obj, &custom_metrics_obj,
            &compute_percentiles, &group_by_file, &checkpoint_size,
            &parallelism, &force_rebuild, &runtime_arg)) {
        return -1;
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

// ---------------------------------------------------------------------------
// resolve() - check what exists vs needs building
// ---------------------------------------------------------------------------

static PyObject* Indexer_resolve(IndexerObject* self,
                                 PyObject* Py_UNUSED(ignored)) {
    const char* directory = as_utf8(self->directory);
    const char* index_dir = as_utf8(self->index_dir);

    ResolverInput input;
    input.directory = directory ? directory : "";
    input.index_dir = index_dir ? index_dir : "";
    input.require_checkpoints = self->require_checkpoint;
    input.require_bloom = self->require_bloom;
    input.require_aggregation = self->require_aggregation;
    input.checkpoint_size = self->checkpoint_size;
    input.aggregation_config = build_aggregation_config(self);

    // Add files if provided
    if (self->files && PyList_Check(self->files)) {
        Py_ssize_t n = PyList_Size(self->files);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char* s = as_utf8(PyList_GetItem(self->files, i));
            if (s) input.files.emplace_back(s);
        }
    }

    ResolverResult result;

    if (!run_blocking([&] {
            Runtime* rt = get_batch_indexer_runtime(self);
            rt->submit(run_coro_scope(
                           rt->executor(),
                           [](CoroScope& scope, ResolverInput in,
                              ResolverResult* out) -> CoroTask<void> {
                               IndexResolverUtility resolver;
                               *out = co_await resolver(scope, std::move(in));
                           },
                           std::move(input), &result),
                       "batch-indexer-resolve")
                .get();
        })) {
        return nullptr;
    }

    // Build result dict
    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;

    dict_set_steal(dict, "total_files",
                   PyLong_FromSize_t(result.all_files.size()));
    dict_set_steal(dict, "index_path",
                   PyUnicode_FromString(result.index_path.c_str()));
    dict_set_steal(dict, "aggregation_interval_us",
                   PyLong_FromUnsignedLongLong(result.stored_time_interval_us));
    dict_set_steal(dict, "needs_rebuild",
                   PyBool_FromLong(result.needs_augmentation));

    // Ready files
    PyObject* ready_list = PyList_New(result.cached.size());
    for (std::size_t i = 0; i < result.cached.size(); ++i) {
        PyList_SetItem(
            ready_list, i,
            PyUnicode_FromString(result.cached[i].file_path.c_str()));
    }
    PyDict_SetItemString(dict, "ready", ready_list);

    // Needs work files (union of all needs_* lists)
    std::vector<std::string> needs_work;
    for (const auto& item : result.needs_checkpoint) {
        needs_work.push_back(item.file_path);
    }
    for (const auto& item : result.needs_bloom) {
        bool found = false;
        for (const auto& existing : needs_work) {
            if (existing == item.file_path) {
                found = true;
                break;
            }
        }
        if (!found) needs_work.push_back(item.file_path);
    }
    for (const auto& item : result.needs_aggregation) {
        bool found = false;
        for (const auto& existing : needs_work) {
            if (existing == item.file_path) {
                found = true;
                break;
            }
        }
        if (!found) needs_work.push_back(item.file_path);
    }

    PyObject* needs_list = PyList_New(needs_work.size());
    for (std::size_t i = 0; i < needs_work.size(); ++i) {
        PyList_SetItem(needs_list, i,
                       PyUnicode_FromString(needs_work[i].c_str()));
    }
    PyDict_SetItemString(dict, "needs_work", needs_list);

    return dict;
}

// ---------------------------------------------------------------------------
// build() - build missing index tiers
// ---------------------------------------------------------------------------

static PyObject* Indexer_build(IndexerObject* self,
                               PyObject* Py_UNUSED(ignored)) {
    const char* directory = as_utf8(self->directory);
    const char* index_dir = as_utf8(self->index_dir);

    ResolveAndBuildInput input;
    input.directory = directory ? directory : "";
    input.index_dir = index_dir ? index_dir : "";
    input.require_checkpoints = self->require_checkpoint;
    input.require_bloom = self->require_bloom;
    input.build_bloom = self->build_bloom;
    input.require_aggregation = self->require_aggregation;
    input.aggregation_config = build_aggregation_config(self);
    input.checkpoint_size = self->checkpoint_size;
    input.parallelism = self->parallelism;
    input.force_rebuild = self->force_rebuild;

    // Add files if provided
    if (self->files && PyList_Check(self->files)) {
        Py_ssize_t n = PyList_Size(self->files);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char* s = as_utf8(PyList_GetItem(self->files, i));
            if (s) input.files.emplace_back(s);
        }
    }

    if (!run_blocking([&] {
            Runtime* rt = get_batch_indexer_runtime(self);
            rt->submit(run_coro_scope(
                           rt->executor(),
                           [](CoroScope& scope,
                              ResolveAndBuildInput in) -> CoroTask<void> {
                               co_await resolve_and_build_index(&scope,
                                                                std::move(in));
                           },
                           std::move(input)),
                       "batch-indexer-build")
                .get();
        })) {
        return nullptr;
    }

    Py_RETURN_NONE;
}

// ---------------------------------------------------------------------------
// ensure_indexed() - resolve + build if needed
// ---------------------------------------------------------------------------

static PyObject* Indexer_ensure_indexed(IndexerObject* self,
                                        PyObject* Py_UNUSED(ignored)) {
    // First resolve
    PyObject* status = Indexer_resolve(self, nullptr);
    if (!status) return nullptr;

    // Build if files need work, or the aggregation tier must be rebuilt
    // (stored time interval differs from the requested one).
    PyObject* needs_work = PyDict_GetItemString(status, "needs_work");
    PyObject* needs_rebuild = PyDict_GetItemString(status, "needs_rebuild");
    bool work_pending = needs_work && PyList_Size(needs_work) > 0;
    bool rebuild_pending = needs_rebuild && PyObject_IsTrue(needs_rebuild);
    if (work_pending || rebuild_pending) {
        Py_DECREF(status);

        // Build
        PyObject* result = Indexer_build(self, nullptr);
        if (!result) return nullptr;
        Py_DECREF(result);

        // Re-resolve
        status = Indexer_resolve(self, nullptr);
    }

    return status;
}

// ---------------------------------------------------------------------------
// get_checkpoint_indexer() - get a single-file checkpoint indexer
// ---------------------------------------------------------------------------

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
    indexer->handle = dftu_indexer_create(file_path, index_path.c_str(),
                                          self->checkpoint_size, 0);
    if (!indexer->handle) {
        Py_DECREF((PyObject*)indexer);
        PyErr_SetString(PyExc_RuntimeError,
                        "Failed to create checkpoint indexer");
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

static PyObject* Indexer_get_hash_table(IndexerObject* self, PyObject* args) {
    const char* type_str = nullptr;
    if (!PyArg_ParseTuple(args, "s", &type_str)) {
        return nullptr;
    }

    using dftracer::utils::utilities::indexer::IndexDatabase;
    using HashType = IndexDatabase::HashType;

    HashType type;
    if (std::strcmp(type_str, "file") == 0) {
        type = HashType::FILE;
    } else if (std::strcmp(type_str, "host") == 0) {
        type = HashType::HOST;
    } else if (std::strcmp(type_str, "string") == 0) {
        type = HashType::STRING;
    } else if (std::strcmp(type_str, "proc") == 0) {
        type = HashType::PROC;
    } else {
        PyErr_SetString(PyExc_ValueError,
                        "type must be 'file', 'host', 'string', or 'proc'");
        return nullptr;
    }

    auto idx_opt = resolve_index_path(self);
    if (!idx_opt) return nullptr;
    std::string index_path = std::move(*idx_opt);

    std::unordered_map<std::string, std::string> hash_map;
    if (!run_blocking_r(
            [&] {
                IndexDatabase db(index_path,
                                 dftracer::utils::utilities::indexer::
                                     IndexOpenMode::ReadOnly);
                return db.query_hash_table(type);
            },
            hash_map)) {
        return nullptr;
    }

    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;

    for (const auto& [hash, name] : hash_map) {
        PyObject* key = PyUnicode_FromStringAndSize(hash.data(), hash.size());
        PyObject* val = PyUnicode_FromStringAndSize(name.data(), name.size());
        PyDict_SetItem(dict, key, val);
        Py_DECREF(key);
        Py_DECREF(val);
    }

    return dict;
}

static PyObject* Indexer_query_file_pids(IndexerObject* self, PyObject* args) {
    int file_id;
    if (!PyArg_ParseTuple(args, "i", &file_id)) {
        return nullptr;
    }

    using dftracer::utils::utilities::indexer::IndexDatabase;

    auto idx_opt = resolve_index_path(self);
    if (!idx_opt) return nullptr;
    std::string index_path = std::move(*idx_opt);

    std::unordered_set<std::uint64_t> pids;
    if (!run_blocking_r(
            [&] {
                IndexDatabase db(index_path,
                                 dftracer::utils::utilities::indexer::
                                     IndexOpenMode::ReadOnly);
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
    using dftracer::utils::utilities::indexer::IndexDatabase;

    auto idx_opt = resolve_index_path(self);
    if (!idx_opt) return nullptr;
    std::string index_path = std::move(*idx_opt);

    std::unordered_map<int, std::unordered_set<std::uint64_t>> all_pids;
    if (!run_blocking_r(
            [&] {
                IndexDatabase db(index_path,
                                 dftracer::utils::utilities::indexer::
                                     IndexOpenMode::ReadOnly);
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
    using dftracer::utils::utilities::indexer::IndexDatabase;

    auto idx_opt = resolve_index_path(self);
    if (!idx_opt) return nullptr;
    std::string index_path = std::move(*idx_opt);

    std::unordered_map<std::string, int> file_ids;
    std::unordered_map<int, std::unordered_set<std::uint64_t>> all_pids;

    if (!run_blocking([&] {
            IndexDatabase db(
                index_path,
                dftracer::utils::utilities::indexer::IndexOpenMode::ReadOnly);
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

#ifdef DFTRACER_UTILS_ENABLE_ARROW
static PyObject* count_hash_entries_fn(PyObject* /*self*/, PyObject* args) {
    const char* index_path = nullptr;
    const char* type_str = nullptr;
    if (!PyArg_ParseTuple(args, "ss", &index_path, &type_str)) return nullptr;

    using dftracer::utils::utilities::indexer::IndexDatabase;
    using HashType = IndexDatabase::HashType;

    HashType type;
    if (std::strcmp(type_str, "file") == 0) {
        type = HashType::FILE;
    } else if (std::strcmp(type_str, "host") == 0) {
        type = HashType::HOST;
    } else if (std::strcmp(type_str, "string") == 0) {
        type = HashType::STRING;
    } else if (std::strcmp(type_str, "proc") == 0) {
        type = HashType::PROC;
    } else {
        PyErr_SetString(PyExc_ValueError,
                        "type must be 'file', 'host', 'string', or 'proc'");
        return nullptr;
    }

    std::uint64_t count = 0;
    if (!run_blocking_r(
            [&] {
                IndexDatabase db(index_path,
                                 dftracer::utils::utilities::indexer::
                                     IndexOpenMode::ReadOnly);
                return db.count_hash_entries(type);
            },
            count)) {
        return nullptr;
    }
    return PyLong_FromUnsignedLongLong(count);
}

static PyMethodDef BatchIndexerModuleMethods[] = {
    {"count_hash_entries", DFTU_PYCFUNCTION(count_hash_entries_fn),
     METH_VARARGS,
     "count_hash_entries(index_path, type)\n"
     "--\n\n"
     "Number of hashes of `type` ('file', 'host', 'string', 'proc') in the\n"
     "index at `index_path`. Counted by iteration, so a table holding tens\n"
     "of millions of entries is not materialised to take its length.\n"},
    {nullptr, nullptr, 0, nullptr}};
#endif

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
    {"get_hash_table", DFTU_PYCFUNCTION(Indexer_get_hash_table), METH_VARARGS,
     "get_hash_table(type)\n"
     "--\n\n"
     "Query hash table mappings.\n\n"
     "Args:\n"
     "    type: 'file', 'host', 'string', or 'proc'\n\n"
     "Returns:\n"
     "    dict mapping hash values to resolved names.\n"},
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
    "- directory: scan for .pfw/.pfw.gz files\n"
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

int dftracer::utils::python::init_indexer(PyObject* m) {
    if (register_type(m, &IndexerType, "Indexer") < 0) return -1;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    if (PyModule_AddFunctions(m, BatchIndexerModuleMethods) < 0) return -1;
#endif

    return 0;
}
