#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/sst_distribution.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_key.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_visitor.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_tracker.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/file_partition.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_member_scanner.h>
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
using dftracer::utils::utilities::filesystem::FileEntry;
using dftracer::utils::utilities::filesystem::PatternDirectoryScannerUtility;
using dftracer::utils::utilities::filesystem::
    PatternDirectoryScannerUtilityInput;
using dftracer::utils::utilities::indexer::IndexBatchBuilderUtility;
using dftracer::utils::utilities::indexer::IndexBatchSink;
using dftracer::utils::utilities::indexer::IndexBuildBatchConfig;
using dftracer::utils::utilities::indexer::IndexBuildBatchResult;
using dftracer::utils::utilities::indexer::IndexDatabaseSstWriterContext;
using dftracer::utils::utilities::indexer::plan_lpt_partition;
using dftracer::utils::utilities::indexer::SstArtifactRegistry;
using dftracer::utils::utilities::indexer::internal::
    enumerate_gzip_member_candidates;
using dftracer::utils::utilities::indexer::internal::GzipMember;

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

// Field names in the Artifacts dict returned by build_sst_batch and
// consumed by SstArtifactRegistry.append. Must match the field names on
// IndexDatabaseSstWriterContext::Artifacts.
constexpr const char *ARTIFACT_FIELDS[] = {
    "metadata_sst",        "checkpoints_sst",         "manifest_sst",
    "chunk_bloom_sst",     "file_bloom_sst",          "chunk_stats_sst",
    "chunk_dim_stats_sst", "dimensions_sst",          "file_scalar_stats_sst",
    "file_cat_counts_sst", "file_pid_tid_counts_sst", "file_name_counts_sst",
    "name_dictionary_sst", "name_file_postings_sst",  "name_chunk_postings_sst",
    "hash_tables_sst",     "aggregation_sst",         "system_metrics_sst",
};

/// Map a slot name to the matching Artifacts member. Kept in one place so
/// that adding a new CF requires updating only `ARTIFACT_FIELDS` plus
/// `dispatch_*` below.
std::optional<std::string> *artifacts_slot(
    IndexDatabaseSstWriterContext::Artifacts &a, std::string_view name) {
    if (name == "metadata_sst") return &a.metadata_sst;
    if (name == "checkpoints_sst") return &a.checkpoints_sst;
    if (name == "manifest_sst") return &a.manifest_sst;
    if (name == "chunk_bloom_sst") return &a.chunk_bloom_sst;
    if (name == "file_bloom_sst") return &a.file_bloom_sst;
    if (name == "chunk_stats_sst") return &a.chunk_stats_sst;
    if (name == "chunk_dim_stats_sst") return &a.chunk_dim_stats_sst;
    if (name == "dimensions_sst") return &a.dimensions_sst;
    if (name == "file_scalar_stats_sst") return &a.file_scalar_stats_sst;
    if (name == "file_cat_counts_sst") return &a.file_cat_counts_sst;
    if (name == "file_pid_tid_counts_sst") return &a.file_pid_tid_counts_sst;
    if (name == "file_name_counts_sst") return &a.file_name_counts_sst;
    if (name == "name_dictionary_sst") return &a.name_dictionary_sst;
    if (name == "name_file_postings_sst") return &a.name_file_postings_sst;
    if (name == "name_chunk_postings_sst") return &a.name_chunk_postings_sst;
    if (name == "hash_tables_sst") return &a.hash_tables_sst;
    if (name == "aggregation_sst") return &a.aggregation_sst;
    if (name == "system_metrics_sst") return &a.system_metrics_sst;
    return nullptr;
}

/// Convert a Python artifacts dict to the C++ Artifacts struct. Missing,
/// None, or empty-string entries become nullopt. Returns false on type
/// errors (exception set).
bool artifacts_from_dict(PyObject *dict,
                         IndexDatabaseSstWriterContext::Artifacts *out) {
    if (!PyDict_Check(dict)) {
        PyErr_SetString(PyExc_TypeError, "artifacts must be a dict");
        return false;
    }
    for (const char *field : ARTIFACT_FIELDS) {
        PyObject *val = PyDict_GetItemString(dict, field);  // borrowed
        if (!val || val == Py_None) continue;
        if (!PyUnicode_Check(val)) {
            PyErr_Format(PyExc_TypeError, "artifacts['%s'] must be str or None",
                         field);
            return false;
        }
        const char *s = PyUnicode_AsUTF8(val);
        if (!s) return false;
        if (s[0] == '\0') continue;
        auto *slot = artifacts_slot(*out, field);
        if (slot) *slot = std::string(s);
    }
    return true;
}

