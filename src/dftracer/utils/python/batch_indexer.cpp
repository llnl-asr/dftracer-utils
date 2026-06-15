#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/python/batch_indexer.h>
#include <dftracer/utils/python/indexer.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregator_types.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics_serialization.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/resolve_and_build.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::composites::dft::aggregators;

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

static PyObject* Indexer_new(PyTypeObject* type, PyObject* args,
                             PyObject* kwds) {
    IndexerObject* self = (IndexerObject*)type->tp_alloc(type, 0);
    if (self) {
        self->runtime_obj = nullptr;
        self->directory = nullptr;
        self->files = nullptr;
        self->index_dir = nullptr;
        self->require_checkpoint = 1;
        self->require_bloom = 1;
        self->require_manifest = 1;
        self->require_aggregation = 0;
        self->time_interval_ms = 5000.0;
        self->group_keys = nullptr;
        self->custom_metric_fields = nullptr;
        self->compute_percentiles = 0;
        self->checkpoint_size = 32 * 1024 * 1024;
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
                                   "require_manifest",
                                   "require_aggregation",
                                   "time_interval_ms",
                                   "group_keys",
                                   "custom_metric_fields",
                                   "compute_percentiles",
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
    int require_manifest = 1;
    int require_aggregation = 0;
    double time_interval_ms = 5000.0;
    PyObject* group_keys_obj = Py_None;
    PyObject* custom_metrics_obj = Py_None;
    int compute_percentiles = 0;
    Py_ssize_t checkpoint_size = 32 * 1024 * 1024;  // 32MB default
    Py_ssize_t parallelism = 0;
    int force_rebuild = 0;
    PyObject* runtime_arg = nullptr;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|sOsppppdOOpnnpO", (char**)kwlist, &directory,
            &files_obj, &index_dir, &require_checkpoint, &require_bloom,
            &require_manifest, &require_aggregation, &time_interval_ms,
            &group_keys_obj, &custom_metrics_obj, &compute_percentiles,
            &checkpoint_size, &parallelism, &force_rebuild, &runtime_arg)) {
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
    self->require_manifest = require_manifest;
    self->require_aggregation = require_aggregation;
    self->time_interval_ms = time_interval_ms;
    self->compute_percentiles = compute_percentiles;
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
    return get_default_runtime();
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
            const char* s =
                PyUnicode_AsUTF8(PyList_GetItem(self->group_keys, i));
            if (s) config.extra_group_keys.emplace_back(s);
        }
    }
    if (self->custom_metric_fields &&
        PyList_Check(self->custom_metric_fields)) {
        Py_ssize_t n = PyList_Size(self->custom_metric_fields);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char* s =
                PyUnicode_AsUTF8(PyList_GetItem(self->custom_metric_fields, i));
            if (s) config.custom_metric_fields.emplace_back(s);
        }
    }

    config.compute_percentiles = self->compute_percentiles != 0;
    return config;
}

// ---------------------------------------------------------------------------
// resolve() - check what exists vs needs building
// ---------------------------------------------------------------------------

