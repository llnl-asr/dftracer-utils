#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/file_partition.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/association_tracker.h>
#include <dftracer/utils/index/store/index_backend.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/python/py_agg_config.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_seq_helpers.h>
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/sst_distribution.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using dftracer::utils::Runtime;
using dftracer::utils::index::build::BatchBuilder;
using dftracer::utils::index::build::IndexBuildBatchConfig;
using dftracer::utils::index::build::IndexBuildBatchResult;
using dftracer::utils::index::build::plan_lpt_partition;
using dftracer::utils::index::store::IndexDatabaseSstWriterContext;
using dftracer::utils::index::store::IndexWrite;
using dftracer::utils::index::store::SstArtifactRegistry;
using dftracer::utils::python::parse_string_seq;
using dftracer::utils::utilities::filesystem::FileEntry;
using dftracer::utils::utilities::filesystem::PatternDirectoryScannerUtility;
using dftracer::utils::utilities::filesystem::
    PatternDirectoryScannerUtilityInput;

// ---------------------------------------------------------------------------
// SstArtifactRegistry type
// ---------------------------------------------------------------------------

typedef struct {
    PyObject_HEAD std::shared_ptr<SstArtifactRegistry> registry;
} SstArtifactRegistryObject;

static void SstArtifactRegistry_dealloc(SstArtifactRegistryObject *self) {
    self->registry.~shared_ptr<SstArtifactRegistry>();
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *SstArtifactRegistry_new(PyTypeObject *type,
                                         PyObject * /*args*/,
                                         PyObject * /*kwds*/) {
    auto *self = (SstArtifactRegistryObject *)type->tp_alloc(type, 0);
    if (!self) return NULL;
    new (&self->registry) std::shared_ptr<SstArtifactRegistry>(
        std::make_shared<SstArtifactRegistry>());
    return (PyObject *)self;
}

namespace {

// Artifacts dicts map "<family>_sst" to an SST path (or None), one entry per
// layout::Family.
std::string artifact_field(std::size_t family) {
    return std::string(dftracer::utils::index::store::layout::family_name(
               static_cast<dftracer::utils::index::store::layout::Family>(
                   family))) +
           "_sst";
}

/// Convert a Python artifacts dict to the C++ Artifacts struct. Missing
/// or None entries and empty strings are dropped. Returns false on type
/// errors (exception set).
bool artifacts_from_dict(PyObject *dict,
                         IndexDatabaseSstWriterContext::Artifacts *out) {
    if (!PyDict_Check(dict)) {
        PyErr_SetString(PyExc_TypeError, "artifacts must be a dict");
        return false;
    }
    for (std::size_t f = 0; f < out->sst.size(); ++f) {
        const auto field = artifact_field(f);
        PyObject *val = PyDict_GetItemString(dict, field.c_str());  // borrowed
        if (!val || val == Py_None) continue;
        if (!PyList_Check(val)) {
            PyErr_Format(PyExc_TypeError,
                         "artifacts['%s'] must be a list of str or None",
                         field.c_str());
            return false;
        }
        for (Py_ssize_t i = 0; i < PyList_GET_SIZE(val); ++i) {
            PyObject *item = PyList_GET_ITEM(val, i);  // borrowed
            if (!PyUnicode_Check(item)) {
                PyErr_Format(PyExc_TypeError,
                             "artifacts['%s'] must be a list of str",
                             field.c_str());
                return false;
            }
            const char *s = as_utf8(item);
            if (!s) return false;
            if (s[0] != '\0') out->sst[f].emplace_back(s);
        }
    }
    return true;
}

PyObject *artifacts_to_dict(const IndexDatabaseSstWriterContext::Artifacts &a) {
    PyObject *dict = PyDict_New();
    if (!dict) return NULL;
    for (std::size_t f = 0; f < a.sst.size(); ++f) {
        PyObject *v = PyList_New(static_cast<Py_ssize_t>(a.sst[f].size()));
        for (std::size_t i = 0; v && i < a.sst[f].size(); ++i) {
            PyObject *item = PyUnicode_FromString(a.sst[f][i].c_str());
            if (!item) {
                Py_CLEAR(v);
                break;
            }
            PyList_SET_ITEM(v, static_cast<Py_ssize_t>(i), item);
        }
        if (!v ||
            PyDict_SetItemString(dict, artifact_field(f).c_str(), v) != 0) {
            Py_XDECREF(v);
            Py_DECREF(dict);
            return NULL;
        }
        Py_DECREF(v);
    }
    return dict;
}

}  // namespace

static PyObject *SstArtifactRegistry_append(SstArtifactRegistryObject *self,
                                            PyObject *args) {
    PyObject *dict;
    if (!PyArg_ParseTuple(args, "O", &dict)) return NULL;
    IndexDatabaseSstWriterContext::Artifacts a;
    if (!artifacts_from_dict(dict, &a)) return NULL;
    self->registry->append(std::move(a));
    Py_RETURN_NONE;
}

static PyMethodDef SstArtifactRegistry_methods[] = {
    {"append", DFTU_PYCFUNCTION(SstArtifactRegistry_append), METH_VARARGS,
     "append(artifacts_dict) -> None\n"
     "Add a per-batch Artifacts dict (as returned by build_sst_batch or "
     "IndexDatabaseSstWriterContext.commit) to the registry."},
    {NULL}};

static PyTypeObject SstArtifactRegistryType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext.SstArtifactRegistry",
    sizeof(SstArtifactRegistryObject),
    0,
    (destructor)SstArtifactRegistry_dealloc,
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
    "Thread-safe collector for SST artifact paths.",
    0,
    0,
    0,
    0,
    0,
    0,
    SstArtifactRegistry_methods,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    SstArtifactRegistry_new,
};

