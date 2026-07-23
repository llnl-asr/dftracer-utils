#include <dftracer/utils/python/index_database.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_list_helpers.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/sst_distribution.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>

#include <new>
#include <string>
#include <unordered_set>
#include <vector>

using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::SstArtifactRegistry;

static void IndexDatabase_dealloc(IndexDatabaseObject *self) {
    self->db.~shared_ptr<IndexDatabase>();
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *IndexDatabase_new(PyTypeObject *type, PyObject * /*args*/,
                                   PyObject * /*kwds*/) {
    auto *self = (IndexDatabaseObject *)type->tp_alloc(type, 0);
    if (!self) return NULL;
    new (&self->db) std::shared_ptr<IndexDatabase>();
    return (PyObject *)self;
}

static int IndexDatabase_init(IndexDatabaseObject *self, PyObject *args,
                              PyObject *kwds) {
    static const char *kwlist[] = {"index_path", NULL};
    const char *index_path;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", (char **)kwlist,
                                     &index_path)) {
        return -1;
    }
    try {
        self->db = std::make_shared<IndexDatabase>(index_path);
    } catch (const std::exception &e) {
        set_typed_py_error(e);
        return -1;
    }
    return 0;
}

static PyObject *IndexDatabase_init_schema(IndexDatabaseObject *self,
                                           PyObject * /*ignored*/) {
    if (!self->db) {
        PyErr_SetString(PyExc_RuntimeError, "IndexDatabase not initialised");
        return NULL;
    }
    if (!run_blocking([&] { self->db->init_schema(); })) return NULL;
    Py_RETURN_NONE;
}

static PyObject *IndexDatabase_register_files(IndexDatabaseObject *self,
                                              PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"paths", "build_manifest", NULL};
    PyObject *paths_obj;
    int build_manifest = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|p", (char **)kwlist,
                                     &paths_obj, &build_manifest)) {
        return NULL;
    }
    std::vector<std::string> paths;
    if (!parse_str_list(paths_obj, "paths", paths)) return NULL;

    std::vector<int> ids;
    if (!run_blocking_r(
            [&] {
                return self->db->register_files(paths, build_manifest != 0);
            },
            ids)) {
        return NULL;
    }

    PyObject *out = PyList_New(static_cast<Py_ssize_t>(ids.size()));
    if (!out) return NULL;
    for (Py_ssize_t i = 0; i < static_cast<Py_ssize_t>(ids.size()); ++i) {
        PyList_SET_ITEM(out, i, PyLong_FromLong(ids[i]));
    }
    return out;
}

static PyObject *IndexDatabase_reserve_file_id_range(IndexDatabaseObject *self,
                                                     PyObject *args) {
    Py_ssize_t count;
    if (!PyArg_ParseTuple(args, "n", &count)) return NULL;
    if (count < 0) {
        PyErr_SetString(PyExc_ValueError, "count must be >= 0");
        return NULL;
    }
    int first;
    if (!run_blocking_r(
            [&] {
                return self->db->reserve_file_id_range(
                    static_cast<std::size_t>(count));
            },
            first)) {
        return NULL;
    }
    return PyLong_FromLong(first);
}

static PyObject *IndexDatabase_bulk_ingest(IndexDatabaseObject *self,
                                           PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"registry", "skip_cfs", NULL};
    PyObject *registry_obj;
    PyObject *skip_cfs_obj = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", (char **)kwlist,
                                     &registry_obj, &skip_cfs_obj)) {
        return NULL;
    }

    SstArtifactRegistry *registry = sst_artifact_registry_get(registry_obj);
    if (!registry) {
        PyErr_SetString(PyExc_TypeError,
                        "expected an SstArtifactRegistry instance");
        return NULL;
    }

    std::unordered_set<std::string> skip_cfs;
    if (skip_cfs_obj && skip_cfs_obj != Py_None) {
        PyObject *seq =
            PySequence_Fast(skip_cfs_obj, "skip_cfs must be an iterable");
        if (!seq) return NULL;
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
            const char *s = PyUnicode_AsUTF8(item);
            if (!s) {
                Py_DECREF(seq);
                return NULL;
            }
            skip_cfs.emplace(s);
        }
        Py_DECREF(seq);
    }

    if (!run_blocking([&] { self->db->bulk_ingest(*registry, skip_cfs); }))
        return NULL;
    Py_RETURN_NONE;
}