static PyObject* Indexer_resolve(IndexerObject* self,
                                 PyObject* Py_UNUSED(ignored)) {
    const char* directory = PyUnicode_AsUTF8(self->directory);
    const char* index_dir = PyUnicode_AsUTF8(self->index_dir);

    ResolverInput input;
    input.directory = directory ? directory : "";
    input.index_dir = index_dir ? index_dir : "";
    input.require_checkpoints = self->require_checkpoint;
    input.require_bloom = self->require_bloom;
    input.require_manifest = self->require_manifest;
    input.require_aggregation = self->require_aggregation;
    input.aggregation_config = build_aggregation_config(self);

    // Add files if provided
    if (self->files && PyList_Check(self->files)) {
        Py_ssize_t n = PyList_Size(self->files);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char* s = PyUnicode_AsUTF8(PyList_GetItem(self->files, i));
            if (s) input.files.emplace_back(s);
        }
    }

    ResolverResult result;
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime* rt = get_batch_indexer_runtime(self);
        rt->submit(run_coro_scope(
                       rt->executor(),
                       [](CoroScope& scope, ResolverInput in,
                          ResolverResult* out) -> CoroTask<void> {
                           IndexResolverUtility resolver;
                           // Use scope.spawn(utility, input) which auto-binds
                           // context for utilities with NeedsContext tag
                           *out = co_await scope.spawn(resolver, std::move(in));
                       },
                       std::move(input), &result),
                   "batch-indexer-resolve")
            .get();
    } catch (const std::exception& e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
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
    for (const auto& item : result.needs_manifest) {
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
    const char* directory = PyUnicode_AsUTF8(self->directory);
    const char* index_dir = PyUnicode_AsUTF8(self->index_dir);

    ResolveAndBuildInput input;
    input.directory = directory ? directory : "";
    input.index_dir = index_dir ? index_dir : "";
    input.require_checkpoints = self->require_checkpoint;
    input.require_bloom = self->require_bloom;
    input.require_manifest = self->require_manifest;
    input.require_aggregation = self->require_aggregation;
    input.aggregation_config = build_aggregation_config(self);
    input.checkpoint_size = self->checkpoint_size;
    input.parallelism = self->parallelism;
    input.force_rebuild = self->force_rebuild;

    // Add files if provided
    if (self->files && PyList_Check(self->files)) {
        Py_ssize_t n = PyList_Size(self->files);
        for (Py_ssize_t i = 0; i < n; i++) {
            const char* s = PyUnicode_AsUTF8(PyList_GetItem(self->files, i));
            if (s) input.files.emplace_back(s);
        }
    }

    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
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
    } catch (const std::exception& e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
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
    const char* index_dir = PyUnicode_AsUTF8(self->index_dir);
    std::string index_path = dftracer::utils::utilities::composites::dft::
        internal::determine_index_path(file_path, index_dir ? index_dir : "");

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
    indexer->build_manifest = 0;

    // Share runtime reference
    if (self->runtime_obj) {
        Py_INCREF(self->runtime_obj);
        indexer->runtime_obj = self->runtime_obj;
    } else {
        indexer->runtime_obj = nullptr;
    }

    // Create the native handle
    indexer->handle = dft_indexer_create(file_path, index_path.c_str(),
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
    const char* path = obj ? PyUnicode_AsUTF8(obj) : nullptr;
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
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        hash_map = db.query_hash_table(type);
    } catch (const std::exception& e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
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
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        pids = db.query_file_pids(file_id);
    } catch (const std::exception& e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
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
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        all_pids = db.query_all_file_pids();
    } catch (const std::exception& e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
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
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        file_ids = db.query_all_file_info_ids();
        all_pids = db.query_all_file_pids();
    } catch (const std::exception& e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
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
#include <dftracer/utils/python/trace_reader_iterator.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>

static PyObject* create_arrow_batch_capsule(
    dftracer::utils::utilities::common::arrow::ArrowExportResult&& result) {
    auto* obj = (ArrowBatchCapsuleObject*)ArrowBatchCapsuleType.tp_alloc(
        &ArrowBatchCapsuleType, 0);
    if (!obj) return nullptr;
    obj->result =
        new dftracer::utils::utilities::common::arrow::ArrowExportResult(
            std::move(result));
    return (PyObject*)obj;
}

namespace {

using dftracer::utils::utilities::common::arrow::ArrowExportResult;
using dftracer::utils::utilities::common::arrow::ColumnSpec;
using dftracer::utils::utilities::common::arrow::ColumnType;
using dftracer::utils::utilities::common::arrow::RecordBatchBuilder;

static bool parse_agg_type_str(const char* type_str, AggMapType& out) {
    if (strcmp(type_str, "events") == 0) {
        out = AggMapType::EVENT;
        return true;
    }
    if (strcmp(type_str, "profiles") == 0) {
        out = AggMapType::PROFILE;
        return true;
    }
    if (strcmp(type_str, "system") == 0) {
        out = AggMapType::SYSTEM;
        return true;
    }
    PyErr_SetString(PyExc_ValueError,
                    "type must be 'events', 'profiles', or 'system'");
    return false;
}

struct AggDbHandle {
    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db;
    std::unique_ptr<EventAggregator> agg;
};

static std::unique_ptr<AggDbHandle> open_agg_db(const std::string& index_path,
                                                std::string& error_msg) {
    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db;
    try {
        db = EventAggregator::open_with_merge_operator(index_path);
    } catch (...) {
        auto& mgr = dftracer::utils::rocksdb::RocksDBManager::instance();
        mgr.reset(index_path);
        db = mgr.get_or_open(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        if (db && db->is_open()) {
            load_intern_dictionary(*db);
        }
    }
    if (!db || !db->is_open()) {
        error_msg = "Failed to open aggregation database";
        return nullptr;
    }
    std::string config_val;
    auto key = std::string_view(AGG_GLOBAL_CONFIG_KEY,
                                sizeof(AGG_GLOBAL_CONFIG_KEY) - 1);
    if (!db->get(key, &config_val, dftracer::utils::rocksdb::cf::AGGREGATION)
             .ok()) {
        error_msg = "No aggregation config found - was aggregation enabled?";
        return nullptr;
    }
    auto cfg = deserialize_agg_global_config(config_val);
    auto handle = std::make_unique<AggDbHandle>();
    handle->db = db;
    handle->agg = std::make_unique<EventAggregator>(db, cfg.config_hash);
    return handle;
}

static std::optional<dftracer::utils::utilities::common::query::Query>
parse_query_arg(const char* query_str) {
    if (!query_str || query_str[0] == '\0') return std::nullopt;
    auto result = dftracer::utils::utilities::common::query::Query::from_string(
        query_str);
    if (!result) {
        PyErr_SetString(PyExc_ValueError, result.error().message.c_str());
        return std::nullopt;
    }
    return std::move(*result);
}

constexpr std::uint16_t DFT_NUM_SHARDS = 4096;

template <typename Output, typename ScanFn>
void parallel_shard_scan_range(Runtime* rt, std::uint16_t outer_begin,
                               std::uint16_t outer_end, ScanFn&& scan_fn,
                               std::vector<Output>& outputs) {
    if (outer_end <= outer_begin) return;
    const std::size_t span = static_cast<std::size_t>(outer_end - outer_begin);
    const std::size_t num_tasks = std::min<std::size_t>(rt->threads(), span);
    const std::size_t shards_per_task = (span + num_tasks - 1) / num_tasks;
    rt->submit(run_coro_scope(
                   rt->executor(),
                   [&](CoroScope& scope) -> CoroTask<void> {
                       std::vector<dftracer::utils::coro::SpawnFuture<Output>>
                           futures;
                       futures.reserve(num_tasks);
                       for (std::size_t t = 0; t < num_tasks; ++t) {
                           auto shard_begin = static_cast<std::uint16_t>(
                               outer_begin + t * shards_per_task);
                           auto shard_end =
                               static_cast<std::uint16_t>(std::min<std::size_t>(
                                   outer_begin + (t + 1) * shards_per_task,
                                   outer_end));
                           futures.push_back(
                               scope.spawn([&scan_fn, shard_begin, shard_end](
                                               CoroScope&) -> CoroTask<Output> {
                                   co_return scan_fn(shard_begin, shard_end);
                               }));
                       }
                       outputs.reserve(num_tasks);
                       for (auto& f : futures) {
                           outputs.push_back(co_await f);
                       }
                   }),
               "parallel-shard-scan-range")
        .get();
}

template <typename Output, typename ScanFn>
void parallel_shard_scan(Runtime* rt, ScanFn&& scan_fn,
                         std::vector<Output>& outputs) {
    parallel_shard_scan_range<Output>(rt, 0, DFT_NUM_SHARDS,
                                      std::forward<ScanFn>(scan_fn), outputs);
}

static void append_results_to_list(PyObject* list,
                                   std::vector<ArrowExportResult>& results) {
    for (auto& r : results) {
        PyObject* capsule = create_arrow_batch_capsule(std::move(r));
        if (capsule) {
            PyList_Append(list, capsule);
            Py_DECREF(capsule);
        }
    }
}

struct AggScanInput {
    const EventAggregator* agg;
    AggMapType target_type;
    AggregationBatchType batch_type;
    Py_ssize_t batch_size;
    std::uint16_t shard_begin;
    std::uint16_t shard_end;
};

struct AggScanOutput {
    std::vector<ArrowExportResult> results;
};

AggScanOutput scan_aggregation_shard_range(AggScanInput input) {
    AggScanOutput output;

    static const std::vector<ColumnSpec> schema = {
        {"batch_type", ColumnType::INT64},  {"cat", ColumnType::DICT_STRING},
        {"name", ColumnType::DICT_STRING},  {"pid", ColumnType::UINT64},
        {"tid", ColumnType::UINT64},        {"hhash", ColumnType::DICT_STRING},
        {"fhash", ColumnType::DICT_STRING}, {"time_bucket", ColumnType::UINT64},
        {"count", ColumnType::UINT64},      {"dur_total", ColumnType::UINT64},
        {"dur_min", ColumnType::UINT64},    {"dur_max", ColumnType::UINT64},
        {"dur_mean", ColumnType::DOUBLE},   {"dur_std", ColumnType::DOUBLE},
        {"size_total", ColumnType::UINT64}, {"size_min", ColumnType::UINT64},
        {"size_max", ColumnType::UINT64},   {"size_mean", ColumnType::DOUBLE},
        {"size_std", ColumnType::DOUBLE},   {"ts", ColumnType::UINT64},
        {"te", ColumnType::UINT64},
    };

    RecordBatchBuilder builder;
    builder.declare_schema(schema);
    builder.reserve(static_cast<std::size_t>(input.batch_size));

    std::size_t row_count = 0;

    input.agg->scan_shard_range_raw(
        input.shard_begin, input.shard_end,
        [&](std::string_view key_bytes, std::string_view val_bytes) -> bool {
            AggKeyView kv;
            if (!parse_agg_key_view(key_bytes, kv)) return true;
            if (kv.map_type != input.target_type) return true;

            AggMetricsFullView mv;
            if (!parse_agg_value_full_view(val_bytes, mv)) return true;

            std::size_t ci = 0;
            builder.append_int64(ci++,
                                 static_cast<std::int64_t>(input.batch_type));
            builder.append_dict_string(ci++, kv.cat);
            builder.append_dict_string(ci++, kv.name);
            builder.append_uint64(ci++, kv.pid);
            builder.append_uint64(ci++, kv.tid);
            builder.append_dict_string(ci++, kv.hhash);
            builder.append_dict_string(ci++, kv.fhash);
            builder.append_uint64(ci++, kv.time_bucket);
            builder.append_uint64(ci++, mv.count);
            builder.append_uint64(ci++, mv.dur_total);
            builder.append_uint64(ci++, mv.count > 0 ? mv.dur_min : 0);
            builder.append_uint64(ci++, mv.dur_max);
            builder.append_double(ci++, mv.dur_mean);
            builder.append_double(ci++, mv.dur_stddev());
            builder.append_uint64(ci++, mv.size_total);
            builder.append_uint64(ci++, mv.count > 0 ? mv.size_min : 0);
            builder.append_uint64(ci++, mv.size_max);
            builder.append_double(ci++, mv.size_mean);
            builder.append_double(ci++, mv.size_stddev());
            builder.append_uint64(ci++, mv.ts);
            builder.append_uint64(ci++, mv.te);
            builder.end_row();

            row_count++;
            if (static_cast<Py_ssize_t>(row_count) >= input.batch_size) {
                auto arrow = builder.finish();
                if (arrow.valid()) {
                    output.results.push_back(std::move(arrow));
                }
                builder.reset(true);
                builder.reserve(static_cast<std::size_t>(input.batch_size));
                row_count = 0;
            }
            return true;
        });

    if (row_count > 0) {
        auto arrow = builder.finish();
        if (arrow.valid()) {
            output.results.push_back(std::move(arrow));
        }
    }

    return output;
}

enum class IOCategory : std::int8_t {
    READ = 1,
    WRITE = 2,
    METADATA = 3,
    PCTL = 4,
    IPC = 5,
    OTHER = 6,
    SYNC = 7,
};

inline IOCategory get_io_category(std::string_view func_name) {
    if (func_name == "read" || func_name == "pread" || func_name == "readv" ||
        func_name == "preadv" || func_name == "fread") {
        return IOCategory::READ;
    }
    if (func_name == "write" || func_name == "pwrite" ||
        func_name == "writev" || func_name == "pwritev" ||
        func_name == "fwrite") {
        return IOCategory::WRITE;
    }
    if (func_name == "fsync" || func_name == "fdatasync" ||
        func_name == "msync" || func_name == "sync") {
        return IOCategory::SYNC;
    }
    if (func_name == "open" || func_name == "open64" || func_name == "close" ||
        func_name == "fopen" || func_name == "fopen64" ||
        func_name == "fclose" || func_name == "stat" || func_name == "fstat" ||
        func_name == "lstat" || func_name == "fstatat" ||
        func_name == "__xstat" || func_name == "__xstat64" ||
        func_name == "__lxstat" || func_name == "__lxstat64" ||
        func_name == "__fxstat" || func_name == "__fxstat64" ||
        func_name == "access" || func_name == "lseek" ||
        func_name == "lseek64" || func_name == "fseek" ||
        func_name == "ftell" || func_name == "seek" || func_name == "fcntl" ||
        func_name == "ftruncate" || func_name == "mkdir" ||
        func_name == "rmdir" || func_name == "unlink" ||
        func_name == "remove" || func_name == "rename" || func_name == "link" ||
        func_name == "readlink" || func_name == "opendir" ||
        func_name == "closedir" || func_name == "readdir") {
        return IOCategory::METADATA;
    }
    return IOCategory::OTHER;
}

inline char* fast_itoa(std::uint64_t val, char* buf) {
    char* p = buf;
    do {
        *p++ = '0' + (val % 10);
        val /= 10;
    } while (val);
    std::reverse(buf, p);
    return p;
}

class HashResolver {
   public:
    HashResolver(
        const std::unordered_map<std::string, std::string>* file_hashes,
        const std::unordered_map<std::string, std::string>* host_hashes)
        : file_hashes_(file_hashes), host_hashes_(host_hashes) {
        if (file_hashes_) {
            for (const auto& [hash, name] : *file_hashes_) {
                auto hash_sv = intern_.intern(hash);
                auto name_sv = intern_.intern(name);
                file_map_[hash_sv] = name_sv;
            }
        }
        if (host_hashes_) {
            for (const auto& [hash, name] : *host_hashes_) {
                auto hash_sv = intern_.intern(hash);
                auto name_sv = intern_.intern(name);
                host_map_[hash_sv] = name_sv;
            }
        }
    }

    // Unresolved hashes resolve to empty (not the hash itself): the
    // dfanalyzer side treats empty file_name/host_name as missing (NA).
    std::string_view resolve_file(std::string_view hash) {
        if (hash.empty()) return hash;
        auto it = file_map_.find(intern_.intern(hash));
        return it != file_map_.end() ? it->second : std::string_view{};
    }

    std::string_view resolve_host(std::string_view hash) {
        if (hash.empty()) return hash;
        auto it = host_map_.find(intern_.intern(hash));
        return it != host_map_.end() ? it->second : std::string_view{};
    }

    std::string_view intern(std::string_view sv) { return intern_.intern(sv); }

   private:
    const std::unordered_map<std::string, std::string>* file_hashes_;
    const std::unordered_map<std::string, std::string>* host_hashes_;
    dftracer::utils::StringIntern intern_;
    std::unordered_map<std::string_view, std::string_view> file_map_;
    std::unordered_map<std::string_view, std::string_view> host_map_;
};

struct ProcKey {
    std::string_view hhash;
    std::uint64_t pid;
    std::uint64_t tid;
    bool operator==(const ProcKey& o) const {
        return hhash == o.hhash && pid == o.pid && tid == o.tid;
    }
};

struct ProcKeyHash {
    std::size_t operator()(const ProcKey& k) const {
        return std::hash<std::string_view>{}(k.hhash) ^
               (std::hash<std::uint64_t>{}(k.pid) << 1) ^
               (std::hash<std::uint64_t>{}(k.tid) << 2);
    }
};

static const std::vector<ColumnSpec> DFANALYZER_SCHEMA = {
    {"cat", ColumnType::DICT_STRING},
    {"func_name", ColumnType::DICT_STRING},
    {"pid", ColumnType::INT64},
    {"tid", ColumnType::INT64},
    {"file_hash", ColumnType::DICT_STRING},
    {"host_hash", ColumnType::DICT_STRING},
    {"file_name", ColumnType::DICT_STRING},
    {"host_name", ColumnType::DICT_STRING},
    {"proc_name", ColumnType::DICT_STRING},
    {"io_cat", ColumnType::INT64},
    {"acc_pat", ColumnType::INT64},
    {"count", ColumnType::INT64},
    {"time", ColumnType::DOUBLE},
    {"size", ColumnType::INT64},
    {"time_min", ColumnType::DOUBLE},
    {"time_max", ColumnType::DOUBLE},
    {"size_min", ColumnType::INT64},
    {"size_max", ColumnType::INT64},
    {"offset_min", ColumnType::INT64},
    {"offset_max", ColumnType::INT64},
    {"time_range", ColumnType::INT64},
    {"time_start", ColumnType::INT64},
    {"time_end", ColumnType::INT64},
};

enum GroupByField : std::uint32_t {
    GB_CAT = 1u << 0,
    GB_FUNC_NAME = 1u << 1,
    GB_PID = 1u << 2,
    GB_TID = 1u << 3,
    GB_FILE_HASH = 1u << 4,
    GB_HOST_HASH = 1u << 5,
    GB_FILE_NAME = 1u << 6,
    GB_HOST_NAME = 1u << 7,
    GB_PROC_NAME = 1u << 8,
    GB_IO_CAT = 1u << 9,
    GB_ACC_PAT = 1u << 10,
    GB_TIME_RANGE = 1u << 11,
};

struct GroupByConfig {
    std::uint32_t mask = 0;
    std::vector<GroupByField> order;
    std::vector<std::string> names;  // matches `order`, used for schema
};

inline std::optional<GroupByField> parse_group_by_name(std::string_view name) {
    if (name == "cat") return GB_CAT;
    if (name == "func_name") return GB_FUNC_NAME;
    if (name == "pid") return GB_PID;
    if (name == "tid") return GB_TID;
    if (name == "file_hash") return GB_FILE_HASH;
    if (name == "host_hash") return GB_HOST_HASH;
    if (name == "file_name") return GB_FILE_NAME;
    if (name == "host_name") return GB_HOST_NAME;
    if (name == "proc_name") return GB_PROC_NAME;
    if (name == "io_cat") return GB_IO_CAT;
    if (name == "acc_pat") return GB_ACC_PAT;
    if (name == "time_range") return GB_TIME_RANGE;
    return std::nullopt;
}

struct CoarseKey {
    std::string_view cat;
    std::string_view func_name;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::string_view file_hash;
    std::string_view host_hash;
    std::string_view file_name;
    std::string_view host_name;
    std::string_view proc_name;
    std::int64_t io_cat = 0;
    std::int64_t acc_pat = 0;
    std::int64_t time_range = 0;

    bool operator==(const CoarseKey& o) const {
        return cat == o.cat && func_name == o.func_name && pid == o.pid &&
               tid == o.tid && file_hash == o.file_hash &&
               host_hash == o.host_hash && file_name == o.file_name &&
               host_name == o.host_name && proc_name == o.proc_name &&
               io_cat == o.io_cat && acc_pat == o.acc_pat &&
               time_range == o.time_range;
    }
};

struct CoarseKeyHash {
    std::size_t operator()(const CoarseKey& k) const {
        auto combine = [](std::size_t h, std::size_t v) {
            return h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
        };
        std::size_t h = std::hash<std::string_view>{}(k.cat);
        h = combine(h, std::hash<std::string_view>{}(k.func_name));
        h = combine(h, std::hash<std::uint64_t>{}(k.pid));
        h = combine(h, std::hash<std::uint64_t>{}(k.tid));
        h = combine(h, std::hash<std::string_view>{}(k.file_hash));
        h = combine(h, std::hash<std::string_view>{}(k.host_hash));
        h = combine(h, std::hash<std::string_view>{}(k.file_name));
        h = combine(h, std::hash<std::string_view>{}(k.host_name));
        h = combine(h, std::hash<std::string_view>{}(k.proc_name));
        h = combine(h, std::hash<std::int64_t>{}(k.io_cat));
        h = combine(h, std::hash<std::int64_t>{}(k.acc_pat));
        h = combine(h, std::hash<std::int64_t>{}(k.time_range));
        return h;
    }
};

struct CoarseMetrics {
    std::uint64_t count = 0;
    double time_sum = 0.0;
    double time_sq_sum = 0.0;
    double time_min_val = std::numeric_limits<double>::infinity();
    double time_max_val = -std::numeric_limits<double>::infinity();
    double time_call_min_val = std::numeric_limits<double>::infinity();
    double time_call_max_val = -std::numeric_limits<double>::infinity();
    std::uint64_t size_sum = 0;
    double size_sq_sum = 0.0;
    std::uint64_t size_min_val = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t size_max_val = 0;
    std::uint64_t size_call_min_val = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t size_call_max_val = 0;
    bool has_size = false;
    std::uint64_t time_start_val = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t time_end_val = 0;
    bool has_time_bounds = false;
};

inline std::vector<ColumnSpec> make_coarse_schema(const GroupByConfig& cfg) {
    std::vector<ColumnSpec> specs;
    specs.reserve(cfg.order.size() + 16);
    for (std::size_t i = 0; i < cfg.order.size(); ++i) {
        GroupByField f = cfg.order[i];
        const std::string& name = cfg.names[i];
        switch (f) {
            case GB_CAT:
            case GB_FUNC_NAME:
            case GB_FILE_HASH:
            case GB_HOST_HASH:
            case GB_FILE_NAME:
            case GB_HOST_NAME:
            case GB_PROC_NAME:
                specs.push_back({name, ColumnType::DICT_STRING});
                break;
            case GB_PID:
            case GB_TID:
            case GB_IO_CAT:
            case GB_ACC_PAT:
            case GB_TIME_RANGE:
                specs.push_back({name, ColumnType::INT64});
                break;
        }
    }
    specs.push_back({"count", ColumnType::INT64});
    specs.push_back({"time", ColumnType::DOUBLE});
    specs.push_back({"size", ColumnType::INT64});
    specs.push_back({"time_sq", ColumnType::DOUBLE});
    specs.push_back({"size_sq", ColumnType::DOUBLE});
    specs.push_back({"time_min", ColumnType::DOUBLE});
    specs.push_back({"time_max", ColumnType::DOUBLE});
    specs.push_back({"size_min", ColumnType::INT64});
    specs.push_back({"size_max", ColumnType::INT64});
    specs.push_back({"time_call_min", ColumnType::DOUBLE});
    specs.push_back({"time_call_max", ColumnType::DOUBLE});
    specs.push_back({"size_call_min", ColumnType::INT64});
    specs.push_back({"size_call_max", ColumnType::INT64});
    specs.push_back({"time_start", ColumnType::INT64});
    specs.push_back({"time_end", ColumnType::INT64});
    return specs;
}

struct DfanalyzerScanInput {
    const EventAggregator* agg;
    const DfanalyzerContext* ctx;
    std::optional<AggMapType> type_filter;
    Py_ssize_t batch_size;
    std::uint16_t shard_begin;
    std::uint16_t shard_end;
    const GroupByConfig* group_by = nullptr;  // null = full granularity
};

struct DfanalyzerScanOutput {
    std::vector<ArrowExportResult> events;
    std::vector<ArrowExportResult> profiles;
    std::vector<ArrowExportResult> system;
};

DfanalyzerScanOutput scan_dfanalyzer_shards(DfanalyzerScanInput input) {
    DfanalyzerScanOutput output;

    const bool coarse = input.group_by != nullptr;
    const std::vector<ColumnSpec> coarse_schema =
        coarse ? make_coarse_schema(*input.group_by)
               : std::vector<ColumnSpec>{};

    auto make_builder = [&]() {
        RecordBatchBuilder b;
        if (coarse) {
            b.declare_schema(coarse_schema);
        } else {
            b.declare_schema(DFANALYZER_SCHEMA);
        }
        b.reserve(static_cast<std::size_t>(input.batch_size));
        return b;
    };

    RecordBatchBuilder event_builder, profile_builder, system_builder;
    bool use_events =
        !input.type_filter || *input.type_filter == AggMapType::EVENT;
    bool use_profiles =
        !input.type_filter || *input.type_filter == AggMapType::PROFILE;
    bool use_system =
        !input.type_filter || *input.type_filter == AggMapType::SYSTEM;

    if (use_events) event_builder = make_builder();
    if (use_profiles) profile_builder = make_builder();
    if (use_system) system_builder = make_builder();

    auto bucket_width_us = static_cast<std::uint64_t>(
        input.ctx->time_granularity * input.ctx->time_resolution);
    std::size_t event_count = 0, profile_count = 0, system_count = 0;

    HashResolver resolver(input.ctx->file_hashes, input.ctx->host_hashes);
    std::unordered_map<ProcKey, std::string, ProcKeyHash> proc_name_cache;
    std::unordered_map<std::string_view, IOCategory> io_cat_cache;

    std::unordered_map<CoarseKey, CoarseMetrics, CoarseKeyHash> event_coarse,
        profile_coarse, system_coarse;

    auto flush_builder = [&](RecordBatchBuilder& builder, std::size_t& count,
                             std::vector<ArrowExportResult>& results) {
        if (count > 0) {
            auto arrow = builder.finish();
            if (arrow.valid()) {
                results.push_back(std::move(arrow));
            }
            builder.reset(true);
            builder.reserve(static_cast<std::size_t>(input.batch_size));
            count = 0;
        }
    };

    auto append_row = [&](RecordBatchBuilder& builder, std::size_t& count,
                          std::vector<ArrowExportResult>& results,
                          const AggKeyView& kv, const AggMetricsView& mv,
                          std::string_view file_name,
                          std::string_view host_name,
                          std::string_view proc_name, IOCategory io_cat) {
        std::size_t ci = 0;
        builder.append_dict_string(ci++, kv.cat);
        builder.append_dict_string(ci++, kv.name);
        builder.append_int64(ci++, static_cast<std::int64_t>(kv.pid));
        builder.append_int64(ci++, static_cast<std::int64_t>(kv.tid));
        builder.append_dict_string(ci++, kv.fhash);
        builder.append_dict_string(ci++, kv.hhash);
        builder.append_dict_string(ci++, file_name);
        builder.append_dict_string(ci++, host_name);
        builder.append_dict_string(ci++, proc_name);
        builder.append_int64(ci++, static_cast<std::int64_t>(io_cat));
        builder.append_int64(ci++, 0);

        builder.append_int64(ci++, static_cast<std::int64_t>(mv.count));
        builder.append_double(ci++, static_cast<double>(mv.dur_total) /
                                        input.ctx->time_resolution);

        if (mv.size_total > 0) {
            builder.append_int64(ci++,
                                 static_cast<std::int64_t>(mv.size_total));
        } else {
            builder.append_null(ci++);
        }

        builder.append_double(ci++, mv.count > 0
                                        ? static_cast<double>(mv.dur_min) /
                                              input.ctx->time_resolution
                                        : 0.0);
        builder.append_double(ci++, mv.count > 0
                                        ? static_cast<double>(mv.dur_max) /
                                              input.ctx->time_resolution
                                        : 0.0);

        if (mv.size_total > 0 && mv.count > 0) {
            builder.append_int64(ci++, static_cast<std::int64_t>(mv.size_min));
            builder.append_int64(ci++, static_cast<std::int64_t>(mv.size_max));
        } else {
            builder.append_null(ci++);
            builder.append_null(ci++);
        }

        // offset_min > offset_max only when no offset was ever recorded
        // (MetricStats default min=UINT64_MAX, max=0); 0 is a valid offset.
        if (mv.offset_min <= mv.offset_max) {
            builder.append_int64(ci++,
                                 static_cast<std::int64_t>(mv.offset_min));
            builder.append_int64(ci++,
                                 static_cast<std::int64_t>(mv.offset_max));
        } else {
            builder.append_null(ci++);
            builder.append_null(ci++);
        }

        auto time_range = bucket_width_us > 0
                              ? static_cast<std::int64_t>(
                                    (kv.time_bucket - input.ctx->time_origin) /
                                    bucket_width_us)
                              : 0;
        builder.append_int64(ci++, time_range);
        // Counter (profile) rows align to the bucket grid: time_start is the
        // bucket start, time_end one bucket later. Plain events keep the
        // precise min/max event timestamps.
        if (kv.map_type == AggMapType::PROFILE) {
            auto bucket_start = static_cast<std::int64_t>(
                kv.time_bucket - input.ctx->time_origin);
            builder.append_int64(ci++, bucket_start);
            builder.append_int64(ci++, bucket_start + static_cast<std::int64_t>(
                                                          bucket_width_us));
        } else {
            builder.append_int64(ci++, static_cast<std::int64_t>(
                                           mv.ts - input.ctx->time_origin));
            builder.append_int64(ci++, static_cast<std::int64_t>(
                                           mv.te - input.ctx->time_origin));
        }
        builder.end_row();

        count++;
        if (static_cast<Py_ssize_t>(count) >= input.batch_size) {
            flush_builder(builder, count, results);
        }
    };

    auto accumulate_coarse =
        [&](std::unordered_map<CoarseKey, CoarseMetrics, CoarseKeyHash>& map,
            const AggKeyView& kv, const AggMetricsView& mv,
            std::string_view file_name, std::string_view host_name,
            std::string_view proc_name, IOCategory io_cat) {
            const auto& cfg = *input.group_by;
            // Probe with non-interned views; hash/equality compare by content,
            // so string_view lifetime doesn't matter for lookup. We only copy
            // (intern) on first insert.
            CoarseKey probe;
            if (cfg.mask & GB_CAT) probe.cat = kv.cat;
            if (cfg.mask & GB_FUNC_NAME) probe.func_name = kv.name;
            if (cfg.mask & GB_PID) probe.pid = kv.pid;
            if (cfg.mask & GB_TID) probe.tid = kv.tid;
            if (cfg.mask & GB_FILE_HASH) probe.file_hash = kv.fhash;
            if (cfg.mask & GB_HOST_HASH) probe.host_hash = kv.hhash;
            if (cfg.mask & GB_FILE_NAME) probe.file_name = file_name;
            if (cfg.mask & GB_HOST_NAME) probe.host_name = host_name;
            if (cfg.mask & GB_PROC_NAME) probe.proc_name = proc_name;
            if (cfg.mask & GB_IO_CAT)
                probe.io_cat = static_cast<std::int64_t>(io_cat);
            if (cfg.mask & GB_TIME_RANGE) {
                probe.time_range =
                    bucket_width_us > 0
                        ? static_cast<std::int64_t>(
                              (kv.time_bucket - input.ctx->time_origin) /
                              bucket_width_us)
                        : 0;
            }
            // acc_pat is always 0 today; included for completeness.

            auto it = map.find(probe);
            if (it == map.end()) {
                // First sighting: promote views referencing unstable DB buffers
                // to interned copies. file_name/host_name come from the
                // resolver's intern pool, and proc_name from proc_name_cache;
                // both already stable across iterations, no copy needed.
                CoarseKey stable = probe;
                if (cfg.mask & GB_CAT) stable.cat = resolver.intern(kv.cat);
                if (cfg.mask & GB_FUNC_NAME)
                    stable.func_name = resolver.intern(kv.name);
                if (cfg.mask & GB_FILE_HASH)
                    stable.file_hash = resolver.intern(kv.fhash);
                if (cfg.mask & GB_HOST_HASH)
                    stable.host_hash = resolver.intern(kv.hhash);
                auto [nit, _] = map.emplace(std::move(stable), CoarseMetrics{});
                it = nit;
            }
            CoarseMetrics& m = it->second;
            m.count += mv.count;
            double time_val =
                static_cast<double>(mv.dur_total) / input.ctx->time_resolution;
            m.time_sum += time_val;
            m.time_sq_sum += time_val * time_val;
            if (time_val < m.time_call_min_val) m.time_call_min_val = time_val;
            if (time_val > m.time_call_max_val) m.time_call_max_val = time_val;
            if (mv.count > 0) {
                double dur_min_v = static_cast<double>(mv.dur_min) /
                                   input.ctx->time_resolution;
                double dur_max_v = static_cast<double>(mv.dur_max) /
                                   input.ctx->time_resolution;
                if (dur_min_v < m.time_min_val) m.time_min_val = dur_min_v;
                if (dur_max_v > m.time_max_val) m.time_max_val = dur_max_v;
            }
            if (mv.size_total > 0) {
                m.has_size = true;
                m.size_sum += mv.size_total;
                double sz = static_cast<double>(mv.size_total);
                m.size_sq_sum += sz * sz;
                if (mv.size_total < m.size_call_min_val)
                    m.size_call_min_val = mv.size_total;
                if (mv.size_total > m.size_call_max_val)
                    m.size_call_max_val = mv.size_total;
                if (mv.count > 0) {
                    if (mv.size_min < m.size_min_val)
                        m.size_min_val = mv.size_min;
                    if (mv.size_max > m.size_max_val)
                        m.size_max_val = mv.size_max;
                }
            }
            if (mv.ts >= input.ctx->time_origin) {
                m.has_time_bounds = true;
                auto ts_off = mv.ts - input.ctx->time_origin;
                auto te_off = mv.te - input.ctx->time_origin;
                if (ts_off < m.time_start_val) m.time_start_val = ts_off;
                if (te_off > m.time_end_val) m.time_end_val = te_off;
            }
        };

    input.agg->scan_shard_range_raw(
        input.shard_begin, input.shard_end,
        [&](std::string_view key_bytes, std::string_view val_bytes) -> bool {
            AggKeyView kv;
            if (!parse_agg_key_view(key_bytes, kv)) return true;

            if (input.type_filter && kv.map_type != *input.type_filter)
                return true;

            if (input.ctx->query_filter) {
                auto& q = *input.ctx->query_filter;
                dftracer::utils::utilities::common::query::ValueMap fields;
                if (q.references("cat")) fields["cat"] = std::string(kv.cat);
                if (q.references("name")) fields["name"] = std::string(kv.name);
                if (q.references("pid")) fields["pid"] = kv.pid;
                if (q.references("tid")) fields["tid"] = kv.tid;
                if (q.references("hhash"))
                    fields["hhash"] = std::string(kv.hhash);
                if (q.references("fhash"))
                    fields["fhash"] = std::string(kv.fhash);
                if (q.references("time_bucket"))
                    fields["time_bucket"] = kv.time_bucket;
                if (!q.evaluate(fields)) return true;
            }

            AggMetricsView mv;
            if (!parse_agg_value_view(val_bytes, mv)) return true;

            auto file_name = resolver.resolve_file(kv.fhash);
            auto host_name = resolver.resolve_host(kv.hhash);

            ProcKey pk{kv.hhash, kv.pid, kv.tid};
            auto proc_it = proc_name_cache.find(pk);
            std::string_view proc_name;
            if (proc_it != proc_name_cache.end()) {
                proc_name = proc_it->second;
            } else {
                std::string pn = "app#";
                if (!host_name.empty()) {
                    pn.append(host_name);
                } else if (!kv.hhash.empty()) {
                    pn.append(kv.hhash);
                } else {
                    pn.append("unknown");
                }
                pn.push_back('#');
                pn.append(std::to_string(kv.pid));
                pn.push_back('#');
                pn.append(std::to_string(kv.tid));
                ProcKey stable_pk{resolver.intern(kv.hhash), kv.pid, kv.tid};
                auto [it, _] =
                    proc_name_cache.emplace(stable_pk, std::move(pn));
                proc_name = it->second;
            }

            auto io_it = io_cat_cache.find(kv.name);
            IOCategory io_cat;
            if (io_it != io_cat_cache.end()) {
                io_cat = io_it->second;
            } else {
                io_cat = get_io_category(kv.name);
                io_cat_cache[resolver.intern(kv.name)] = io_cat;
            }

            if (coarse) {
                switch (kv.map_type) {
                    case AggMapType::EVENT:
                        if (use_events)
                            accumulate_coarse(event_coarse, kv, mv, file_name,
                                              host_name, proc_name, io_cat);
                        break;
                    case AggMapType::PROFILE:
                        if (use_profiles)
                            accumulate_coarse(profile_coarse, kv, mv, file_name,
                                              host_name, proc_name, io_cat);
                        break;
                    case AggMapType::SYSTEM:
                        if (use_system)
                            accumulate_coarse(system_coarse, kv, mv, file_name,
                                              host_name, proc_name, io_cat);
                        break;
                }
            } else {
                switch (kv.map_type) {
                    case AggMapType::EVENT:
                        append_row(event_builder, event_count, output.events,
                                   kv, mv, file_name, host_name, proc_name,
                                   io_cat);
                        break;
                    case AggMapType::PROFILE:
                        append_row(profile_builder, profile_count,
                                   output.profiles, kv, mv, file_name,
                                   host_name, proc_name, io_cat);
                        break;
                    case AggMapType::SYSTEM:
                        append_row(system_builder, system_count, output.system,
                                   kv, mv, file_name, host_name, proc_name,
                                   io_cat);
                        break;
                }
            }
            return true;
        });

    if (coarse) {
        const auto& cfg = *input.group_by;
        auto flush_coarse = [&](std::unordered_map<CoarseKey, CoarseMetrics,
                                                   CoarseKeyHash>& map,
                                RecordBatchBuilder& builder, std::size_t& count,
                                std::vector<ArrowExportResult>& results) {
            for (auto& [key, m] : map) {
                std::size_t ci = 0;
                for (std::size_t i = 0; i < cfg.order.size(); ++i) {
                    switch (cfg.order[i]) {
                        case GB_CAT:
                            builder.append_dict_string(ci++, key.cat);
                            break;
                        case GB_FUNC_NAME:
                            builder.append_dict_string(ci++, key.func_name);
                            break;
                        case GB_PID:
                            builder.append_int64(
                                ci++, static_cast<std::int64_t>(key.pid));
                            break;
                        case GB_TID:
                            builder.append_int64(
                                ci++, static_cast<std::int64_t>(key.tid));
                            break;
                        case GB_FILE_HASH:
                            builder.append_dict_string(ci++, key.file_hash);
                            break;
                        case GB_HOST_HASH:
                            builder.append_dict_string(ci++, key.host_hash);
                            break;
                        case GB_FILE_NAME:
                            builder.append_dict_string(ci++, key.file_name);
                            break;
                        case GB_HOST_NAME:
                            builder.append_dict_string(ci++, key.host_name);
                            break;
                        case GB_PROC_NAME:
                            builder.append_dict_string(ci++, key.proc_name);
                            break;
                        case GB_IO_CAT:
                            builder.append_int64(ci++, key.io_cat);
                            break;
                        case GB_ACC_PAT:
                            builder.append_int64(ci++, key.acc_pat);
                            break;
                        case GB_TIME_RANGE:
                            builder.append_int64(ci++, key.time_range);
                            break;
                    }
                }
                builder.append_int64(ci++, static_cast<std::int64_t>(m.count));
                builder.append_double(ci++, m.time_sum);
                if (m.has_size) {
                    builder.append_int64(ci++,
                                         static_cast<std::int64_t>(m.size_sum));
                } else {
                    builder.append_null(ci++);
                }
                builder.append_double(ci++, m.time_sq_sum);
                if (m.has_size) {
                    builder.append_double(ci++, m.size_sq_sum);
                } else {
                    builder.append_null(ci++);
                }
                builder.append_double(ci++, m.count > 0 ? m.time_min_val : 0.0);
                builder.append_double(ci++, m.count > 0 ? m.time_max_val : 0.0);
                if (m.has_size) {
                    builder.append_int64(
                        ci++, static_cast<std::int64_t>(m.size_min_val));
                    builder.append_int64(
                        ci++, static_cast<std::int64_t>(m.size_max_val));
                } else {
                    builder.append_null(ci++);
                    builder.append_null(ci++);
                }
                builder.append_double(ci++,
                                      m.count > 0 ? m.time_call_min_val : 0.0);
                builder.append_double(ci++,
                                      m.count > 0 ? m.time_call_max_val : 0.0);
                if (m.has_size) {
                    builder.append_int64(
                        ci++, static_cast<std::int64_t>(m.size_call_min_val));
                    builder.append_int64(
                        ci++, static_cast<std::int64_t>(m.size_call_max_val));
                } else {
                    builder.append_null(ci++);
                    builder.append_null(ci++);
                }
                builder.append_int64(
                    ci++, m.has_time_bounds
                              ? static_cast<std::int64_t>(m.time_start_val)
                              : 0);
                builder.append_int64(
                    ci++, m.has_time_bounds
                              ? static_cast<std::int64_t>(m.time_end_val)
                              : 0);
                builder.end_row();
                ++count;
                if (static_cast<Py_ssize_t>(count) >= input.batch_size) {
                    flush_builder(builder, count, results);
                }
            }
            flush_builder(builder, count, results);
        };
        if (use_events)
            flush_coarse(event_coarse, event_builder, event_count,
                         output.events);
        if (use_profiles)
            flush_coarse(profile_coarse, profile_builder, profile_count,
                         output.profiles);
        if (use_system)
            flush_coarse(system_coarse, system_builder, system_count,
                         output.system);
    } else {
        if (use_events)
            flush_builder(event_builder, event_count, output.events);
        if (use_profiles)
            flush_builder(profile_builder, profile_count, output.profiles);
        if (use_system)
            flush_builder(system_builder, system_count, output.system);
    }

    return output;
}

// Two-pass scan over SYSTEM_METRICS CF: pass 1 discovers metric column names
// (dynamic per workload), pass 2 emits rows. Needed because RecordBatchBuilder
// requires the schema up front.
std::vector<ArrowExportResult> scan_system_metrics_buffer(
    const EventAggregator* agg, const DfanalyzerContext* ctx,
    Py_ssize_t batch_size) {
    std::vector<ArrowExportResult> results;
    if (!agg) return results;

    std::vector<std::string> metric_names_ordered;
    std::unordered_set<std::string> metric_name_seen;
    agg->scan_system_metrics_raw(
        [&](std::string_view, std::string_view val_bytes) -> bool {
            auto m = deserialize_system_value(val_bytes);
            if (m.metrics) {
                for (const auto& [name, _] : *m.metrics) {
                    if (metric_name_seen.insert(name).second) {
                        metric_names_ordered.push_back(name);
                    }
                }
            }
            return true;
        });

    if (metric_names_ordered.empty()) return results;

    // SystemAggregationMetrics::metrics is an unordered_map; sort the
    // discovered column names so the emitted Arrow schema is deterministic
    // across runs and builds.
    std::sort(metric_names_ordered.begin(), metric_names_ordered.end());

    std::vector<ColumnSpec> schema;
    schema.reserve(6 + metric_names_ordered.size());
    schema.push_back({"host_hash", ColumnType::DICT_STRING});
    schema.push_back({"name", ColumnType::DICT_STRING});
    schema.push_back({"time_bucket", ColumnType::INT64});
    schema.push_back({"ts", ColumnType::INT64});
    schema.push_back({"te", ColumnType::INT64});
    schema.push_back({"count", ColumnType::INT64});
    for (const auto& mn : metric_names_ordered) {
        schema.push_back({mn, ColumnType::DOUBLE});
    }

    RecordBatchBuilder builder;
    builder.declare_schema(schema);
    builder.reserve(static_cast<std::size_t>(batch_size));

    auto flush = [&](std::size_t& row_count) {
        if (row_count == 0) return;
        auto arrow = builder.finish();
        if (arrow.valid()) results.push_back(std::move(arrow));
        builder.reset(true);
        builder.reserve(static_cast<std::size_t>(batch_size));
        row_count = 0;
    };

    std::size_t row_count = 0;
    const std::size_t n_metric_cols = metric_names_ordered.size();

    agg->scan_system_metrics_raw(
        [&](std::string_view key_bytes, std::string_view val_bytes) -> bool {
            auto k = deserialize_system_key(key_bytes);
            auto m = deserialize_system_value(val_bytes);

            std::size_t ci = 0;
            builder.append_dict_string(ci++, k.key.hhash);
            builder.append_dict_string(ci++, k.key.name);
            builder.append_int64(ci++,
                                 static_cast<std::int64_t>(k.key.time_bucket));
            builder.append_int64(ci++, static_cast<std::int64_t>(m.ts));
            builder.append_int64(ci++, static_cast<std::int64_t>(m.te));
            builder.append_int64(ci++, static_cast<std::int64_t>(m.count));

            for (std::size_t i = 0; i < n_metric_cols; ++i) {
                const auto& mn = metric_names_ordered[i];
                bool present = false;
                if (m.metrics) {
                    auto it = m.metrics->find(mn);
                    if (it != m.metrics->end()) {
                        builder.append_double(ci++, it->second.mean);
                        present = true;
                    }
                }
                if (!present) builder.append_null(ci++);
            }
            builder.end_row();
            row_count++;
            if (static_cast<Py_ssize_t>(row_count) >= batch_size) {
                flush(row_count);
            }
            return true;
        });
    flush(row_count);

    (void)ctx;
    return results;
}

}  // namespace

static PyObject* Indexer_iter_aggregation(IndexerObject* self, PyObject* args,
                                          PyObject* kwds) {
    static const char* kwlist[] = {"type", "batch_size", nullptr};
    const char* type_str = "events";
    Py_ssize_t batch_size = 10000;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|sn", (char**)kwlist,
                                     &type_str, &batch_size)) {
        return nullptr;
    }

    AggMapType target_type;
    if (!parse_agg_type_str(type_str, target_type)) return nullptr;

    AggregationBatchType batch_type;
    if (target_type == AggMapType::EVENT)
        batch_type = AggregationBatchType::EVENT;
    else if (target_type == AggMapType::PROFILE)
        batch_type = AggregationBatchType::PROFILE;
    else
        batch_type = AggregationBatchType::SYSTEM;

    auto idx_opt = resolve_index_path(self);
    if (!idx_opt) return nullptr;
    std::string index_path = std::move(*idx_opt);

    PyObject* batch_list = PyList_New(0);
    if (!batch_list) return nullptr;

    std::string error_msg;
    std::vector<dftracer::utils::utilities::common::arrow::ArrowExportResult>
        results;

    Py_BEGIN_ALLOW_THREADS try {
        auto handle = open_agg_db(index_path, error_msg);
        if (handle) {
            Runtime* rt = get_batch_indexer_runtime(self);
            std::vector<AggScanOutput> outputs;
            parallel_shard_scan<AggScanOutput>(
                rt,
                [&](std::uint16_t shard_begin, std::uint16_t shard_end) {
                    AggScanInput input;
                    input.agg = handle->agg.get();
                    input.target_type = target_type;
                    input.batch_type = batch_type;
                    input.batch_size = batch_size;
                    input.shard_begin = shard_begin;
                    input.shard_end = shard_end;
                    return scan_aggregation_shard_range(input);
                },
                outputs);

            for (auto& out : outputs) {
                for (auto& r : out.results) {
                    results.push_back(std::move(r));
                }
            }
        }
    } catch (const std::exception& e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        Py_DECREF(batch_list);
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return nullptr;
    }

    append_results_to_list(batch_list, results);

    PyObject* iter = PyObject_GetIter(batch_list);
    Py_DECREF(batch_list);
    return iter;
}

static PyObject* Indexer_iter_arrow_dfanalyzer(IndexerObject* self,
                                               PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {
        "type",  "batch_size", "time_granularity", "time_resolution",
        "query", nullptr};
    const char* type_str = "events";
    Py_ssize_t batch_size = 10000;
    double time_granularity = 1.0;
    double time_resolution = 1000000.0;
    const char* query_str = nullptr;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|snddz", (char**)kwlist,
                                     &type_str, &batch_size, &time_granularity,
                                     &time_resolution, &query_str)) {
        return nullptr;
    }

    AggMapType target_type;
    if (!parse_agg_type_str(type_str, target_type)) return nullptr;

    auto query_opt = parse_query_arg(query_str);
    if (!query_opt && PyErr_Occurred()) return nullptr;

    auto idx_opt = resolve_index_path(self);
    if (!idx_opt) return nullptr;
    std::string index_path = std::move(*idx_opt);

    PyObject* batch_list = PyList_New(0);
    if (!batch_list) return nullptr;

    std::string error_msg;
    std::vector<ArrowExportResult> results;

    Py_BEGIN_ALLOW_THREADS try {
        auto handle = open_agg_db(index_path, error_msg);
        if (handle) {
            dftracer::utils::utilities::indexer::IndexDatabase idx_db(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
            auto file_hashes =
                idx_db.query_hash_table(dftracer::utils::utilities::indexer::
                                            IndexDatabase::HashType::FILE);
            auto host_hashes =
                idx_db.query_hash_table(dftracer::utils::utilities::indexer::
                                            IndexDatabase::HashType::HOST);

            auto time_bounds = handle->agg->query_time_bounds();
            std::uint64_t time_origin =
                time_bounds.valid ? time_bounds.min_time_bucket : 0;

            DfanalyzerContext ctx;
            ctx.file_hashes = &file_hashes;
            ctx.host_hashes = &host_hashes;
            ctx.query_filter = query_opt ? &*query_opt : nullptr;
            ctx.time_origin = time_origin;
            ctx.time_resolution = time_resolution;
            ctx.time_granularity = time_granularity;

            Runtime* rt = get_batch_indexer_runtime(self);
            std::vector<DfanalyzerScanOutput> outputs;
            parallel_shard_scan<DfanalyzerScanOutput>(
                rt,
                [&](std::uint16_t shard_begin, std::uint16_t shard_end) {
                    DfanalyzerScanInput input;
                    input.agg = handle->agg.get();
                    input.ctx = &ctx;
                    input.type_filter = target_type;
                    input.batch_size = batch_size;
                    input.shard_begin = shard_begin;
                    input.shard_end = shard_end;
                    return scan_dfanalyzer_shards(input);
                },
                outputs);

            for (auto& out : outputs) {
                for (auto& r : out.events) results.push_back(std::move(r));
                for (auto& r : out.profiles) results.push_back(std::move(r));
                for (auto& r : out.system) results.push_back(std::move(r));
            }
        }
    } catch (const std::exception& e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        Py_DECREF(batch_list);
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return nullptr;
    }

    append_results_to_list(batch_list, results);

    PyObject* iter = PyObject_GetIter(batch_list);
    Py_DECREF(batch_list);
    return iter;
}

static bool parse_group_by_arg(PyObject* obj, GroupByConfig& out) {
    if (!obj || obj == Py_None) return true;
    if (!PySequence_Check(obj)) {
        PyErr_SetString(PyExc_TypeError,
                        "group_by must be a sequence of strings or None");
        return false;
    }
    Py_ssize_t n = PySequence_Length(obj);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* item = PySequence_GetItem(obj, i);
        if (!item) return false;
        if (!PyUnicode_Check(item)) {
            Py_DECREF(item);
            PyErr_SetString(PyExc_TypeError,
                            "group_by entries must be strings");
            return false;
        }
        Py_ssize_t sz = 0;
        const char* s = PyUnicode_AsUTF8AndSize(item, &sz);
        if (!s) {
            Py_DECREF(item);
            return false;
        }
        std::string_view sv(s, static_cast<std::size_t>(sz));
        auto field = parse_group_by_name(sv);
        if (!field) {
            std::string msg = "unsupported group_by field: ";
            msg.append(sv);
            Py_DECREF(item);
            PyErr_SetString(PyExc_ValueError, msg.c_str());
            return false;
        }
        if (!(out.mask & *field)) {
            out.mask |= *field;
            out.order.push_back(*field);
            out.names.emplace_back(sv);
        }
        Py_DECREF(item);
    }
    return true;
}