SstArtifactRegistry *dftracer::utils::python::sst_artifact_registry_get(
    PyObject *obj) {
    if (!PyObject_TypeCheck(obj, &SstArtifactRegistryType)) return nullptr;
    return ((SstArtifactRegistryObject *)obj)->registry.get();
}

// ---------------------------------------------------------------------------
// scan_files: parallel directory scan with size info
// ---------------------------------------------------------------------------

static PyObject *scan_files_fn(PyObject * /*self*/, PyObject *args,
                               PyObject *kwds) {
    static const char *kwlist[] = {"directory", "patterns", "recursive",
                                   "runtime", NULL};
    const char *directory;
    PyObject *patterns_obj = NULL;
    int recursive = 0;
    PyObject *runtime_arg = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|OpO",
                                     const_cast<char **>(kwlist), &directory,
                                     &patterns_obj, &recursive, &runtime_arg)) {
        return NULL;
    }

    std::vector<std::string> patterns;
    if (patterns_obj && patterns_obj != Py_None) {
        if (!parse_string_seq(patterns_obj, "patterns must be a sequence",
                              patterns))
            return NULL;
    }

    Runtime *rt = nullptr;
    if (runtime_arg && runtime_arg != Py_None) {
        if (!PyObject_TypeCheck(runtime_arg, &RuntimeType)) {
            PyObject *native = PyObject_GetAttrString(runtime_arg, "_native");
            if (!native || !PyObject_TypeCheck(native, &RuntimeType)) {
                Py_XDECREF(native);
                PyErr_SetString(PyExc_TypeError,
                                "runtime must be a Runtime instance or None");
                return NULL;
            }
            rt = ((RuntimeObject *)native)->runtime.get();
            Py_DECREF(native);
        } else {
            rt = ((RuntimeObject *)runtime_arg)->runtime.get();
        }
    } else {
        rt = dftracer::utils::python::get_default_runtime();
    }

    PatternDirectoryScannerUtilityInput input(directory, patterns,
                                              recursive != 0, true);
    std::vector<FileEntry> entries;
    if (!run_blocking([&] {
            rt->submit(dftracer::utils::run_coro_scope(
                           rt->executor(),
                           [](dftracer::utils::CoroScope &scope,
                              PatternDirectoryScannerUtilityInput in,
                              std::vector<FileEntry> *out)
                               -> dftracer::utils::coro::CoroTask<void> {
                               PatternDirectoryScannerUtility scanner;
                               *out = co_await scanner(scope, in);
                           },
                           std::move(input), &entries),
                       "scan-files")
                .get();
        })) {
        return NULL;
    }

    PyObject *out = PyList_New(static_cast<Py_ssize_t>(entries.size()));
    if (!out) return NULL;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        PyObject *t = Py_BuildValue("(sn)", entries[i].path.c_str(),
                                    (Py_ssize_t)entries[i].size);
        if (!t) {
            Py_DECREF(out);
            return NULL;
        }
        PyList_SET_ITEM(out, i, t);
    }
    return out;
}

// ---------------------------------------------------------------------------
// plan_lpt_partition: LPT bin-packing of (path, size) pairs
// ---------------------------------------------------------------------------