static PyObject *IndexDatabase_write_agg_file_markers(IndexDatabaseObject *self,
                                                      PyObject *args) {
    PyObject *ids_obj;
    if (!PyArg_ParseTuple(args, "O", &ids_obj)) return NULL;

    PyObject *seq = PySequence_Fast(ids_obj, "file_ids must be an iterable");
    if (!seq) return NULL;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    std::vector<int> file_ids;
    file_ids.reserve(static_cast<std::size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
        long v = PyLong_AsLong(item);
        if (v == -1 && PyErr_Occurred()) {
            Py_DECREF(seq);
            return NULL;
        }
        file_ids.push_back(static_cast<int>(v));
    }
    Py_DECREF(seq);

    if (!run_blocking([&] { self->db->write_agg_file_markers(file_ids); }))
        return NULL;
    Py_RETURN_NONE;
}

static PyObject *IndexDatabase_write_agg_global_config(
    IndexDatabaseObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"time_interval_us", "config_hash", NULL};
    unsigned long long time_interval_us = 0;
    unsigned int config_hash = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "K|I", (char **)kwlist,
                                     &time_interval_us, &config_hash)) {
        return NULL;
    }
    if (!run_blocking([&] {
            self->db->write_agg_global_config(
                static_cast<std::uint64_t>(time_interval_us),
                static_cast<std::uint32_t>(config_hash));
        })) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *IndexDatabase_write_aggregation_tracker(
    IndexDatabaseObject *self, PyObject *args) {
    PyObject *blobs_obj;
    if (!PyArg_ParseTuple(args, "O", &blobs_obj)) return NULL;
    PyObject *seq = PySequence_Fast(blobs_obj, "blobs must be an iterable");
    if (!seq) return NULL;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    std::vector<std::string> blobs;
    blobs.reserve(static_cast<std::size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
        if (item == Py_None) continue;
        char *buf = nullptr;
        Py_ssize_t len = 0;
        if (PyBytes_Check(item)) {
            if (PyBytes_AsStringAndSize(item, &buf, &len) < 0) {
                Py_DECREF(seq);
                return NULL;
            }
        } else {
            Py_DECREF(seq);
            PyErr_SetString(PyExc_TypeError,
                            "blobs entries must be bytes or None");
            return NULL;
        }
        if (len > 0) blobs.emplace_back(buf, static_cast<std::size_t>(len));
    }
    Py_DECREF(seq);
    if (!run_blocking([&] { self->db->write_aggregation_tracker(blobs); }))
        return NULL;
    Py_RETURN_NONE;
}

static PyObject *IndexDatabase_rebuild_root_summaries(IndexDatabaseObject *self,
                                                      PyObject * /*ignored*/) {
    if (!run_blocking([&] { self->db->rebuild_root_summaries(); })) return NULL;
    Py_RETURN_NONE;
}

static PyObject *build_str_list(const std::vector<std::string> &v) {
    PyObject *lst = PyList_New(static_cast<Py_ssize_t>(v.size()));
    if (!lst) return NULL;
    for (Py_ssize_t i = 0; i < static_cast<Py_ssize_t>(v.size()); ++i) {
        PyObject *s =
            PyUnicode_FromString(v[static_cast<std::size_t>(i)].c_str());
        if (!s) {
            Py_DECREF(lst);
            return NULL;
        }
        PyList_SET_ITEM(lst, i, s);
    }
    return lst;
}

static PyObject *IndexDatabase_find_stale_files(IndexDatabaseObject *self,
                                                PyObject *args,
                                                PyObject *kwds) {
    static const char *kwlist[] = {"paths", NULL};
    PyObject *paths_obj;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", (char **)kwlist,
                                     &paths_obj)) {
        return NULL;
    }
    std::vector<std::string> paths;
    if (!parse_str_list(paths_obj, "paths", paths)) return NULL;

    IndexDatabase::StaleCheckResult result;
    if (!run_blocking_r([&] { return self->db->find_stale_files(paths); },
                        result)) {
        return NULL;
    }

    PyObject *changed = build_str_list(result.changed);
    PyObject *added = build_str_list(result.added);
    PyObject *removed = build_str_list(result.removed);
    PyObject *d = PyDict_New();
    if (!changed || !added || !removed || !d) {
        Py_XDECREF(changed);
        Py_XDECREF(added);
        Py_XDECREF(removed);
        Py_XDECREF(d);
        return NULL;
    }
    PyObject *so = PyBool_FromLong(result.schema_outdated);
    PyObject *st = PyBool_FromLong(result.stale());
    PyDict_SetItemString(d, "changed", changed);
    PyDict_SetItemString(d, "added", added);
    PyDict_SetItemString(d, "removed", removed);
    PyDict_SetItemString(d, "schema_outdated", so);
    PyDict_SetItemString(d, "stale", st);
    Py_DECREF(changed);
    Py_DECREF(added);
    Py_DECREF(removed);
    Py_DECREF(so);
    Py_DECREF(st);
    return d;
}