static PyObject* Indexer_iter_arrow_dfanalyzer_all(IndexerObject* self,
                                                   PyObject* args,
                                                   PyObject* kwds) {
    static const char* kwlist[] = {"batch_size",      "time_granularity",
                                   "time_resolution", "query",
                                   "group_by",        nullptr};
    Py_ssize_t batch_size = 10000;
    double time_granularity = 1.0;
    double time_resolution = 1000000.0;
    const char* query_str = nullptr;
    PyObject* group_by_obj = nullptr;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|nddzO", (char**)kwlist, &batch_size,
            &time_granularity, &time_resolution, &query_str, &group_by_obj)) {
        return nullptr;
    }

    auto query_opt = parse_query_arg(query_str);
    if (!query_opt && PyErr_Occurred()) return nullptr;

    GroupByConfig group_by_cfg;
    if (!parse_group_by_arg(group_by_obj, group_by_cfg)) return nullptr;
    const GroupByConfig* group_by_ptr =
        group_by_cfg.mask != 0 ? &group_by_cfg : nullptr;

    auto idx_opt = resolve_index_path(self);
    if (!idx_opt) return nullptr;
    std::string index_path = std::move(*idx_opt);

    PyObject* result_dict = PyDict_New();
    if (!result_dict) return nullptr;

    PyObject* events_list = PyList_New(0);
    PyObject* profiles_list = PyList_New(0);
    PyObject* system_list = PyList_New(0);
    if (!events_list || !profiles_list || !system_list) {
        Py_XDECREF(events_list);
        Py_XDECREF(profiles_list);
        Py_XDECREF(system_list);
        Py_DECREF(result_dict);
        return nullptr;
    }

    std::string error_msg;
    std::vector<ArrowExportResult> events_results, profiles_results,
        system_results;

    Py_BEGIN_ALLOW_THREADS try {
        auto handle = open_agg_db(index_path, error_msg);
        if (handle) {
            dftracer::utils::utilities::indexer::IndexDatabase idx_db(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
            auto file_hashes =
                idx_db.query_hash_table(dftracer::utils::utilities::indexer::
                                            IndexDatabase::HashType::FILE);
            auto host_hashes =
                idx_db.query_hash_table(dftracer::utils::utilities::indexer::
                                            IndexDatabase::HashType::HOST);

            auto time_bounds = handle->agg->query_time_bounds();
            std::uint64_t time_origin =
                time_bounds.valid ? time_bounds.min_time_bucket : 0;

            DfanalyzerContext ctx;
            ctx.file_hashes = &file_hashes;
            ctx.host_hashes = &host_hashes;
            ctx.query_filter = query_opt ? &*query_opt : nullptr;
            ctx.time_origin = time_origin;
            ctx.time_resolution = time_resolution;
            ctx.time_granularity = time_granularity;

            Runtime* rt = get_batch_indexer_runtime(self);
            std::vector<DfanalyzerScanOutput> outputs;
            parallel_shard_scan<DfanalyzerScanOutput>(
                rt,
                [&](std::uint16_t shard_begin, std::uint16_t shard_end) {
                    DfanalyzerScanInput input;
                    input.agg = handle->agg.get();
                    input.ctx = &ctx;
                    input.type_filter = std::nullopt;
                    input.batch_size = batch_size;
                    input.shard_begin = shard_begin;
                    input.shard_end = shard_end;
                    input.group_by = group_by_ptr;
                    return scan_dfanalyzer_shards(input);
                },
                outputs);

            for (auto& out : outputs) {
                for (auto& r : out.events)
                    events_results.push_back(std::move(r));
                for (auto& r : out.profiles)
                    profiles_results.push_back(std::move(r));
                for (auto& r : out.system)
                    system_results.push_back(std::move(r));
            }

            auto sys_buf =
                scan_system_metrics_buffer(handle->agg.get(), &ctx, batch_size);
            for (auto& r : sys_buf) system_results.push_back(std::move(r));
        }
    } catch (const std::exception& e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        Py_DECREF(events_list);
        Py_DECREF(profiles_list);
        Py_DECREF(system_list);
        Py_DECREF(result_dict);
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return nullptr;
    }

    append_results_to_list(events_list, events_results);
    append_results_to_list(profiles_list, profiles_results);
    append_results_to_list(system_list, system_results);

    PyDict_SetItemString(result_dict, "events", events_list);
    PyDict_SetItemString(result_dict, "profiles", profiles_list);
    PyDict_SetItemString(result_dict, "system", system_list);
    Py_DECREF(events_list);
    Py_DECREF(profiles_list);
    Py_DECREF(system_list);

    return result_dict;
}

