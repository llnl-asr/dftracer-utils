#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/indexer.h>
#include <dftracer/utils/python/indexer_checkpoint.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <structmember.h>

#include <cstring>

static void Indexer_dealloc(IndexerObject *self) {
    if (self->handle) {
        // The Python wrapper owns only the native indexer handle. The
        // underlying RocksDB instance remains manager-owned and may continue to
        // live process-wide for the same .dftindex path.
        dft_indexer_destroy(self->handle);
        self->handle = NULL;
    }
    Py_XDECREF(self->gz_path);
    Py_XDECREF(self->index_path);
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static void Indexer_release_handle(IndexerObject *self) {
    if (self->handle) {
        // Releasing the handle drops this wrapper's native indexer state only.
        // Shared RocksDB lifetime is managed separately by RocksDBManager.
        dft_indexer_destroy(self->handle);
        self->handle = NULL;
    }
}

static PyObject *Indexer_new(PyTypeObject *type, PyObject *args,
                             PyObject *kwds) {
    IndexerObject *self;
    self = (IndexerObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->handle = NULL;
        self->gz_path = NULL;
        self->index_path = NULL;
        self->checkpoint_size = 0;
        self->build_bloom = 0;
        self->build_manifest = 0;
        self->index_threshold =
            dftracer::utils::constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD;
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int Indexer_init(IndexerObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {
        "gz_path",         "index_path",  "checkpoint_size",
        "force_rebuild",   "build_bloom", "build_manifest",
        "index_threshold", "runtime",     NULL};
    const char *gz_path;
    const char *index_path = NULL;
    std::uint64_t checkpoint_size =
        dftracer::utils::constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    int force_rebuild = 0;
    int build_bloom = 0;
    int build_manifest = 0;
    std::uint64_t index_threshold =
        dftracer::utils::constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD;
    PyObject *runtime_arg = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "s|snpppnO", (char **)kwlist, &gz_path, &index_path,
            &checkpoint_size, &force_rebuild, &build_bloom, &build_manifest,
            &index_threshold, &runtime_arg)) {
        return -1;
    }

    if (runtime_arg && runtime_arg != Py_None) {
        if (PyObject_TypeCheck(runtime_arg, &RuntimeType)) {
            Py_INCREF(runtime_arg);
            self->runtime_obj = runtime_arg;
        } else {
            PyObject *native = PyObject_GetAttrString(runtime_arg, "_native");
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

    self->gz_path = PyUnicode_FromString(gz_path);
    if (!self->gz_path) {
        return -1;
    }

    if (index_path) {
        self->index_path = PyUnicode_FromString(index_path);
    } else {
        const std::string index_path = dftracer::utils::utilities::composites::
            dft::internal::determine_index_path(gz_path, "");
        self->index_path = PyUnicode_FromString(index_path.c_str());
    }

    if (!self->index_path) {
        Py_DECREF(self->gz_path);
        return -1;
    }

    self->checkpoint_size = checkpoint_size;
    self->build_bloom = build_bloom;
    self->build_manifest = build_manifest;
    self->index_threshold = index_threshold;

    const char *index_path_str = PyUnicode_AsUTF8(self->index_path);
    if (!index_path_str) {
        return -1;
    }

    self->handle = dft_indexer_create(gz_path, index_path_str, checkpoint_size,
                                      force_rebuild);
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create indexer");
        return -1;
    }

    return 0;
}

static dftracer::utils::Runtime *get_indexer_runtime(IndexerObject *self) {
    if (self->runtime_obj) {
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    }
    return get_default_runtime();
}

static PyObject *Indexer_build(IndexerObject *self,
                               PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    using namespace dftracer::utils;
    using namespace dftracer::utils::utilities::indexer;

    const char *gz = PyUnicode_AsUTF8(self->gz_path);
    const char *idx = PyUnicode_AsUTF8(self->index_path);
    if (!gz || !idx) {
        return NULL;
    }

    auto config = IndexBuildConfig::for_file(gz)
                      .with_checkpoint_size(
                          static_cast<std::size_t>(self->checkpoint_size))
                      .with_bloom(self->build_bloom != 0)
                      .with_manifest(self->build_manifest != 0)
                      .with_index_threshold(
                          static_cast<std::size_t>(self->index_threshold));

    std::string idx_str(idx);
    auto pos = idx_str.find_last_of('/');
    if (pos != std::string::npos) {
        config.with_index_dir(idx_str.substr(0, pos));
    }

    Runtime *rt = get_indexer_runtime(self);
    IndexBuildResult build_result;

    try {
        auto build_coro =
            [](IndexBuildConfig cfg) -> coro::CoroTask<IndexBuildResult> {
            IndexBuilderUtility builder;
            co_return co_await builder.process(cfg);
        };

        Py_BEGIN_ALLOW_THREADS auto handle =
            rt->submit(build_coro(config), "indexer-build");
        build_result = handle.get();
        Py_END_ALLOW_THREADS
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    if (!build_result.success) {
        PyErr_SetString(PyExc_RuntimeError, build_result.error_message.c_str());
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *Indexer_need_rebuild(IndexerObject *self,
                                      PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    int result = dft_indexer_need_rebuild(self->handle);
    return PyBool_FromLong(result);
}

static PyObject *Indexer_exists(IndexerObject *self,
                                PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    int result = dft_indexer_exists(self->handle);
    return PyBool_FromLong(result);
}

static PyObject *Indexer_get_max_bytes(IndexerObject *self,
                                       PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    uint64_t result = dft_indexer_get_max_bytes(self->handle);
    return PyLong_FromUnsignedLongLong(result);
}

static PyObject *Indexer_get_num_lines(IndexerObject *self,
                                       PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    uint64_t result = dft_indexer_get_num_lines(self->handle);
    return PyLong_FromUnsignedLongLong(result);
}

static PyObject *Indexer_find_checkpoint(IndexerObject *self, PyObject *args) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    std::size_t target_offset;
    if (!PyArg_ParseTuple(args, "n", &target_offset)) {
        return NULL;
    }

    dft_indexer_checkpoint_t checkpoint;
    int found =
        dft_indexer_find_checkpoint(self->handle, target_offset, &checkpoint);

    if (!found) {
        Py_RETURN_NONE;
    }

    // Create IndexerCheckpoint object
    IndexerCheckpointObject *cp_obj =
        (IndexerCheckpointObject *)IndexerCheckpoint_new(&IndexerCheckpointType,
                                                         NULL, NULL);
    if (!cp_obj) {
        return NULL;
    }

    cp_obj->checkpoint = checkpoint;
    return (PyObject *)cp_obj;
}

static PyObject *Indexer_get_checkpoints(IndexerObject *self,
                                         PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    dft_indexer_checkpoint_t *checkpoints = NULL;
    std::size_t count = 0;

    int result =
        dft_indexer_get_checkpoints(self->handle, &checkpoints, &count);
    if (result != 0 || !checkpoints) {
        dft_indexer_free_checkpoints(checkpoints, count);
        PyObject *list = PyList_New(0);
        return list;
    }

    PyObject *list = PyList_New(count);
    if (!list) {
        dft_indexer_free_checkpoints(checkpoints, count);
        return NULL;
    }

    for (std::size_t i = 0; i < count; i++) {
        IndexerCheckpointObject *cp_obj =
            (IndexerCheckpointObject *)IndexerCheckpoint_new(
                &IndexerCheckpointType, NULL, NULL);
        if (!cp_obj) {
            Py_DECREF(list);
            dft_indexer_free_checkpoints(checkpoints, count);
            return NULL;
        }
        cp_obj->checkpoint = checkpoints[i];
        PyList_SetItem(list, i, (PyObject *)cp_obj);
    }

    dft_indexer_free_checkpoints(checkpoints, count);
    return list;
}

static PyObject *Indexer_has_bloom(IndexerObject *self, void *closure) {
    const char *idx = PyUnicode_AsUTF8(self->index_path);
    const char *gz = PyUnicode_AsUTF8(self->gz_path);
    if (!idx || !gz) {
        Py_RETURN_FALSE;
    }
    try {
        using namespace dftracer::utils::utilities::indexer;
        using namespace dftracer::utils::utilities::indexer::internal;
        IndexDatabase db(
            idx, dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        std::string logical = get_logical_path(gz);
        int fid = db.get_file_info_id(logical);
        if (fid >= 0 && db.has_bloom_data(fid)) {
            Py_RETURN_TRUE;
        }
    } catch (...) {
    }
    Py_RETURN_FALSE;
}

static PyObject *Indexer_has_manifest(IndexerObject *self, void *closure) {
    const char *idx = PyUnicode_AsUTF8(self->index_path);
    const char *gz = PyUnicode_AsUTF8(self->gz_path);
    if (!idx || !gz) {
        Py_RETURN_FALSE;
    }
    try {
        using namespace dftracer::utils::utilities::indexer;
        using namespace dftracer::utils::utilities::indexer::internal;
        IndexDatabase db(
            idx, dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        std::string logical = get_logical_path(gz);
        int fid = db.get_file_info_id(logical);
        if (fid >= 0 && db.has_manifest_data(fid)) {
            Py_RETURN_TRUE;
        }
    } catch (...) {
    }
    Py_RETURN_FALSE;
}

static PyObject *Indexer_gz_path(IndexerObject *self, void *closure) {
    Py_INCREF(self->gz_path);
    return self->gz_path;
}

static PyObject *Indexer_index_path(IndexerObject *self, void *closure) {
    Py_INCREF(self->index_path);
    return self->index_path;
}

static PyObject *Indexer_checkpoint_size(IndexerObject *self, void *closure) {
    return PyLong_FromUnsignedLongLong(self->checkpoint_size);
}

static PyObject *Indexer_enter(IndexerObject *self,
                               PyObject *Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *Indexer_close(IndexerObject *self,
                               PyObject *Py_UNUSED(ignored)) {
    Indexer_release_handle(self);
    Py_RETURN_NONE;
}

static PyObject *Indexer_exit(IndexerObject *self, PyObject *args) {
    Indexer_release_handle(self);
    Py_RETURN_NONE;
}

static PyMethodDef Indexer_methods[] = {
    {"build", (PyCFunction)Indexer_build, METH_NOARGS,
     "build()\n"
     "--\n"
     "\n"
     "Build or rebuild the index.\n"},
    {"need_rebuild", (PyCFunction)Indexer_need_rebuild, METH_NOARGS,
     "Check if a rebuild is needed."},
    {"exists", (PyCFunction)Indexer_exists, METH_NOARGS,
     "Check if the .dftindex store exists."},
    {"get_max_bytes", (PyCFunction)Indexer_get_max_bytes, METH_NOARGS,
     "Get the maximum uncompressed bytes in the indexed file."},
    {"get_num_lines", (PyCFunction)Indexer_get_num_lines, METH_NOARGS,
     "Get the total number of lines in the indexed file."},
    {"find_checkpoint", (PyCFunction)Indexer_find_checkpoint, METH_VARARGS,
     "Find the best checkpoint for a given uncompressed offset.\n"
     "\n"
     "Args:\n"
     "    offset (int): Uncompressed byte offset.\n"},
    {"get_checkpoints", (PyCFunction)Indexer_get_checkpoints, METH_NOARGS,
     "Get all checkpoints for this file as a list."},
    {"close", (PyCFunction)Indexer_close, METH_NOARGS,
     "Release this Python wrapper's native indexer handle.\n"
     "\n"
     "The shared RocksDB instance for the same .dftindex path remains managed\n"
     "by the native RocksDBManager cache."},
    {"__enter__", (PyCFunction)Indexer_enter, METH_NOARGS,
     "Enter the runtime context for the with statement."},
    {"__exit__", (PyCFunction)Indexer_exit, METH_VARARGS,
     "Release this Python wrapper on context exit.\n"
     "\n"
     "This does not force-close the shared RocksDB instance for the same\n"
     ".dftindex path."},
    {NULL} /* Sentinel */
};

static PyGetSetDef Indexer_getsetters[] = {
    {"gz_path", (getter)Indexer_gz_path, NULL, "Path to the gzip file", NULL},
    {"index_path", (getter)Indexer_index_path, NULL,
     "Path to the .dftindex store", NULL},
    {"checkpoint_size", (getter)Indexer_checkpoint_size, NULL,
     "Checkpoint size in bytes", NULL},
    {"has_bloom", (getter)Indexer_has_bloom, NULL,
     "Whether bloom data exists in index", NULL},
    {"has_manifest", (getter)Indexer_has_manifest, NULL,
     "Whether manifest data exists in index", NULL},
    {NULL} /* Sentinel */
};

PyTypeObject IndexerType = {
    PyVarObject_HEAD_INIT(NULL, 0) "indexer.Indexer", /* tp_name */
    sizeof(IndexerObject),                            /* tp_basicsize */
    0,                                                /* tp_itemsize */
    (destructor)Indexer_dealloc,                      /* tp_dealloc */
    0,                                                /* tp_vectorcall_offset */
    0,                                                /* tp_getattr */
    0,                                                /* tp_setattr */
    0,                                                /* tp_as_async */
    0,                                                /* tp_repr */
    0,                                                /* tp_as_number */
    0,                                                /* tp_as_sequence */
    0,                                                /* tp_as_mapping */
    0,                                                /* tp_hash */
    0,                                                /* tp_call */
    0,                                                /* tp_str */
    0,                                                /* tp_getattro */
    0,                                                /* tp_setattro */
    0,                                                /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,         /* tp_flags */
    "Indexer(gz_path: str, index_path: str | None = None,\n"
    "       checkpoint_size: int = 1048576,\n"
    "       force_rebuild: bool = False, build_bloom: bool = False,\n"
    "       build_manifest: bool = False,\n"
    "       index_threshold: int = 1048576,\n"
    "       runtime: Runtime | None = None)\n"
    "--\n"
    "\n"
    "Indexer for creating and managing gzip trace index stores.\n"
    "\n"
    "Args:\n"
    "    gz_path (str): Path to the gzip trace file.\n"
    "    index_path (str or None): Path to the .dftindex store. If None,\n"
    "        uses the root-local \".dftindex\" next to gz_path.\n"
    "    checkpoint_size (int): Checkpoint size in bytes for index\n"
    "        building (default 1 MB).\n"
    "    force_rebuild (bool): If True, rebuild the index even if it\n"
    "        exists.\n"
    "    build_bloom (bool): If True, build bloom filter data in the\n"
    "        store.\n"
    "    build_manifest (bool): If True, build manifest data in the\n"
    "        store.\n"
    "    index_threshold (int): Skip indexing for files smaller than\n"
    "        this (default 1 MB).\n"
    "    runtime (Runtime or None): Runtime instance for thread pool\n"
    "        control. If None, uses the default global Runtime.\n", /* tp_doc */
    0,                      /* tp_traverse */
    0,                      /* tp_clear */
    0,                      /* tp_richcompare */
    0,                      /* tp_weaklistoffset */
    0,                      /* tp_iter */
    0,                      /* tp_iternext */
    Indexer_methods,        /* tp_methods */
    0,                      /* tp_members */
    Indexer_getsetters,     /* tp_getset */
    0,                      /* tp_base */
    0,                      /* tp_dict */
    0,                      /* tp_descr_get */
    0,                      /* tp_descr_set */
    0,                      /* tp_dictoffset */
    (initproc)Indexer_init, /* tp_init */
    0,                      /* tp_alloc */
    Indexer_new,            /* tp_new */
};

int init_indexer(PyObject *m) {
    if (PyType_Ready(&IndexerType) < 0) return -1;

    Py_INCREF(&IndexerType);
    if (PyModule_AddObject(m, "Indexer", (PyObject *)&IndexerType) < 0) {
        Py_DECREF(&IndexerType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