static PyObject *plan_lpt_partition_fn(PyObject * /*self*/, PyObject *args) {
    PyObject *entries_obj;
    Py_ssize_t num_workers;
    if (!PyArg_ParseTuple(args, "On", &entries_obj, &num_workers)) return NULL;
    if (num_workers <= 0) num_workers = 1;

    std::vector<FileEntry> entries;
    PyObject *seq = PySequence_Fast(entries_obj,
                                    "entries must be a sequence of "
                                    "(path, size) tuples");
    if (!seq) return NULL;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    entries.reserve(n);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
        const char *path = nullptr;
        Py_ssize_t size = 0;
        if (!PyArg_ParseTuple(item, "sn", &path, &size)) {
            Py_DECREF(seq);
            return NULL;
        }
        FileEntry fe;
        fe.path = path;
        fe.size = static_cast<std::size_t>(size);
        fe.is_regular_file = true;
        entries.push_back(std::move(fe));
    }
    Py_DECREF(seq);

    auto buckets = plan_lpt_partition(std::move(entries),
                                      static_cast<std::size_t>(num_workers));

    PyObject *out = PyList_New(static_cast<Py_ssize_t>(buckets.size()));
    if (!out) return NULL;
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        PyObject *lst = PyList_New(static_cast<Py_ssize_t>(buckets[i].size()));
        if (!lst) {
            Py_DECREF(out);
            return NULL;
        }
        for (std::size_t j = 0; j < buckets[i].size(); ++j) {
            PyObject *t = Py_BuildValue("(sn)", buckets[i][j].path.c_str(),
                                        (Py_ssize_t)buckets[i][j].size);
            if (!t) {
                Py_DECREF(lst);
                Py_DECREF(out);
                return NULL;
            }
            PyList_SET_ITEM(lst, j, t);
        }
        PyList_SET_ITEM(out, i, lst);
    }
    return out;
}

// ---------------------------------------------------------------------------
// build_sst_batch: run the indexer pipeline with an SST sink and return
// the merged Artifacts dict.
// ---------------------------------------------------------------------------