// ---------------------------------------------------------------------------
// scan_aggregation_manifest — module-level entry point for analyze_trace.
//
// Each Dask worker calls this with its slice of the agg manifest
// (agg_ssts + sys_ssts) and optionally a [shard_begin, shard_end) range.
// The function opens a scratch IndexDatabase at `scratch_dir`, ingests the
// SSTs into its AGGREGATION/SYSTEM_METRICS CFs (nearly free when SSTs live
// on the same filesystem as `scratch_dir` — RocksDB hard-links them), then
// runs the same parallel shard scan that `iter_arrow_dfanalyzer_all` uses.
//
// AGG_GLOBAL_CONFIG_KEY is not written by worker SSTs, so we construct the
// EventAggregator with config_hash=0 directly instead of going through
// `open_agg_db` (which requires the config key). The config hash is used
// by the aggregator only for write-time validation, not for reads.
//
// The scratch DB is NOT cleaned up here — the Python caller owns
// `scratch_dir` lifetime and should remove it after gathering results.
// ---------------------------------------------------------------------------

static bool collect_string_list(PyObject* obj, const char* name,
                                std::vector<std::string>& out) {
    if (!obj || obj == Py_None) return true;
    PyObject* seq = PySequence_Fast(obj, name);
    if (!seq) return false;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    out.reserve(static_cast<std::size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
        if (!PyUnicode_Check(item)) {
            Py_DECREF(seq);
            PyErr_Format(PyExc_TypeError, "%s items must be str", name);
            return false;
        }
        const char* s = PyUnicode_AsUTF8(item);
        if (!s) {
            Py_DECREF(seq);
            return false;
        }
        out.emplace_back(s);
    }
    Py_DECREF(seq);
    return true;
}