static PyMethodDef IndexDatabase_methods[] = {
    {"init_schema", (PyCFunction)IndexDatabase_init_schema, METH_NOARGS,
     "Idempotently initialise the schema version key."},
    {"register_files", (PyCFunction)IndexDatabase_register_files,
     METH_VARARGS | METH_KEYWORDS,
     "register_files(paths, build_manifest=False) -> list[int]\n"
     "Register each path in the DEFAULT-CF file registry and return the "
     "assigned file_ids. Idempotent for files with matching hash."},
    {"find_stale_files", (PyCFunction)IndexDatabase_find_stale_files,
     METH_VARARGS | METH_KEYWORDS,
     "find_stale_files(paths) -> dict\n"
     "Stat-only (mtime + size) staleness check of the given trace paths "
     "against the index. Returns {changed, added, removed, schema_outdated, "
     "stale}."},
    {"reserve_file_id_range", (PyCFunction)IndexDatabase_reserve_file_id_range,
     METH_VARARGS,
     "reserve_file_id_range(count) -> int\n"
     "Atomically reserve `count` contiguous file_ids, return the first."},
    {"bulk_ingest", (PyCFunction)IndexDatabase_bulk_ingest,
     METH_VARARGS | METH_KEYWORDS,
     "bulk_ingest(registry, skip_cfs=None) -> None\n"
     "Ingest all SSTs collected in the SstArtifactRegistry.\n"
     "skip_cfs is an optional iterable of CF names whose SSTs are left "
     "outside the unified DB (used by distributed builds to keep "
     "AGGREGATION/SYSTEM_METRICS SSTs addressable by manifest)."},
    {"rebuild_root_summaries",
     (PyCFunction)IndexDatabase_rebuild_root_summaries, METH_NOARGS,
     "Recompute ROOT_* summary column families from per-file CFs."},
    {"write_agg_global_config",
     (PyCFunction)IndexDatabase_write_agg_global_config,
     METH_VARARGS | METH_KEYWORDS,
     "write_agg_global_config(time_interval_us, config_hash=0) -> None\n"
     "Write the AGG_GLOBAL_CONFIG_KEY marker into the AGGREGATION CF. "
     "Required for `iter_arrow_dfanalyzer_all` on distributed builds "
     "(which never materialise the key via worker SSTs) or "
     "post-consolidate indices."},
    {"write_agg_file_markers",
     (PyCFunction)IndexDatabase_write_agg_file_markers, METH_VARARGS,
     "write_agg_file_markers(file_ids) -> None\n"
     "Write per-file aggregation completion markers (\\xFF\\xFF + file_id) "
     "into the AGGREGATION CF. Required after distributed_index otherwise "
     "`ensure_indexed()` concludes aggregation is incomplete and re-runs "
     "the entire build."},
    {"write_aggregation_tracker",
     (PyCFunction)IndexDatabase_write_aggregation_tracker, METH_VARARGS,
     "write_aggregation_tracker(blobs) -> None\n"
     "Merge a list of serialized AssociationTracker bytes and write the "
     "result to the AGGREGATION CF under the `__tracker__` key."},
    {NULL}};

PyTypeObject IndexDatabaseType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext.IndexDatabase",
    sizeof(IndexDatabaseObject),
    0,
    (destructor)IndexDatabase_dealloc,
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
    "Handle to a .dftindex RocksDB store.",
    0,
    0,
    0,
    0,
    0,
    0,
    IndexDatabase_methods,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    (initproc)IndexDatabase_init,
    0,
    IndexDatabase_new,
};

int init_index_database(PyObject *m) {
    if (register_type(m, &IndexDatabaseType, "IndexDatabase") < 0) return -1;
    return 0;
}
