#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/file_partition.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/association_tracker.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/python/distribution.h>
#include <dftracer/utils/python/py_agg_config.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_seq_helpers.h>
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

using dftracer::utils::Runtime;
using dftracer::utils::index::build::BatchBuilder;
using dftracer::utils::index::build::IndexBuildBatchConfig;
using dftracer::utils::index::build::IndexBuildBatchResult;
using dftracer::utils::index::build::plan_lpt_partition;
using dftracer::utils::python::parse_string_seq;
using dftracer::utils::utilities::filesystem::FileEntry;
using dftracer::utils::utilities::filesystem::PatternDirectoryScannerUtility;
using dftracer::utils::utilities::filesystem::
    PatternDirectoryScannerUtilityInput;

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
// build_index_batch: run the indexer pipeline into the index of this worker
// ---------------------------------------------------------------------------

static PyObject *build_index_batch_fn(PyObject * /*self*/, PyObject *args,
                                      PyObject *kwds) {
    static const char *kwlist[] = {
        "files",         "index_dir",   "checkpoint_size",
        "force_rebuild", "build_bloom", "bloom_dimensions",
        "parallelism",   "runtime",     "aggregation_config",
        "progress",      NULL};
    PyObject *files_obj;
    const char *index_dir;
    Py_ssize_t checkpoint_size = static_cast<Py_ssize_t>(
        dftracer::utils::constants::indexer::DEFAULT_CHECKPOINT_SIZE);
    int force_rebuild = 0;
    int build_bloom = 1;
    PyObject *bloom_dims_obj = NULL;
    Py_ssize_t parallelism = 0;
    PyObject *runtime_arg = NULL;
    PyObject *aggregation_config_obj = NULL;
    PyObject *progress_obj = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "Os|nppOnOOO", const_cast<char **>(kwlist), &files_obj,
            &index_dir, &checkpoint_size, &force_rebuild, &build_bloom,
            &bloom_dims_obj, &parallelism, &runtime_arg,
            &aggregation_config_obj, &progress_obj)) {
        return NULL;
    }

    // Unpack files.
    std::vector<std::string> files;
    {
        if (!parse_string_seq(files_obj, "files must be a sequence", files))
            return NULL;
    }
    if (files.empty()) return PyBytes_FromStringAndSize(nullptr, 0);

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

    // Optional aggregation config, extracted from the Python dataclass.
    std::shared_ptr<
        dftracer::utils::index::schemas::dft::agg::AggregationConfig>
        agg_config_ptr;
    if (aggregation_config_obj && aggregation_config_obj != Py_None)
        agg_config_ptr = std::make_shared<
            dftracer::utils::index::schemas::dft::agg::AggregationConfig>(
            dftracer::utils::python::agg_config_from_py(
                aggregation_config_obj));

    auto batch_config = std::make_shared<IndexBuildBatchConfig>();
    batch_config->file_paths = std::move(files);
    batch_config->index_dir = index_dir;
    batch_config->build_bloom = build_bloom != 0;
    batch_config->checkpoint_size = static_cast<std::size_t>(checkpoint_size);
    batch_config->force_rebuild = force_rebuild != 0;
    batch_config->bloom_dimensions = std::move(bloom_dims);
    batch_config->parallelism =
        parallelism > 0 ? static_cast<std::size_t>(parallelism)
                        : (rt ? std::max<std::size_t>(rt->threads(), 1) : 1);

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

    IndexBuildBatchResult result;
    if (!run_blocking([&] {
            // Held across the build, so the builder shares this process's
            // writer of the index, which then closes and leaves a complete
            // data file. Each worker writes the index directly and numbers its
            // own files, since a file id is a label inside one process.
            dftracer::utils::index::store::IndexDatabase store_db(index_dir);
            // A worker process may hold this index from an earlier task.
            store_db.refresh();
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
                       "build-index-batch")
                .get();
            store_db.close_writer();
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

    // Combine the per-file trackers the folds produced out-of-band.
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
    return tracker_bytes;
}

static PyObject *enable_aggregation_deterministic_ids_fn(PyObject * /*self*/,
                                                         PyObject * /*args*/) {
    dftracer::utils::index::schemas::dft::agg::
        enable_deterministic_intern_ids();
    Py_RETURN_NONE;
}

// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------

static PyMethodDef DistributionMethods[] = {
    {"build_index_batch", DFTU_PYCFUNCTION(build_index_batch_fn),
     METH_VARARGS | METH_KEYWORDS,
     "build_index_batch(files, index_dir, ...) -> bytes\n"
     "Run the indexer pipeline on `files` into the index at `index_dir`, "
     "which this worker writes directly and numbers its own files in. "
     "Returns the serialized merged AssociationTracker of the batch's "
     "aggregation folds (empty bytes when no aggregation_config was "
     "passed)."},
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
    {"enable_aggregation_deterministic_ids",
     DFTU_PYCFUNCTION(enable_aggregation_deterministic_ids_fn), METH_NOARGS,
     "enable_aggregation_deterministic_ids() -> None\n"
     "Flip the global aggregation dstd::StringIntern into deterministic-id "
     "mode "
     "so the same string maps to the same 32-bit id in every worker "
     "process. Call once at worker startup BEFORE any aggregation work."},
    {NULL, NULL, 0, NULL}};

int dftracer::utils::python::init_distribution(PyObject *m) {
    if (PyModule_AddFunctions(m, DistributionMethods) < 0) return -1;
    return 0;
}