static bool collect_string_string_dict(
    PyObject* obj, const char* name,
    std::unordered_map<std::string, std::string>& out) {
    if (!obj || obj == Py_None) return true;
    if (!PyDict_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "%s must be a dict[str, str] or None",
                     name);
        return false;
    }
    PyObject *k, *v;
    Py_ssize_t pos = 0;
    while (PyDict_Next(obj, &pos, &k, &v)) {
        if (!PyUnicode_Check(k) || !PyUnicode_Check(v)) {
            PyErr_Format(PyExc_TypeError, "%s must map str -> str", name);
            return false;
        }
        const char* ks = PyUnicode_AsUTF8(k);
        const char* vs = PyUnicode_AsUTF8(v);
        if (!ks || !vs) return false;
        out.emplace(ks, vs);
    }
    return true;
}

static PyObject* scan_aggregation_manifest_fn(PyObject* /*self*/,
                                              PyObject* args, PyObject* kwds) {
    static const char* kwlist[] = {
        "agg_ssts",        "sys_ssts",    "scratch_dir",
        "meta_index_path", "batch_size",  "time_granularity",
        "time_resolution", "query",       "group_by",
        "shard_begin",     "shard_end",   "runtime",
        "file_hashes",     "host_hashes", nullptr};

    PyObject* agg_ssts_obj = nullptr;
    PyObject* sys_ssts_obj = nullptr;
    const char* scratch_dir = nullptr;
    const char* meta_index_path = nullptr;
    Py_ssize_t batch_size = 10000;
    double time_granularity = 1.0;
    double time_resolution = 1000000.0;
    const char* query_str = nullptr;
    PyObject* group_by_obj = nullptr;
    int shard_begin_i = 0;
    int shard_end_i = DFT_NUM_SHARDS;
    PyObject* runtime_obj = nullptr;
    PyObject* file_hashes_obj = nullptr;
    PyObject* host_hashes_obj = nullptr;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "OOss|nddzOiiOOO", (char**)kwlist, &agg_ssts_obj,
            &sys_ssts_obj, &scratch_dir, &meta_index_path, &batch_size,
            &time_granularity, &time_resolution, &query_str, &group_by_obj,
            &shard_begin_i, &shard_end_i, &runtime_obj, &file_hashes_obj,
            &host_hashes_obj)) {
        return nullptr;
    }

    if (shard_begin_i < 0 || shard_end_i > DFT_NUM_SHARDS ||
        shard_begin_i >= shard_end_i) {
        PyErr_Format(PyExc_ValueError,
                     "shard range [%d, %d) invalid (must be within [0, %d))",
                     shard_begin_i, shard_end_i, (int)DFT_NUM_SHARDS);
        return nullptr;
    }

    std::vector<std::string> agg_ssts;
    std::vector<std::string> sys_ssts;
    if (!collect_string_list(agg_ssts_obj, "agg_ssts", agg_ssts))
        return nullptr;
    if (!collect_string_list(sys_ssts_obj, "sys_ssts", sys_ssts))
        return nullptr;

    std::unordered_map<std::string, std::string> preloaded_file_hashes;
    std::unordered_map<std::string, std::string> preloaded_host_hashes;
    const bool hashes_preloaded =
        (file_hashes_obj && file_hashes_obj != Py_None) ||
        (host_hashes_obj && host_hashes_obj != Py_None);
    if (!collect_string_string_dict(file_hashes_obj, "file_hashes",
                                    preloaded_file_hashes))
        return nullptr;
    if (!collect_string_string_dict(host_hashes_obj, "host_hashes",
                                    preloaded_host_hashes))
        return nullptr;

    auto query_opt = parse_query_arg(query_str);
    if (!query_opt && PyErr_Occurred()) return nullptr;

    GroupByConfig group_by_cfg;
    if (!parse_group_by_arg(group_by_obj, group_by_cfg)) return nullptr;
    const GroupByConfig* group_by_ptr =
        group_by_cfg.mask != 0 ? &group_by_cfg : nullptr;

    Runtime* rt = nullptr;
    if (runtime_obj && runtime_obj != Py_None) {
        if (!PyObject_TypeCheck(runtime_obj, &RuntimeType)) {
            PyErr_SetString(PyExc_TypeError,
                            "runtime must be a Runtime instance or None");
            return nullptr;
        }
        rt = ((RuntimeObject*)runtime_obj)->runtime.get();
    } else {
        rt = get_default_runtime();
    }

    PyObject* result_dict = PyDict_New();
    if (!result_dict) return nullptr;
    PyObject* events_list = PyList_New(0);
    PyObject* profiles_list = PyList_New(0);
    PyObject* system_list = PyList_New(0);
    if (!events_list || !profiles_list || !system_list) {
        Py_XDECREF(events_list);
        Py_XDECREF(profiles_list);
        Py_XDECREF(system_list);
        Py_DECREF(result_dict);
        return nullptr;
    }

    std::string error_msg;
    std::vector<ArrowExportResult> events_results, profiles_results,
        system_results;
    std::string scratch_index_path = std::string(scratch_dir) + "/.dftindex";
    std::string meta_index_path_str(meta_index_path);

    Py_BEGIN_ALLOW_THREADS try {
        namespace rcf = dftracer::utils::rocksdb::cf;
        using clock = std::chrono::steady_clock;
        auto ms = [](clock::time_point a, clock::time_point b) -> long long {
            return std::chrono::duration_cast<std::chrono::milliseconds>(b - a)
                .count();
        };

        auto t_start = clock::now();
        dftracer::utils::utilities::indexer::IndexDatabase scratch_db(
            scratch_index_path);
        auto t_scratch_open = clock::now();

        auto raw_db = scratch_db.db();
        for (const auto& p : agg_ssts) {
            auto st = raw_db->ingest_external_files(rcf::AGGREGATION, {p},
                                                    /*ingest_behind=*/false);
            if (!st.ok()) {
                error_msg =
                    "ingest AGGREGATION sst '" + p + "': " + st.ToString();
                break;
            }
        }
        if (error_msg.empty()) {
            for (const auto& p : sys_ssts) {
                auto st = raw_db->ingest_external_files(
                    rcf::SYSTEM_METRICS, {p}, /*ingest_behind=*/false);
                if (!st.ok()) {
                    error_msg = "ingest SYSTEM_METRICS sst '" + p +
                                "': " + st.ToString();
                    break;
                }
            }
        }
        auto t_ingest = clock::now();

        if (error_msg.empty()) {
            auto agg =
                std::make_unique<EventAggregator>(raw_db, /*cfg_hash=*/0);

            // If the caller passed pre-loaded hash tables, skip opening
            // the meta DB on lustre. When many dask workers run
            // scan_aggregation_manifest in parallel, loading the hash
            // tables N times from the same file is significant lustre
            // metadata pressure; loading once on the coordinator and
            // passing them in eliminates the redundant reads.
            std::unordered_map<std::string, std::string> loaded_file_hashes;
            std::unordered_map<std::string, std::string> loaded_host_hashes;
            std::unique_ptr<dftracer::utils::utilities::indexer::IndexDatabase>
                meta_db;
            if (!hashes_preloaded) {
                meta_db = std::make_unique<
                    dftracer::utils::utilities::indexer::IndexDatabase>(
                    meta_index_path_str, dftracer::utils::rocksdb::
                                             RocksDatabase::OpenMode::ReadOnly);
                loaded_file_hashes = meta_db->query_hash_table(
                    dftracer::utils::utilities::indexer::IndexDatabase::
                        HashType::FILE);
                loaded_host_hashes = meta_db->query_hash_table(
                    dftracer::utils::utilities::indexer::IndexDatabase::
                        HashType::HOST);
            }
            const auto& file_hashes =
                hashes_preloaded ? preloaded_file_hashes : loaded_file_hashes;
            const auto& host_hashes =
                hashes_preloaded ? preloaded_host_hashes : loaded_host_hashes;
            auto t_hash_tables = clock::now();

            auto time_bounds = agg->query_time_bounds();
            std::uint64_t time_origin =
                time_bounds.valid ? time_bounds.min_time_bucket : 0;

            DfanalyzerContext ctx;
            ctx.file_hashes = &file_hashes;
            ctx.host_hashes = &host_hashes;
            ctx.query_filter = query_opt ? &*query_opt : nullptr;
            ctx.time_origin = time_origin;
            ctx.time_resolution = time_resolution;
            ctx.time_granularity = time_granularity;

            std::vector<DfanalyzerScanOutput> outputs;
            parallel_shard_scan_range<DfanalyzerScanOutput>(
                rt, static_cast<std::uint16_t>(shard_begin_i),
                static_cast<std::uint16_t>(shard_end_i),
                [&](std::uint16_t sb, std::uint16_t se) {
                    DfanalyzerScanInput input;
                    input.agg = agg.get();
                    input.ctx = &ctx;
                    input.type_filter = std::nullopt;
                    input.batch_size = batch_size;
                    input.shard_begin = sb;
                    input.shard_end = se;
                    input.group_by = group_by_ptr;
                    return scan_dfanalyzer_shards(input);
                },
                outputs);
            auto t_scan = clock::now();

            for (auto& out : outputs) {
                for (auto& r : out.events)
                    events_results.push_back(std::move(r));
                for (auto& r : out.profiles)
                    profiles_results.push_back(std::move(r));
                for (auto& r : out.system)
                    system_results.push_back(std::move(r));
            }

            std::fprintf(
                stderr,
                "[scan_aggregation_manifest] n_agg=%zu n_sys=%zu "
                "scratch_open=%lldms ingest=%lldms hash_tables=%lldms "
                "scan=%lldms\n",
                agg_ssts.size(), sys_ssts.size(), ms(t_start, t_scratch_open),
                ms(t_scratch_open, t_ingest), ms(t_ingest, t_hash_tables),
                ms(t_hash_tables, t_scan));
            std::fflush(stderr);
        }
    } catch (const std::exception& e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        Py_DECREF(events_list);
        Py_DECREF(profiles_list);
        Py_DECREF(system_list);
        Py_DECREF(result_dict);
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return nullptr;
    }

    append_results_to_list(events_list, events_results);
    append_results_to_list(profiles_list, profiles_results);
    append_results_to_list(system_list, system_results);

    PyDict_SetItemString(result_dict, "events", events_list);
    PyDict_SetItemString(result_dict, "profiles", profiles_list);
    PyDict_SetItemString(result_dict, "system", system_list);
    Py_DECREF(events_list);
    Py_DECREF(profiles_list);
    Py_DECREF(system_list);

    return result_dict;
}