PyObject *artifacts_to_dict(const IndexDatabaseSstWriterContext::Artifacts &a) {
    PyObject *dict = PyDict_New();
    if (!dict) return NULL;
    auto set_field = [&](const char *name,
                         const std::optional<std::string> &slot) -> bool {
        PyObject *v = slot.has_value() ? PyUnicode_FromString(slot->c_str())
                                       : (Py_INCREF(Py_None), Py_None);
        if (!v) return false;
        int rc = PyDict_SetItemString(dict, name, v);
        Py_DECREF(v);
        return rc == 0;
    };
    if (!set_field("metadata_sst", a.metadata_sst) ||
        !set_field("checkpoints_sst", a.checkpoints_sst) ||
        !set_field("manifest_sst", a.manifest_sst) ||
        !set_field("chunk_bloom_sst", a.chunk_bloom_sst) ||
        !set_field("file_bloom_sst", a.file_bloom_sst) ||
        !set_field("chunk_stats_sst", a.chunk_stats_sst) ||
        !set_field("chunk_dim_stats_sst", a.chunk_dim_stats_sst) ||
        !set_field("dimensions_sst", a.dimensions_sst) ||
        !set_field("file_scalar_stats_sst", a.file_scalar_stats_sst) ||
        !set_field("file_cat_counts_sst", a.file_cat_counts_sst) ||
        !set_field("file_pid_tid_counts_sst", a.file_pid_tid_counts_sst) ||
        !set_field("file_name_counts_sst", a.file_name_counts_sst) ||
        !set_field("name_dictionary_sst", a.name_dictionary_sst) ||
        !set_field("name_file_postings_sst", a.name_file_postings_sst) ||
        !set_field("name_chunk_postings_sst", a.name_chunk_postings_sst) ||
        !set_field("hash_tables_sst", a.hash_tables_sst) ||
        !set_field("aggregation_sst", a.aggregation_sst) ||
        !set_field("system_metrics_sst", a.system_metrics_sst)) {
        Py_DECREF(dict);
        return NULL;
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
    {"append", (PyCFunction)SstArtifactRegistry_append, METH_VARARGS,
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

SstArtifactRegistry *sst_artifact_registry_get(PyObject *obj) {
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
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|OpO", (char **)kwlist,
                                     &directory, &patterns_obj, &recursive,
                                     &runtime_arg)) {
        return NULL;
    }

    std::vector<std::string> patterns;
    if (patterns_obj && patterns_obj != Py_None) {
        PyObject *seq =
            PySequence_Fast(patterns_obj, "patterns must be a sequence");
        if (!seq) return NULL;
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        patterns.reserve(n);
        for (Py_ssize_t i = 0; i < n; ++i) {
            const char *s = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(seq, i));
            if (!s) {
                Py_DECREF(seq);
                return NULL;
            }
            patterns.emplace_back(s);
        }
        Py_DECREF(seq);
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
        rt = get_default_runtime();
    }

    PatternDirectoryScannerUtilityInput input(directory, patterns,
                                              recursive != 0, true);
    std::vector<FileEntry> entries;
    try {
        Py_BEGIN_ALLOW_THREADS rt
            ->submit(dftracer::utils::run_coro_scope(
                         rt->executor(),
                         [](dftracer::utils::CoroScope &scope,
                            PatternDirectoryScannerUtilityInput in,
                            std::vector<FileEntry> *out)
                             -> dftracer::utils::coro::CoroTask<void> {
                             PatternDirectoryScannerUtility scanner;
                             *out = co_await scope.spawn(scanner, in);
                         },
                         std::move(input), &entries),
                     "scan-files")
            .get();
        Py_END_ALLOW_THREADS
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
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
                                   "build_manifest",
                                   "force_rebuild",
                                   "bloom_dimensions",
                                   "parallelism",
                                   "flush_every_files",
                                   "runtime",
                                   "aggregation_config",
                                   "file_slices",
                                   NULL};
    PyObject *files_obj;
    PyObject *file_ids_obj;
    const char *staging_dir;
    const char *batch_id;
    const char *index_dir = "";
    Py_ssize_t checkpoint_size = 32 * 1024 * 1024;
    int build_manifest = 0;
    int force_rebuild = 0;
    PyObject *bloom_dims_obj = NULL;
    Py_ssize_t parallelism = 0;
    Py_ssize_t flush_every_files = 0;
    PyObject *runtime_arg = NULL;
    PyObject *aggregation_config_obj = NULL;
    PyObject *file_slices_obj = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "OOss|snppOnnOOO", (char **)kwlist, &files_obj,
            &file_ids_obj, &staging_dir, &batch_id, &index_dir,
            &checkpoint_size, &build_manifest, &force_rebuild, &bloom_dims_obj,
            &parallelism, &flush_every_files, &runtime_arg,
            &aggregation_config_obj, &file_slices_obj)) {
        return NULL;
    }

    // Unpack files.
    std::vector<std::string> files;
    {
        PyObject *seq = PySequence_Fast(files_obj, "files must be a sequence");
        if (!seq) return NULL;
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        files.reserve(n);
        for (Py_ssize_t i = 0; i < n; ++i) {
            const char *s = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(seq, i));
            if (!s) {
                Py_DECREF(seq);
                return NULL;
            }
            files.emplace_back(s);
        }
        Py_DECREF(seq);
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
        PyObject *seq = PySequence_Fast(bloom_dims_obj,
                                        "bloom_dimensions must be a sequence");
        if (!seq) return NULL;
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        bloom_dims.reserve(n);
        for (Py_ssize_t i = 0; i < n; ++i) {
            const char *s = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(seq, i));
            if (!s) {
                Py_DECREF(seq);
                return NULL;
            }
            bloom_dims.emplace_back(s);
        }
        Py_DECREF(seq);
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
        rt = get_default_runtime();
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
    std::shared_ptr<dftracer::utils::utilities::composites::dft::aggregators::
                        AggregationConfig>
        agg_config_ptr;
    if (aggregation_config_obj && aggregation_config_obj != Py_None) {
        using dftracer::utils::utilities::composites::dft::aggregators::
            AggregationConfig;
        auto cfg = std::make_shared<AggregationConfig>();
        auto pull_double = [&](const char *name, double fallback) -> double {
            PyObject *v = PyObject_GetAttrString(aggregation_config_obj, name);
            if (!v || v == Py_None) {
                Py_XDECREF(v);
                PyErr_Clear();
                return fallback;
            }
            double out = PyFloat_AsDouble(v);
            Py_DECREF(v);
            if (out == -1.0 && PyErr_Occurred()) return fallback;
            return out;
        };
        auto pull_bool = [&](const char *name, bool fallback) -> bool {
            PyObject *v = PyObject_GetAttrString(aggregation_config_obj, name);
            if (!v || v == Py_None) {
                Py_XDECREF(v);
                PyErr_Clear();
                return fallback;
            }
            int out = PyObject_IsTrue(v);
            Py_DECREF(v);
            return out > 0 ? true : fallback;
        };
        auto pull_string_list =
            [&](const char *name) -> std::vector<std::string> {
            std::vector<std::string> out;
            PyObject *v = PyObject_GetAttrString(aggregation_config_obj, name);
            if (!v || v == Py_None) {
                Py_XDECREF(v);
                PyErr_Clear();
                return out;
            }
            PyObject *seq = PySequence_Fast(v, "expected list of str");
            Py_DECREF(v);
            if (!seq) {
                PyErr_Clear();
                return out;
            }
            Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
            out.reserve(n);
            for (Py_ssize_t i = 0; i < n; ++i) {
                const char *s =
                    PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(seq, i));
                if (s) out.emplace_back(s);
            }
            Py_DECREF(seq);
            return out;
        };
        double time_interval_ms = pull_double("time_interval_ms", 5000.0);
        cfg->time_interval_us =
            static_cast<std::uint64_t>(time_interval_ms * 1000.0);
        cfg->compute_percentiles = pull_bool("compute_percentiles", false);
        cfg->extra_group_keys = pull_string_list("group_keys");
        cfg->custom_metric_fields = pull_string_list("custom_metric_fields");
        agg_config_ptr = std::move(cfg);
    }

    // owned_member_maps must outlive rt->submit: FileSlice::members is raw.
    std::vector<std::vector<GzipMember>> owned_member_maps;
    std::vector<IndexBuildBatchConfig::FileSlice> parsed_slices;
    if (file_slices_obj && file_slices_obj != Py_None) {
        PyObject *seq =
            PySequence_Fast(file_slices_obj, "file_slices must be a sequence");
        if (!seq) return NULL;
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        if (static_cast<std::size_t>(n) != files.size()) {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_ValueError,
                            "file_slices must match files length");
            return NULL;
        }
        owned_member_maps.resize(n);
        parsed_slices.resize(n);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject *entry = PySequence_Fast_GET_ITEM(seq, i);
            if (entry == Py_None) {
                continue;  // leave slice default-constructed (members=null)
            }
            Py_ssize_t mb = 0, me = 0, ckpt_base = 0;
            int skip_scoped = 0;
            PyObject *members_obj = nullptr;
            if (!PyArg_ParseTuple(entry, "nnnpO", &mb, &me, &ckpt_base,
                                  &skip_scoped, &members_obj)) {
                Py_DECREF(seq);
                return NULL;
            }
            PyObject *mseq = PySequence_Fast(
                members_obj, "file_slices[i].members must be a sequence");
            if (!mseq) {
                Py_DECREF(seq);
                return NULL;
            }
            Py_ssize_t mn = PySequence_Fast_GET_SIZE(mseq);
            auto &mv = owned_member_maps[i];
            mv.resize(mn);
            for (Py_ssize_t j = 0; j < mn; ++j) {
                PyObject *m = PySequence_Fast_GET_ITEM(mseq, j);
                unsigned long long c_offset = 0, c_size = 0;
                if (!PyArg_ParseTuple(m, "KK", &c_offset, &c_size)) {
                    Py_DECREF(mseq);
                    Py_DECREF(seq);
                    return NULL;
                }
                mv[j].c_offset = static_cast<std::uint64_t>(c_offset);
                mv[j].c_size = static_cast<std::uint64_t>(c_size);
            }
            Py_DECREF(mseq);
            parsed_slices[i].members = &mv;
            parsed_slices[i].member_begin = static_cast<std::size_t>(mb);
            parsed_slices[i].member_end = static_cast<std::size_t>(me);
            parsed_slices[i].checkpoint_idx_base =
                static_cast<std::uint64_t>(ckpt_base);
            parsed_slices[i].skip_file_scoped_writes = skip_scoped != 0;
        }
        Py_DECREF(seq);
    }

    auto batch_config = std::make_shared<IndexBuildBatchConfig>();
    batch_config->file_paths = std::move(files);
    batch_config->preassigned_file_ids = std::move(file_ids);
    if (!parsed_slices.empty()) {
        batch_config->file_slices = parsed_slices;
    }
    batch_config->index_dir = index_dir;
    batch_config->checkpoint_size = static_cast<std::size_t>(checkpoint_size);
    batch_config->build_manifest = build_manifest != 0;
    batch_config->force_rebuild = force_rebuild != 0;
    batch_config->bloom_dimensions = std::move(bloom_dims);
    batch_config->parallelism =
        parallelism > 0 ? static_cast<std::size_t>(parallelism)
                        : (rt ? std::max<std::size_t>(rt->threads(), 1) : 1);
    batch_config->flush_every_files =
        static_cast<std::size_t>(flush_every_files);
    batch_config->rebuild_root_summaries = false;

    if (agg_config_ptr) {
        auto agg_staging = staging;
        auto agg_prefix = batch + "_agg";
        // Counter keeps per-file SST dirs unique across duplicate file_paths
        // when one worker owns multiple slices of the same file.
        auto visitor_counter = std::make_shared<std::atomic<std::size_t>>(0);
        batch_config->dft_visitor_factory =
            [agg_staging, agg_prefix, agg_config_ptr,
             visitor_counter](const std::string &file_path)
            -> std::vector<std::unique_ptr<
                dftracer::utils::utilities::composites::dft::DftEventVisitor>> {
            using dftracer::utils::utilities::composites::dft::DftEventVisitor;
            using dftracer::utils::utilities::composites::dft::aggregators::
                AggregationVisitor;
            const std::size_t idx =
                visitor_counter->fetch_add(1, std::memory_order_relaxed);
            std::string prefix = agg_prefix + "_" + std::to_string(idx);
            std::vector<std::unique_ptr<DftEventVisitor>> visitors;
            visitors.push_back(std::make_unique<AggregationVisitor>(
                agg_staging, prefix, /*config_hash=*/0, *agg_config_ptr,
                file_path));
            return visitors;
        };
    }

    // Atomic: write phase calls sink_factory from N coroutines concurrently.
    auto batch_counter = std::make_shared<std::atomic<std::size_t>>(0);
    batch_config->sink_factory =
        [staging, batch, batch_counter]() -> std::unique_ptr<IndexBatchSink> {
        const std::size_t idx =
            batch_counter->fetch_add(1, std::memory_order_relaxed);
        std::string sub_batch = batch + "_" + std::to_string(idx);
        return std::make_unique<IndexDatabaseSstWriterContext>(staging,
                                                               sub_batch);
    };
    batch_config->sink_commit = [artifacts](IndexBatchSink &sink) {
        auto &sst = static_cast<IndexDatabaseSstWriterContext &>(sink);
        auto batch_artifacts = sst.commit();
        std::lock_guard<std::mutex> lock(artifacts->mu);
        if (!batch_artifacts.empty()) {
            artifacts->list.push_back(std::move(batch_artifacts));
        }
    };

    IndexBuildBatchResult result;
    std::string submit_error;
    Py_BEGIN_ALLOW_THREADS try {
        rt->submit(dftracer::utils::run_coro_scope(
                       rt->executor(),
                       [](dftracer::utils::CoroScope &scope,
                          std::shared_ptr<IndexBuildBatchConfig> cfg,
                          IndexBuildBatchResult *out)
                           -> dftracer::utils::coro::CoroTask<void> {
                           *out = co_await IndexBatchBuilderUtility::process(
                               &scope, std::move(cfg));
                       },
                       batch_config, &result),
                   "build-sst-batch")
            .get();
    } catch (const std::exception &e) {
        submit_error = e.what();
    }
    Py_END_ALLOW_THREADS if (!submit_error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, submit_error.c_str());
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
    // Harvest per-file aggregation SSTs from extra visitors. Each visitor
    // holds a vector of Artifacts (one per FLUSH_THRESHOLD flush + the
    // file-complete flush) because SstFileWriter requires strictly
    // ascending keys per SST and cross-flush merge operands would collide.
    //
    // `extra_visitors` is indexed per input file, but a single
    // AggregationVisitor instance is typically shared across every file in
    // the batch (one flush at end-of-batch). Without dedup we would emit
    // that visitor's artifact dict N_files times, producing a manifest with
    // N copies of the same SST path. Dedup by visitor pointer so each
    // unique flush-sequence is emitted exactly once.
    using dftracer::utils::utilities::composites::dft::aggregators::
        AggregationVisitor;
    using dftracer::utils::utilities::composites::dft::aggregators::
        AssociationTracker;
    std::unordered_set<AggregationVisitor *> seen_visitors;
    for (auto &file_visitors : result.extra_visitors) {
        for (auto &visitor : file_visitors) {
            auto *agg = dynamic_cast<AggregationVisitor *>(visitor.get());
            if (!agg) continue;
            if (!seen_visitors.insert(agg).second) continue;
            for (auto &a : agg->aggregation_artifacts()) {
                if (a.empty()) continue;
                PyObject *agg_dict = artifacts_to_dict(a);
                if (!agg_dict || PyList_Append(out_list, agg_dict) < 0) {
                    Py_XDECREF(agg_dict);
                    Py_DECREF(out_list);
                    return NULL;
                }
                Py_DECREF(agg_dict);
            }
        }
    }

    AssociationTracker combined;
    bool any_tracker = false;
    for (auto *agg : seen_visitors) {
        auto out = agg->take_output();
        if (out.local_tracker) {
            combined.merge(*out.local_tracker);
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

static PyObject *enable_aggregation_deterministic_ids_fn(PyObject * /*self*/,
                                                         PyObject * /*args*/) {
    dftracer::utils::utilities::composites::dft::aggregators::
        aggregation_intern()
            .enable_deterministic_ids();
    Py_RETURN_NONE;
}

static PyObject *move_artifacts_fn(PyObject * /*self*/, PyObject *args,
                                   PyObject *kwds) {
    static const char *kwlist[] = {"artifacts", "dest_dir", NULL};
    PyObject *dict = NULL;
    const char *dest_dir = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Os", (char **)kwlist, &dict,
                                     &dest_dir)) {
        return NULL;
    }
    IndexDatabaseSstWriterContext::Artifacts a;
    if (!artifacts_from_dict(dict, &a)) return NULL;
    IndexDatabaseSstWriterContext::Artifacts moved;
    try {
        Py_BEGIN_ALLOW_THREADS moved = std::move(a).move_to(dest_dir);
        Py_END_ALLOW_THREADS
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
    return artifacts_to_dict(moved);
}

namespace {

dftracer::utils::coro::CoroTask<void> scan_one_gzip_file(
    std::string path, std::vector<GzipMember> *out) {
    out->clear();
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) co_return;
    struct stat st;
    if (::fstat(fd, &st) == 0 && st.st_size >= 18) {
        co_await enumerate_gzip_member_candidates(
            fd, static_cast<std::uint64_t>(st.st_size), *out);
    }
    ::close(fd);
}

}  // namespace