static PyObject *build_sst_batch_fn(PyObject * /*self*/, PyObject *args,
                                    PyObject *kwds) {
    static const char *kwlist[] = {"files",
                                   "file_ids",
                                   "staging_dir",
                                   "batch_id",
                                   "index_dir",
                                   "checkpoint_size",
                                   "force_rebuild",
                                   "build_bloom",
                                   "bloom_dimensions",
                                   "parallelism",
                                   "flush_every_files",
                                   "runtime",
                                   "aggregation_config",
                                   "progress",
                                   NULL};
    PyObject *files_obj;
    PyObject *file_ids_obj;
    const char *staging_dir;
    const char *batch_id;
    const char *index_dir = "";
    Py_ssize_t checkpoint_size = static_cast<Py_ssize_t>(
        dftracer::utils::constants::indexer::DEFAULT_CHECKPOINT_SIZE);
    int force_rebuild = 0;
    int build_bloom = 1;
    PyObject *bloom_dims_obj = NULL;
    Py_ssize_t parallelism = 0;
    Py_ssize_t flush_every_files = 0;
    PyObject *runtime_arg = NULL;
    PyObject *aggregation_config_obj = NULL;
    PyObject *progress_obj = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "OOss|snppOnnOOO", const_cast<char **>(kwlist),
            &files_obj, &file_ids_obj, &staging_dir, &batch_id, &index_dir,
            &checkpoint_size, &force_rebuild, &build_bloom, &bloom_dims_obj,
            &parallelism, &flush_every_files, &runtime_arg,
            &aggregation_config_obj, &progress_obj)) {
        return NULL;
    }

    // Unpack files.
    std::vector<std::string> files;
    {
        if (!parse_string_seq(files_obj, "files must be a sequence", files))
            return NULL;
    }
    if (files.empty()) {
        return PyDict_New();
    }

    // Unpack file_ids, parallel to files.
    std::vector<int> file_ids;
    {
        PyObject *seq =
            PySequence_Fast(file_ids_obj, "file_ids must be a sequence");
        if (!seq) return NULL;
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        if (static_cast<std::size_t>(n) != files.size()) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_ValueError,
                            "file_ids must have the same length as files");
            return NULL;
        }
        file_ids.reserve(n);
        for (Py_ssize_t i = 0; i < n; ++i) {
            long v = PyLong_AsLong(PySequence_Fast_GET_ITEM(seq, i));
            if (v == -1 && PyErr_Occurred()) {
                Py_DECREF(seq);
                return NULL;
            }
            file_ids.push_back(static_cast<int>(v));
        }
        Py_DECREF(seq);
    }

    // Optional bloom dimensions override.
    std::vector<std::string> bloom_dims;
    if (bloom_dims_obj && bloom_dims_obj != Py_None) {
        if (!parse_string_seq(bloom_dims_obj,
                              "bloom_dimensions must be a sequence",
                              bloom_dims))
            return NULL;
    }

    // Resolve Runtime (matching CheckpointIndexer pattern).
    Runtime *rt = nullptr;
    if (runtime_arg && runtime_arg != Py_None) {
        if (PyObject_TypeCheck(runtime_arg, &RuntimeType)) {
            rt = ((RuntimeObject *)runtime_arg)->runtime.get();
        } else {
            PyObject *native = PyObject_GetAttrString(runtime_arg, "_native");
            if (!native || !PyObject_TypeCheck(native, &RuntimeType)) {
                Py_XDECREF(native);
                PyErr_SetString(PyExc_TypeError,
                                "runtime must be a Runtime instance or None");
                return NULL;
            }
            rt = ((RuntimeObject *)native)->runtime.get();
            Py_DECREF(native);
        }
    } else {
        rt = dftracer::utils::python::get_default_runtime();
    }

    // Build config + sink factory shared state.
    struct SharedArtifacts {
        std::mutex mu;
        std::vector<IndexDatabaseSstWriterContext::Artifacts> list;
    };
    auto artifacts = std::make_shared<SharedArtifacts>();
    auto staging = std::string(staging_dir);
    auto batch = std::string(batch_id);

    // Optional aggregation config, extracted from the Python dataclass.
    std::shared_ptr<
        dftracer::utils::index::schemas::dft::agg::AggregationConfig>
        agg_config_ptr;
    if (aggregation_config_obj && aggregation_config_obj != Py_None)
        agg_config_ptr = std::make_shared<
            dftracer::utils::index::schemas::dft::agg::AggregationConfig>(
            dftracer::utils::python::agg_config_from_py(
                aggregation_config_obj));

    // The store takes each worker's writes directly: no staged SSTs, and the
    // worker numbers its own files, since a file id is a label inside one
    // process there.
    const bool on_store = dftracer::utils::index::store::index_backend() ==
                          dftracer::utils::index::store::IndexBackend::Store;
    if (on_store) file_ids.clear();

    auto batch_config = std::make_shared<IndexBuildBatchConfig>();
    batch_config->file_paths = std::move(files);
    batch_config->preassigned_file_ids = std::move(file_ids);
    batch_config->index_dir = index_dir;
    batch_config->build_bloom = build_bloom != 0;
    batch_config->checkpoint_size = static_cast<std::size_t>(checkpoint_size);
    batch_config->force_rebuild = force_rebuild != 0;
    batch_config->bloom_dimensions = std::move(bloom_dims);
    batch_config->parallelism =
        parallelism > 0 ? static_cast<std::size_t>(parallelism)
                        : (rt ? std::max<std::size_t>(rt->threads(), 1) : 1);
    batch_config->flush_every_files =
        static_cast<std::size_t>(flush_every_files);

    // The build runs with the GIL released; re-acquire it per call. The
    // GIL-holding deleter drops the ref safely after the build.
    if (progress_obj && progress_obj != Py_None) {
        Py_INCREF(progress_obj);
        std::shared_ptr<PyObject> cb(progress_obj, [](PyObject *p) {
            PyGILState_STATE g = PyGILState_Ensure();
            Py_DECREF(p);
            PyGILState_Release(g);
        });
        batch_config->progress = [cb](std::size_t done, std::size_t total) {
            PyGILState_STATE g = PyGILState_Ensure();
            PyObject *r = PyObject_CallFunction(cb.get(), "nn",
                                                static_cast<Py_ssize_t>(done),
                                                static_cast<Py_ssize_t>(total));
            if (r) {
                Py_DECREF(r);
            } else {
                // A failing progress callback must not abort the build.
                PyErr_Clear();
            }
            PyGILState_Release(g);
        };
    }

    if (agg_config_ptr) {
        auto agg_intern =
            dftracer::utils::index::schemas::dft::agg::intern_for_index(
                index_dir);
        // The fold writes aggregation SSTs through the batch build's own SST
        // sink (routed to blob.sst / system_metrics.sst), so they land
        // in `artifacts->list` with bloom/dict - no separate per-file sink.
        batch_config->agg_fold_factory =
            [agg_config_ptr, agg_intern](
                dftracer::utils::dstd::StringIntern &build_intern, int file_id)
            -> std::unique_ptr<
                dftracer::utils::index::schemas::dft::agg::AggregationFold> {
            return std::make_unique<
                dftracer::utils::index::schemas::dft::agg::AggregationFold>(
                build_intern, agg_intern, *agg_config_ptr, file_id);
        };
    }

    // Atomic: write phase calls sink_factory from N coroutines concurrently.
    auto batch_counter = std::make_shared<std::atomic<std::size_t>>(0);
    if (!on_store) {
        batch_config->sink_factory =
            [staging, batch, batch_counter]() -> std::unique_ptr<IndexWrite> {
            const std::size_t idx =
                batch_counter->fetch_add(1, std::memory_order_relaxed);
            std::string sub_batch = batch + "_" + std::to_string(idx);
            return std::make_unique<IndexDatabaseSstWriterContext>(staging,
                                                                   sub_batch);
        };
        batch_config->sink_commit = [artifacts](IndexWrite &sink) {
            auto &sst = static_cast<IndexDatabaseSstWriterContext &>(sink);
            auto batch_artifacts = sst.commit();
            std::lock_guard<std::mutex> lock(artifacts->mu);
            if (!batch_artifacts.empty()) {
                artifacts->list.push_back(std::move(batch_artifacts));
            }
        };
    }
    // Held across the build, so the builder shares this process's writer of
    // the index, which then closes and leaves a complete data file.
    std::optional<dftracer::utils::index::store::IndexDatabase> store_db;
    if (on_store) {
        store_db.emplace(index_dir);
        // A worker process may hold this index from an earlier task.
        store_db->refresh();
    }

    IndexBuildBatchResult result;
    if (!run_blocking([&] {
            rt->submit(dftracer::utils::run_coro_scope(
                           rt->executor(),
                           [](dftracer::utils::CoroScope &scope,
                              std::shared_ptr<IndexBuildBatchConfig> cfg,
                              IndexBuildBatchResult *out)
                               -> dftracer::utils::coro::CoroTask<void> {
                               *out = co_await BatchBuilder::process(
                                   &scope, std::move(cfg));
                           },
                           batch_config, &result),
                       "build-sst-batch")
                .get();
            if (store_db) store_db->close_writer();
        })) {
        return NULL;
    }

    // If any file failed, surface the first error.
    if (result.failed > 0) {
        for (const auto &r : result.results) {
            if (!r.success) {
                PyErr_SetString(PyExc_RuntimeError, r.error_message.c_str());
                return NULL;
            }
        }
    }

    // One dict per committed sink + per-file aggregation below.
    PyObject *out_list = PyList_New(0);
    if (!out_list) return NULL;
    {
        std::lock_guard<std::mutex> lock(artifacts->mu);
        for (const auto &a : artifacts->list) {
            PyObject *main_dict = artifacts_to_dict(a);
            if (!main_dict || PyList_Append(out_list, main_dict) < 0) {
                Py_XDECREF(main_dict);
                Py_DECREF(out_list);
                return NULL;
            }
            Py_DECREF(main_dict);
        }
    }
    // The fold wrote its aggregation SSTs through the batch build's sink, so
    // they are already in `artifacts->list` above (no separate per-visitor
    // harvest). Combine the per-file trackers the folds produced out-of-band.
    using dftracer::utils::index::schemas::dft::agg::AssociationTracker;
    AssociationTracker combined;
    bool any_tracker = false;
    for (auto &ao : result.agg_outputs) {
        if (ao.tracker) {
            ao.tracker->finalize();
            combined.merge(*ao.tracker);
            any_tracker = true;
        }
    }
    PyObject *tracker_bytes = nullptr;
    if (any_tracker) {
        combined.finalize();
        std::string blob = combined.serialize();
        tracker_bytes = PyBytes_FromStringAndSize(
            blob.data(), static_cast<Py_ssize_t>(blob.size()));
    } else {
        tracker_bytes = PyBytes_FromStringAndSize(nullptr, 0);
    }
    if (!tracker_bytes) {
        Py_DECREF(out_list);
        return NULL;
    }
    PyObject *ret = PyTuple_Pack(2, out_list, tracker_bytes);
    Py_DECREF(out_list);
    Py_DECREF(tracker_bytes);
    return ret;
}