static PyMethodDef BatchIndexerModuleMethods[] = {
    {"scan_aggregation_manifest", (PyCFunction)scan_aggregation_manifest_fn,
     METH_VARARGS | METH_KEYWORDS,
     "scan_aggregation_manifest(agg_ssts, sys_ssts, scratch_dir, "
     "meta_index_path, batch_size=10000, time_granularity=1.0, "
     "time_resolution=1e6, query=None, group_by=None, shard_begin=0, "
     "shard_end=4096, runtime=None) -> dict\n"
     "--\n\n"
     "Scan a worker's slice of the distributed aggregation manifest.\n\n"
     "Ingests agg_ssts + sys_ssts into a scratch IndexDatabase at "
     "scratch_dir (caller owns the directory lifecycle) and runs the "
     "dfanalyzer aggregation scan over [shard_begin, shard_end). "
     "meta_index_path is the unified .dftindex used to resolve file / "
     "host hashes. Returns the same dict shape as "
     "Indexer.iter_arrow_dfanalyzer_all."},
    {nullptr, nullptr, 0, nullptr}};
#endif

static PyMethodDef Indexer_methods[] = {
    {"get_checkpoint_indexer", (PyCFunction)Indexer_get_checkpoint_indexer,
     METH_VARARGS,
     "get_checkpoint_indexer(file_path)\n"
     "--\n\n"
     "Get a checkpoint indexer for a specific file.\n\n"
     "Args:\n"
     "    file_path: Path to the trace file (.pfw/.pfw.gz)\n\n"
     "Returns:\n"
     "    Indexer instance for checkpoint-level operations.\n"},
    {"resolve", (PyCFunction)Indexer_resolve, METH_NOARGS,
     "resolve()\n"
     "--\n\n"
     "Check what files exist vs need indexing.\n\n"
     "Returns:\n"
     "    dict with 'total_files', 'ready', 'needs_work', 'index_path'\n"},
    {"build", (PyCFunction)Indexer_build, METH_NOARGS,
     "build()\n"
     "--\n\n"
     "Build all missing index tiers based on require_* flags.\n"},
    {"ensure_indexed", (PyCFunction)Indexer_ensure_indexed, METH_NOARGS,
     "ensure_indexed()\n"
     "--\n\n"
     "Resolve and build if needed.\n\n"
     "Returns:\n"
     "    dict with index status after building.\n"},
    {"get_hash_table", (PyCFunction)Indexer_get_hash_table, METH_VARARGS,
     "get_hash_table(type)\n"
     "--\n\n"
     "Query hash table mappings.\n\n"
     "Args:\n"
     "    type: 'file', 'host', 'string', or 'proc'\n\n"
     "Returns:\n"
     "    dict mapping hash values to resolved names.\n"},
    {"query_file_pids", (PyCFunction)Indexer_query_file_pids, METH_VARARGS,
     "query_file_pids(file_id)\n"
     "--\n\n"
     "Query PIDs observed in a specific file.\n\n"
     "Args:\n"
     "    file_id: Integer file ID from index.\n\n"
     "Returns:\n"
     "    set of PIDs.\n"},
    {"query_all_file_pids", (PyCFunction)Indexer_query_all_file_pids,
     METH_NOARGS,
     "query_all_file_pids()\n"
     "--\n\n"
     "Query PIDs for all indexed files.\n\n"
     "Returns:\n"
     "    dict mapping file_id to set of PIDs.\n"},
    {"query_file_info", (PyCFunction)Indexer_query_file_info, METH_NOARGS,
     "query_file_info()\n"
     "--\n\n"
     "Query file ID to path mapping and per-file PIDs in one call.\n\n"
     "Returns:\n"
     "    tuple of (dict[int, str], dict[int, set[int]]).\n"},
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    {"iter_aggregation", (PyCFunction)Indexer_iter_aggregation,
     METH_VARARGS | METH_KEYWORDS,
     "iter_aggregation(type='events', batch_size=10000)\n"
     "--\n\n"
     "Iterate over aggregation data as Arrow batches.\n\n"
     "Args:\n"
     "    type: 'events', 'profiles', or 'system'\n"
     "    batch_size: Number of entries per batch (default 10000)\n\n"
     "Returns:\n"
     "    Iterator over Arrow batch capsules.\n"},
    {"iter_arrow_dfanalyzer", (PyCFunction)Indexer_iter_arrow_dfanalyzer,
     METH_VARARGS | METH_KEYWORDS,
     "iter_arrow_dfanalyzer(type='events', batch_size=10000, "
     "time_granularity=1.0, time_resolution=1e6, query=None)\n"
     "--\n\n"
     "Iterate over aggregation data as dfanalyzer-compatible Arrow batches.\n\n"
     "Output schema matches dfanalyzer expectations with resolved hashes,\n"
     "normalized time_range, and computed columns (proc_name, io_cat).\n\n"
     "Args:\n"
     "    type: 'events', 'profiles', or 'system'\n"
     "    batch_size: Number of entries per batch (default 10000)\n"
     "    time_granularity: Bucket width in seconds (default 1.0)\n"
     "    time_resolution: Microseconds per output time unit (default 1e6)\n"
     "    query: Optional query filter string (e.g., \"pid == 1234\")\n\n"
     "Returns:\n"
     "    Iterator over Arrow batch capsules.\n"},
    {"iter_arrow_dfanalyzer_all",
     (PyCFunction)Indexer_iter_arrow_dfanalyzer_all,
     METH_VARARGS | METH_KEYWORDS,
     "iter_arrow_dfanalyzer_all(batch_size=10000, time_granularity=1.0, "
     "time_resolution=1e6, query=None, group_by=None)\n"
     "--\n\n"
     "Iterate over all aggregation types in a single scan.\n\n"
     "Returns a dict with 'events', 'profiles', 'system' keys, each "
     "containing\n"
     "a list of Arrow batch capsules. This is ~3x faster than calling\n"
     "iter_arrow_dfanalyzer separately for each type.\n\n"
     "When group_by is provided, the scan collapses dimensions during "
     "aggregation\n"
     "and emits a reduced schema containing only the requested columns plus\n"
     "aggregated metrics (count, time, size, time_sq, size_sq, time_min,\n"
     "time_max, size_min, size_max, time_call_min, time_call_max, "
     "size_call_min,\n"
     "size_call_max, time_start, time_end). Supported group_by columns: "
     "cat,\n"
     "func_name, pid, tid, file_hash, host_hash, file_name, host_name, "
     "proc_name,\n"
     "io_cat, acc_pat, time_range.\n\n"
     "Args:\n"
     "    batch_size: Number of entries per batch (default 10000)\n"
     "    time_granularity: Bucket width in seconds (default 1.0)\n"
     "    time_resolution: Microseconds per output time unit (default 1e6)\n"
     "    query: Optional query filter string\n"
     "    group_by: Optional list of columns to group by; enables coarse\n"
     "        in-scan aggregation (default None = full granularity)\n\n"
     "Returns:\n"
     "    dict with 'events', 'profiles', 'system' lists of Arrow capsules.\n"},
#endif
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
    "             require_manifest=True, require_aggregation=False,\n"
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
    "- Tier 2: Bloom filters (require_bloom), Manifests (require_manifest)\n"
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

int init_indexer(PyObject* m) {
    if (PyType_Ready(&IndexerType) < 0) return -1;

    Py_INCREF(&IndexerType);
    if (PyModule_AddObject(m, "Indexer", (PyObject*)&IndexerType) < 0) {
        Py_DECREF(&IndexerType);
        return -1;
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    if (PyModule_AddFunctions(m, BatchIndexerModuleMethods) < 0) return -1;
#endif

    return 0;
}