static PyObject *enumerate_gzip_members_fn(PyObject * /*self*/, PyObject *args,
                                           PyObject *kwds) {
    static const char *kwlist[] = {"files", "runtime", NULL};
    PyObject *files_obj = NULL;
    PyObject *runtime_arg = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", (char **)kwlist,
                                     &files_obj, &runtime_arg)) {
        return NULL;
    }

    std::vector<std::string> files;
    {
        PyObject *seq = PySequence_Fast(files_obj, "files must be a sequence");
        if (!seq) return NULL;
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        files.reserve(n);
        for (Py_ssize_t i = 0; i < n; ++i) {
            const char *s = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(seq, i));
            if (!s) {
                Py_DECREF(seq);
                return NULL;
            }
            files.emplace_back(s);
        }
        Py_DECREF(seq);
    }

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
        rt = get_default_runtime();
    }

    std::vector<std::vector<GzipMember>> results(files.size());
    std::string submit_error;
    Py_BEGIN_ALLOW_THREADS try {
        rt->submit(
              dftracer::utils::run_coro_scope(
                  rt->executor(),
                  [](dftracer::utils::CoroScope &scope,
                     const std::vector<std::string> *paths,
                     std::vector<std::vector<GzipMember>> *out)
                      -> dftracer::utils::coro::CoroTask<void> {
                      co_await scope.scope(
                          [paths, out](dftracer::utils::CoroScope &child)
                              -> dftracer::utils::coro::CoroTask<void> {
                              for (std::size_t i = 0; i < paths->size(); ++i) {
                                  const std::string &path = (*paths)[i];
                                  auto *slot = &(*out)[i];
                                  child.spawn(
                                      [path, slot](dftracer::utils::CoroScope &)
                                          -> dftracer::utils::coro::CoroTask<
                                              void> {
                                          co_await scan_one_gzip_file(path,
                                                                      slot);
                                      });
                              }
                              co_return;
                          });
                      co_return;
                  },
                  &files, &results),
              "enumerate-gzip-members")
            .get();
    } catch (const std::exception &e) {
        submit_error = e.what();
    }
    Py_END_ALLOW_THREADS if (!submit_error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, submit_error.c_str());
        return NULL;
    }

    PyObject *out_list = PyList_New(static_cast<Py_ssize_t>(results.size()));
    if (!out_list) return NULL;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto &mv = results[i];
        PyObject *inner = PyList_New(static_cast<Py_ssize_t>(mv.size()));
        if (!inner) {
            Py_DECREF(out_list);
            return NULL;
        }
        for (std::size_t j = 0; j < mv.size(); ++j) {
            PyObject *t =
                Py_BuildValue("(KK)", (unsigned long long)mv[j].c_offset,
                              (unsigned long long)mv[j].c_size);
            if (!t) {
                Py_DECREF(inner);
                Py_DECREF(out_list);
                return NULL;
            }
            PyList_SET_ITEM(inner, j, t);
        }
        PyList_SET_ITEM(out_list, i, inner);
    }
    return out_list;
}

