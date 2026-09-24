#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/association_tracker.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/python/index_database.h>
#include <dftracer/utils/python/py_agg_config.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_list_helpers.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/sst_distribution.h>

#include <new>
#include <string>
#include <vector>

using dftracer::utils::index::store::IndexDatabase;
using dftracer::utils::index::store::SstArtifactRegistry;

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
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "s", const_cast<char **>(kwlist), &index_path)) {
        return -1;
    }
    try {
        self->db = std::make_shared<IndexDatabase>(index_path);
    } catch (const std::exception &e) {
        dftracer::utils::python::set_typed_py_error(e);
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

static PyObject *IndexDatabase_assign_file_ids(IndexDatabaseObject *self,
                                               PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"paths", NULL};
    PyObject *paths_obj;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O",
                                     const_cast<char **>(kwlist), &paths_obj)) {
        return NULL;
    }
    std::vector<std::string> paths;
    if (!parse_str_list(paths_obj, "paths", paths)) return NULL;

    std::vector<int> ids;
    if (!run_blocking_r([&] { return self->db->assign_file_ids(paths); },
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
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O",
                                     const_cast<char **>(kwlist), &registry_obj,
                                     &skip_cfs_obj)) {
        return NULL;
    }

    SstArtifactRegistry *registry =
        dftracer::utils::python::sst_artifact_registry_get(registry_obj);
    if (!registry) {
        PyErr_SetString(PyExc_TypeError,
                        "expected an SstArtifactRegistry instance");
        return NULL;
    }

    dftracer::utils::StringViewSet skip_cfs;
    if (skip_cfs_obj && skip_cfs_obj != Py_None) {
        PyObject *seq =
            PySequence_Fast(skip_cfs_obj, "skip_cfs must be an iterable");
        if (!seq) return NULL;
        Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
            const char *s = as_utf8(item);
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

static PyObject *IndexDatabase_write_agg_config(IndexDatabaseObject *self,
                                                PyObject *args) {
    PyObject *config_obj;
    if (!PyArg_ParseTuple(args, "O", &config_obj)) return NULL;
    const auto config = dftracer::utils::python::agg_config_from_py(config_obj);
    if (!run_blocking([&] {
            dftracer::utils::index::schemas::dft::agg::tier::write_config(
                *self->db->db(), {config.time_interval_us, config.params_hash(),
                                  config.group_by_file});
        }))
        return NULL;
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
    if (!run_blocking([&] {
            using dftracer::utils::index::schemas::dft::agg::AssociationTracker;
            AssociationTracker unified;
            for (const auto &b : blobs)
                unified.merge(AssociationTracker::deserialize(b));
            unified.finalize();
            dftracer::utils::index::schemas::dft::agg::tier::write_tracker(
                *self->db->db(), unified.serialize());
        }))
        return NULL;
    Py_RETURN_NONE;
}

static PyObject *IndexDatabase_find_stale_files(IndexDatabaseObject *self,
                                                PyObject *args,
                                                PyObject *kwds) {
    static const char *kwlist[] = {"paths", NULL};
    PyObject *paths_obj;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O",
                                     const_cast<char **>(kwlist), &paths_obj)) {
        return NULL;
    }
    std::vector<std::string> paths;
    if (!parse_str_list(paths_obj, "paths", paths)) return NULL;

    IndexDatabase::StaleCheckResult result;
    if (!run_blocking_r([&] { return self->db->find_stale_files(paths); },
                        result)) {
        return NULL;
    }

    PyObject *changed = str_list_from(result.changed);
    PyObject *added = str_list_from(result.added);
    PyObject *removed = str_list_from(result.removed);
    PyObject *d = PyDict_New();
    if (!changed || !added || !removed || !d) {
        Py_XDECREF(changed);
        Py_XDECREF(added);
        Py_XDECREF(removed);
        Py_XDECREF(d);
        return NULL;
    }
    PyObject *so = PyBool_FromLong(result.format_outdated);
    PyObject *st = PyBool_FromLong(result.stale());
    PyDict_SetItemString(d, "changed", changed);
    PyDict_SetItemString(d, "added", added);
    PyDict_SetItemString(d, "removed", removed);
    PyDict_SetItemString(d, "format_outdated", so);
    PyDict_SetItemString(d, "stale", st);
    Py_DECREF(changed);
    Py_DECREF(added);
    Py_DECREF(removed);
    Py_DECREF(so);
    Py_DECREF(st);
    return d;
}

static PyMethodDef IndexDatabase_methods[] = {
    {"init_schema", DFTU_PYCFUNCTION(IndexDatabase_init_schema), METH_NOARGS,
     "Idempotently write the format version and extension registry."},
    {"assign_file_ids", DFTU_PYCFUNCTION(IndexDatabase_assign_file_ids),
     METH_VARARGS | METH_KEYWORDS,
     "assign_file_ids(paths) -> list[int]\n"
     "The file_id of each path: its registered id, or a newly reserved one. "
     "The file record is written with the file's index data."},
    {"find_stale_files", DFTU_PYCFUNCTION(IndexDatabase_find_stale_files),
     METH_VARARGS | METH_KEYWORDS,
     "find_stale_files(paths) -> dict\n"
     "Stat-only (mtime + size) staleness check of the given trace paths "
     "against the index. Returns {changed, added, removed, format_outdated, "
     "stale}."},
    {"reserve_file_id_range",
     DFTU_PYCFUNCTION(IndexDatabase_reserve_file_id_range), METH_VARARGS,
     "reserve_file_id_range(count) -> int\n"
     "Atomically reserve `count` contiguous file_ids, return the first."},
    {"bulk_ingest", DFTU_PYCFUNCTION(IndexDatabase_bulk_ingest),
     METH_VARARGS | METH_KEYWORDS,
     "bulk_ingest(registry, skip_cfs=None) -> None\n"
     "Ingest all SSTs collected in the SstArtifactRegistry.\n"
     "skip_cfs is an optional iterable of CF names whose SSTs are left "
     "outside the unified DB (used by distributed builds to keep "
     "AGGREGATION/SYSTEM_METRICS SSTs addressable by manifest)."},
    {"write_agg_config", DFTU_PYCFUNCTION(IndexDatabase_write_agg_config),
     METH_VARARGS,
     "write_agg_config(aggregation_config) -> None\n"
     "Record the aggregation config a distributed build aggregated its "
     "files with. Each file's dftracer.agg entry comes with its SSTs."},
    {"write_aggregation_tracker",
     DFTU_PYCFUNCTION(IndexDatabase_write_aggregation_tracker), METH_VARARGS,
     "write_aggregation_tracker(blobs) -> None\n"
     "Merge a list of serialized AssociationTracker bytes and write the "
     "result to the aggregation tier."},
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

int dftracer::utils::python::init_index_database(PyObject *m) {
    if (register_type(m, &IndexDatabaseType, "IndexDatabase") < 0) return -1;
    return 0;
}