static PyObject *index_backend_fn(PyObject * /*self*/, PyObject * /*args*/) {
    using dftracer::utils::index::store::IndexBackend;
    return PyUnicode_FromString(
        dftracer::utils::index::store::index_backend() == IndexBackend::Store
            ? dftracer::utils::constants::indexer::INDEX_BACKEND_STORE
            : dftracer::utils::constants::indexer::INDEX_BACKEND_ROCKSDB);
}

static PyObject *enable_aggregation_deterministic_ids_fn(PyObject * /*self*/,
                                                         PyObject * /*args*/) {
    dftracer::utils::index::schemas::dft::agg::
        enable_deterministic_intern_ids();
    Py_RETURN_NONE;
}

static PyObject *move_artifacts_fn(PyObject * /*self*/, PyObject *args,
                                   PyObject *kwds) {
    static const char *kwlist[] = {"artifacts", "dest_dir", NULL};
    PyObject *dict = NULL;
    const char *dest_dir = NULL;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "Os", const_cast<char **>(kwlist), &dict, &dest_dir)) {
        return NULL;
    }
    IndexDatabaseSstWriterContext::Artifacts a;
    if (!artifacts_from_dict(dict, &a)) return NULL;
    IndexDatabaseSstWriterContext::Artifacts moved;
    if (!run_blocking_r([&] { return std::move(a).move_to(dest_dir); }, moved))
        return NULL;
    return artifacts_to_dict(moved);
}

