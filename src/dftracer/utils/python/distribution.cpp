// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/scratch.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/file_partition.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
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
#include <dftracer/utils/trace/views/sharded_view.h>
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

    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> patterns;
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
    dftracer::utils::dstd::Vector<FileEntry> entries;
    if (!run_blocking([&] {
            rt->submit(dftracer::utils::coro::with_scope(
                           [in = std::move(input),
                            out = &entries](dftracer::utils::coro::Scope &scope)
                               -> dftracer::utils::coro::CoroTask<void> {
                               PatternDirectoryScannerUtility scanner;
                               *out = co_await scanner(scope, in);
                           }),
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

    dftracer::utils::dstd::Vector<FileEntry> entries;
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
        fe.path = dftracer::utils::dstd::Path(
            dftracer::utils::dstd::StringView(path));
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
        dftracer::utils::common::constants::indexer::DEFAULT_CHECKPOINT_SIZE);
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
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> files;
    {
        if (!parse_string_seq(files_obj, "files must be a sequence", files))
            return NULL;
    }
    if (files.empty()) Py_RETURN_NONE;

    // Optional bloom dimensions override.
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> bloom_dims;
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

    // The aggregation tier is built by every batch, the dataclass only
    // customizes it.
    dftracer::utils::dstd::SharedPtr<
        dftracer::utils::index::schemas::dft::agg::AggregationConfig>
        agg_config_ptr;
    if (aggregation_config_obj && aggregation_config_obj != Py_None)
        agg_config_ptr = dftracer::utils::dstd::make_shared<
            dftracer::utils::index::schemas::dft::agg::AggregationConfig>(
            dftracer::utils::python::agg_config_from_py(
                aggregation_config_obj));
    else
        agg_config_ptr = dftracer::utils::dstd::make_shared<
            dftracer::utils::index::schemas::dft::agg::AggregationConfig>(
            dftracer::utils::index::schemas::dft::agg::
                standard_aggregation_config());

    // A fresh build of the whole root stages on node-local scratch when the
    // destination is a slow filesystem, then publishes the store across.
    const dftracer::utils::dstd::String final_root =
        dftracer::utils::index::store::internal::normalize_index_root(
            index_dir);
    dftracer::utils::dstd::Optional<dftracer::utils::common::ScratchSession>
        scratch;
    dftracer::utils::dstd::String build_dir = index_dir;
    if (!dftracer::utils::common::unwrap(
            dftracer::utils::dstd::fs::exists(dftracer::utils::dstd::PathView(
                dftracer::utils::index::store_dir(final_root)))) &&
        dftracer::utils::common::should_stage(final_root)) {
        auto staged = dftracer::utils::common::staged_build_scratch(files);
        if (staged) {
            build_dir = dftracer::utils::dstd::String(staged->dir().str());
            scratch.emplace(std::move(*staged));
        }
    }

    auto batch_config =
        dftracer::utils::dstd::make_shared<IndexBuildBatchConfig>();
    batch_config->file_paths = std::move(files);
    batch_config->index_dir = build_dir;
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
        auto cb = dftracer::utils::dstd::adopt_shared_ptr(
            progress_obj, [](PyObject *p) {
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

    {
        auto agg_intern =
            dftracer::utils::index::schemas::dft::agg::intern_for_index(
                index_dir);
        batch_config->agg_fold_factory =
            [agg_config_ptr,
             agg_intern](dftracer::utils::dstd::StringIntern &build_intern)
            -> dftracer::utils::dstd::UniquePtr<
                dftracer::utils::index::schemas::dft::agg::AggregationFold> {
            return dftracer::utils::dstd::make_unique<
                dftracer::utils::index::schemas::dft::agg::AggregationFold>(
                build_intern, agg_intern, *agg_config_ptr);
        };
    }

    IndexBuildBatchResult result;
    if (!run_blocking([&] {
            rt->submit(dftracer::utils::coro::with_scope(
                           [cfg = batch_config,
                            out = &result](dftracer::utils::coro::Scope &)
                               -> dftracer::utils::coro::CoroTask<void> {
                               *out = co_await BatchBuilder::process(cfg);
                           }),
                       "build-index-batch")
                .get();
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

    // Publish once every handle to the staged store is closed, so a later open
    // reads the destination.
    if (scratch) {
        dftracer::utils::index::release_root(
            dftracer::utils::index::store::internal::normalize_index_root(
                build_dir));
        dftracer::utils::index::release_root(final_root);
        dftracer::utils::common::publish_path(
            dftracer::utils::dstd::PathView(dftracer::utils::index::store_dir(
                dftracer::utils::index::store::internal::normalize_index_root(
                    build_dir))),
            dftracer::utils::dstd::PathView(
                dftracer::utils::index::store_dir(final_root)));
    }

    Py_RETURN_NONE;
}

static PyObject *write_shard_set_fn(PyObject * /*self*/, PyObject *args) {
    const char *root;
    PyObject *shard_dirs_obj;
    if (!PyArg_ParseTuple(args, "sO", &root, &shard_dirs_obj)) return NULL;
    std::vector<std::string> shard_dirs;
    if (!parse_string_seq(shard_dirs_obj, "shard_dirs must be a sequence",
                          shard_dirs))
        return NULL;
    try {
        const auto normalized_root =
            dftracer::utils::index::store::internal::normalize_index_root(root);
        dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> dirs;
        dirs.reserve(shard_dirs.size());
        for (const auto &d : shard_dirs)
            dirs.emplace_back(dftracer::utils::dstd::StringView(d));
        dftracer::utils::trace::views::write_shard_set(
            dftracer::utils::dstd::String(normalized_root), dirs);
    } catch (const std::exception &e) {
        dftracer::utils::python::set_typed_py_error(e);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *consolidate_shard_set_fn(PyObject * /*self*/, PyObject *args) {
    const char *root;
    if (!PyArg_ParseTuple(args, "s", &root)) return NULL;
    std::size_t files = 0;
    try {
        const auto normalized_root =
            dftracer::utils::index::store::internal::normalize_index_root(root);
        files = dftracer::utils::trace::views::consolidate_shard_set(
            dftracer::utils::dstd::String(normalized_root));
    } catch (const std::exception &e) {
        dftracer::utils::python::set_typed_py_error(e);
        return NULL;
    }
    return PyLong_FromSize_t(files);
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
     "build_index_batch(files, index_dir, ...) -> None\n"
     "Run the indexer pipeline on `files` into the index at `index_dir`, "
     "which this worker writes directly and numbers its own files in. "
     "Its aggregation folds are always created; `aggregation_config` "
     "customizes them and defaults to the standard config."},
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
     "Flip the global aggregation dftracer::utils::dstd::StringIntern into "
     "deterministic-id "
     "mode "
     "so the same string maps to the same 32-bit id in every worker "
     "process. Call once at worker startup BEFORE any aggregation work."},
    {"write_shard_set", DFTU_PYCFUNCTION(write_shard_set_fn), METH_VARARGS,
     "write_shard_set(index_root, shard_dirs) -> None\n"
     "Catalog `shard_dirs` as the shards of the index root, in the root's "
     "shards.json. A shard under the root is stored relative to it."},
    {"consolidate_shard_set", DFTU_PYCFUNCTION(consolidate_shard_set_fn),
     METH_VARARGS,
     "consolidate_shard_set(index_root) -> int\n"
     "Merge the shards the root's shards.json names into the root's own "
     "store by moving their data files, then remove the manifest and the "
     "shard directories. Returns the number of trace files the shards "
     "indexed. A failed move keeps the manifest and the shards for a retry."},
    {NULL, NULL, 0, NULL}};

int dftracer::utils::python::init_distribution(PyObject *m) {
    if (PyModule_AddFunctions(m, DistributionMethods) < 0) return -1;
    return 0;
}