// Mirrors build_work_units + lpt_assign_units in dftracer_aggregator_mpi.cpp
// so the Dask backend produces identical work distribution to MPI.
static PyObject *plan_work_units_fn(PyObject * /*self*/, PyObject *args,
                                    PyObject *kwds) {
    static const char *kwlist[] = {"member_map", "num_workers", "target_c_size",
                                   NULL};
    PyObject *map_obj = NULL;
    Py_ssize_t num_workers = 0;
    unsigned long long target_c_size = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "On|K", (char **)kwlist,
                                     &map_obj, &num_workers, &target_c_size)) {
        return NULL;
    }
    if (num_workers <= 0) num_workers = 1;

    std::vector<std::vector<GzipMember>> member_map;
    {
        PyObject *seq =
            PySequence_Fast(map_obj, "member_map must be a sequence");
        if (!seq) return NULL;
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        member_map.resize(n);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject *inner = PySequence_Fast_GET_ITEM(seq, i);
            PyObject *iseq =
                PySequence_Fast(inner, "member_map[i] must be a sequence");
            if (!iseq) {
                Py_DECREF(seq);
                return NULL;
            }
            Py_ssize_t ni = PySequence_Fast_GET_SIZE(iseq);
            member_map[i].resize(ni);
            for (Py_ssize_t j = 0; j < ni; ++j) {
                PyObject *t = PySequence_Fast_GET_ITEM(iseq, j);
                unsigned long long c_offset = 0, c_size = 0;
                if (!PyArg_ParseTuple(t, "KK", &c_offset, &c_size)) {
                    Py_DECREF(iseq);
                    Py_DECREF(seq);
                    return NULL;
                }
                member_map[i][j].c_offset =
                    static_cast<std::uint64_t>(c_offset);
                member_map[i][j].c_size = static_cast<std::uint64_t>(c_size);
            }
            Py_DECREF(iseq);
        }
        Py_DECREF(seq);
    }

    // Fallback: treat empty/non-gzip files as a single whole-file member.
    std::uint64_t total_c = 0;
    for (auto &mv : member_map) {
        if (mv.empty()) mv.push_back({0, 0});
        for (const auto &m : mv) total_c += m.c_size;
    }

    if (target_c_size == 0) {
        target_c_size =
            (total_c + static_cast<std::uint64_t>(num_workers) - 1) /
            std::max<std::uint64_t>(static_cast<std::uint64_t>(num_workers), 1);
    }

    struct Unit {
        std::size_t file_idx;
        std::size_t member_begin;
        std::size_t member_end;
        std::uint64_t c_size;
    };
    std::vector<Unit> units;
    for (std::size_t fi = 0; fi < member_map.size(); ++fi) {
        const auto &members = member_map[fi];
        if (members.empty()) continue;
        std::size_t begin = 0;
        std::uint64_t accum = 0;
        for (std::size_t i = 0; i < members.size(); ++i) {
            accum += members[i].c_size;
            const bool is_last = (i + 1 == members.size());
            if ((target_c_size > 0 && accum >= target_c_size) || is_last) {
                units.push_back({fi, begin, i + 1, accum});
                begin = i + 1;
                accum = 0;
            }
        }
    }

    std::vector<std::size_t> order(units.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (units[a].c_size != units[b].c_size)
            return units[a].c_size > units[b].c_size;
        if (units[a].file_idx != units[b].file_idx)
            return units[a].file_idx < units[b].file_idx;
        return units[a].member_begin < units[b].member_begin;
    });
    const std::size_t nw = static_cast<std::size_t>(num_workers);
    std::vector<std::uint64_t> loads(nw, 0);
    std::vector<std::vector<std::size_t>> per_worker(nw);
    for (std::size_t ord : order) {
        std::size_t best = 0;
        for (std::size_t r = 1; r < nw; ++r)
            if (loads[r] < loads[best]) best = r;
        per_worker[best].push_back(ord);
        loads[best] += std::max<std::uint64_t>(units[ord].c_size, 1);
    }

    PyObject *out = PyList_New(static_cast<Py_ssize_t>(nw));
    if (!out) return NULL;
    for (std::size_t w = 0; w < nw; ++w) {
        // Keep per-worker slices sorted by (file_idx, member_begin) for
        // deterministic, file-group-friendly iteration downstream.
        auto &lst = per_worker[w];
        std::sort(lst.begin(), lst.end(), [&](std::size_t a, std::size_t b) {
            if (units[a].file_idx != units[b].file_idx)
                return units[a].file_idx < units[b].file_idx;
            return units[a].member_begin < units[b].member_begin;
        });
        PyObject *inner = PyList_New(static_cast<Py_ssize_t>(lst.size()));
        if (!inner) {
            Py_DECREF(out);
            return NULL;
        }
        for (std::size_t k = 0; k < lst.size(); ++k) {
            const auto &u = units[lst[k]];
            PyObject *t = Py_BuildValue(
                "(nnnK)", (Py_ssize_t)u.file_idx, (Py_ssize_t)u.member_begin,
                (Py_ssize_t)u.member_end, (unsigned long long)u.c_size);
            if (!t) {
                Py_DECREF(inner);
                Py_DECREF(out);
                return NULL;
            }
            PyList_SET_ITEM(inner, k, t);
        }
        PyList_SET_ITEM(out, w, inner);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------

static PyMethodDef SstDistributionMethods[] = {
    {"build_sst_batch", (PyCFunction)build_sst_batch_fn,
     METH_VARARGS | METH_KEYWORDS,
     "build_sst_batch(files, file_ids, staging_dir, batch_id, ...) "
     "-> (list[dict], bytes)\n"
     "Run the indexer pipeline with an SST sink and return "
     "(artifact_dicts, tracker_blob). The tracker blob is the serialized "
     "merged AssociationTracker from this batch's aggregation visitors "
     "(empty bytes when no aggregation_config was passed)."},
    {"plan_lpt_partition", (PyCFunction)plan_lpt_partition_fn, METH_VARARGS,
     "plan_lpt_partition(entries, num_workers) -> list[list[(path, size)]]\n"
     "Greedy Longest-Processing-Time-first bin-packing of (path, size) "
     "tuples across num_workers buckets. Minimises the maximum per-worker "
     "total size."},
    {"scan_files", (PyCFunction)scan_files_fn, METH_VARARGS | METH_KEYWORDS,
     "scan_files(directory, patterns=None, recursive=False, runtime=None) "
     "-> list[(path, size)]\n"
     "Parallel directory scan returning (path, size) tuples for regular "
     "files matching the patterns."},
    {"enable_aggregation_deterministic_ids",
     (PyCFunction)enable_aggregation_deterministic_ids_fn, METH_NOARGS,
     "enable_aggregation_deterministic_ids() -> None\n"
     "Flip the global aggregation StringIntern into deterministic-id mode "
     "so the same string maps to the same 32-bit id in every worker "
     "process. Call once at worker startup BEFORE any aggregation work."},
    {"move_artifacts", (PyCFunction)move_artifacts_fn,
     METH_VARARGS | METH_KEYWORDS,
     "move_artifacts(artifacts, dest_dir) -> dict\n"
     "Move every populated SST in `artifacts` (as returned by "
     "`build_sst_batch`) into `dest_dir` via the C++ rename/copy helper, "
     "returning a fresh dict with the new paths. Single GIL release, no "
     "per-file Python shutil.move overhead."},
    {"enumerate_gzip_members", (PyCFunction)enumerate_gzip_members_fn,
     METH_VARARGS | METH_KEYWORDS,
     "enumerate_gzip_members(files, runtime=None) -> list[list[(c_offset, "
     "c_size)]]\n"
     "Cooperative async scan of gzip member offsets across `files`. "
     "Returns lists of (c_offset, c_size) parallel to `files`; empty for "
     "non-gzip / unreadable files."},
    {"plan_work_units", (PyCFunction)plan_work_units_fn,
     METH_VARARGS | METH_KEYWORDS,
     "plan_work_units(member_map, num_workers, target_c_size=0) "
     "-> list[list[(file_idx, member_begin, member_end, c_size)]]\n"
     "Deterministic LPT assignment of intra-file gzip-member slices "
     "across workers. Each worker's list contains (file_idx, "
     "member_begin, member_end, c_size) tuples; a file sliced across "
     "multiple workers appears in each owner's list with disjoint "
     "[member_begin, member_end) ranges."},
    {NULL, NULL, 0, NULL}};

int init_sst_distribution(PyObject *m) {
    if (PyType_Ready(&SstArtifactRegistryType) < 0) return -1;
    Py_INCREF(&SstArtifactRegistryType);
    if (PyModule_AddObject(m, "SstArtifactRegistry",
                           (PyObject *)&SstArtifactRegistryType) < 0) {
        Py_DECREF(&SstArtifactRegistryType);
        return -1;
    }
    if (PyModule_AddFunctions(m, SstDistributionMethods) < 0) return -1;
    return 0;
}