// LPT (longest-processing-time) work assignment for balanced SST distribution.
// so the Dask backend produces identical work distribution to MPI.
// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------

static PyMethodDef SstDistributionMethods[] = {
    {"build_sst_batch", DFTU_PYCFUNCTION(build_sst_batch_fn),
     METH_VARARGS | METH_KEYWORDS,
     "build_sst_batch(files, file_ids, staging_dir, batch_id, ...) "
     "-> (list[dict], bytes)\n"
     "Run the indexer pipeline with an SST sink and return "
     "(artifact_dicts, tracker_blob). The tracker blob is the serialized "
     "merged AssociationTracker from this batch's aggregation visitors "
     "(empty bytes when no aggregation_config was passed). On the store "
     "backend the batch writes the index itself and returns no artifacts: "
     "file_ids are not used and staging_dir is unused."},
    {"plan_lpt_partition", DFTU_PYCFUNCTION(plan_lpt_partition_fn),
     METH_VARARGS,
     "plan_lpt_partition(entries, num_workers) -> list[list[(path, size)]]\n"
     "Greedy Longest-Processing-Time-first bin-packing of (path, size) "
     "tuples across num_workers buckets. Minimises the maximum per-worker "
     "total size."},
    {"scan_files", DFTU_PYCFUNCTION(scan_files_fn),
     METH_VARARGS | METH_KEYWORDS,
     "scan_files(directory, patterns=None, recursive=False, runtime=None) "
     "-> list[(path, size)]\n"
     "Parallel directory scan returning (path, size) tuples for regular "
     "files matching the patterns."},
    {"index_backend", DFTU_PYCFUNCTION(index_backend_fn), METH_NOARGS,
     "index_backend() -> str\n"
     "The backend that holds the indexes this process opens: \"rocksdb\" or "
     "\"store\", as DFTRACER_UTILS_INDEX_BACKEND chose it."},
    {"enable_aggregation_deterministic_ids",
     DFTU_PYCFUNCTION(enable_aggregation_deterministic_ids_fn), METH_NOARGS,
     "enable_aggregation_deterministic_ids() -> None\n"
     "Flip the global aggregation dstd::StringIntern into deterministic-id "
     "mode "
     "so the same string maps to the same 32-bit id in every worker "
     "process. Call once at worker startup BEFORE any aggregation work."},
    {"move_artifacts", DFTU_PYCFUNCTION(move_artifacts_fn),
     METH_VARARGS | METH_KEYWORDS,
     "move_artifacts(artifacts, dest_dir) -> dict\n"
     "Move every populated SST in `artifacts` (as returned by "
     "`build_sst_batch`) into `dest_dir` via the C++ rename/copy helper, "
     "returning a fresh dict with the new paths. Single GIL release, no "
     "per-file Python shutil.move overhead."},
    {NULL, NULL, 0, NULL}};

int dftracer::utils::python::init_sst_distribution(PyObject *m) {
    if (register_type(m, &SstArtifactRegistryType, "SstArtifactRegistry") < 0)
        return -1;
    if (PyModule_AddFunctions(m, SstDistributionMethods) < 0) return -1;
    return 0;
}
