#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/python/indexer.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_str_helpers.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <structmember.h>

#include <cstring>
#include <memory>

static void CheckpointIndexer_dealloc(CheckpointIndexerObject *self) {
    if (self->handle) {
        // The Python wrapper owns only the native indexer handle. The
        // underlying RocksDB instance remains manager-owned and may continue to
        // live process-wide for the same .dftindex path.
        delete self->handle;
        self->handle = NULL;
    }
    Py_XDECREF(self->gz_path);
    Py_XDECREF(self->index_path);
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static void CheckpointIndexer_release_handle(CheckpointIndexerObject *self) {
    if (self->handle) {
        // Releasing the handle drops this wrapper's native indexer state only.
        // Shared RocksDB lifetime is managed separately by RocksDBManager.
        delete self->handle;
        self->handle = NULL;
    }
}

static PyObject *CheckpointIndexer_new(PyTypeObject *type, PyObject *,
                                       PyObject *) {
    CheckpointIndexerObject *self;
    self = (CheckpointIndexerObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->handle = NULL;
        self->gz_path = NULL;
        self->index_path = NULL;
        self->checkpoint_size = 0;
        self->build_bloom = 0;
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int CheckpointIndexer_init(CheckpointIndexerObject *self, PyObject *args,
                                  PyObject *kwds) {
    static const char *kwlist[] = {"gz_path",
                                   "index_path",
                                   "checkpoint_size",
                                   "force_rebuild",
                                   "build_bloom",
                                   "runtime",
                                   NULL};
    const char *gz_path;
    const char *index_path = NULL;
    std::uint64_t checkpoint_size =
        dftracer::utils::constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    int force_rebuild = 0;
    int build_bloom = 0;
    PyObject *runtime_arg = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "s|snppO", const_cast<char **>(kwlist), &gz_path,
            &index_path, &checkpoint_size, &force_rebuild, &build_bloom,
            &runtime_arg)) {
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
        const std::string resolved_index_path =
            dftracer::utils::trace::internal::determine_index_path(gz_path, "");
        self->index_path = PyUnicode_FromString(resolved_index_path.c_str());
    }

    if (!self->index_path) {
        Py_DECREF(self->gz_path);
        return -1;
    }

    self->checkpoint_size = checkpoint_size;
    self->build_bloom = build_bloom;

    const char *index_path_str = as_utf8(self->index_path);
    if (!index_path_str) {
        return -1;
    }

    try {
        auto indexer =
            dftracer::utils::index::gzip::CheckpointIndexerFactory::create(
                gz_path, index_path_str, checkpoint_size, force_rebuild != 0);
        if (!indexer) {
            PyErr_SetString(PyExc_RuntimeError, "Unsupported archive format");
            return -1;
        }
        self->handle = new std::shared_ptr<
            dftracer::utils::index::gzip::CheckpointIndexer>(
            std::move(indexer));
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return -1;
    }

    return 0;
}

static dftracer::utils::Runtime *get_indexer_runtime(
    CheckpointIndexerObject *self) {
    if (self->runtime_obj) {
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    }
    return dftracer::utils::python::get_default_runtime();
}

static PyObject *CheckpointIndexer_build(CheckpointIndexerObject *self,
                                         PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    // Use BatchBuilder when bloom is requested; otherwise build checkpoints
    // only.
    if (self->build_bloom) {
        using namespace dftracer::utils;
        using namespace dftracer::utils::index::build;
        using namespace dftracer::utils::index::store;

        const char *gz = as_utf8(self->gz_path);
        const char *idx = as_utf8(self->index_path);
        if (!gz || !idx) {
            return NULL;
        }

        auto batch_config = std::make_shared<IndexBuildBatchConfig>();
        batch_config->file_paths.emplace_back(gz);
        batch_config->checkpoint_size =
            static_cast<std::size_t>(self->checkpoint_size);
        batch_config->parallelism = 1;

        std::string idx_str(idx);
        auto pos = idx_str.find_last_of('/');
        if (pos != std::string::npos) {
            batch_config->index_dir = idx_str.substr(0, pos);
        }

        Runtime *rt = get_indexer_runtime(self);
        IndexBuildBatchResult batch_result;

        if (!run_blocking([&] {
                rt->submit(run_coro_scope(
                               rt->executor(),
                               [](CoroScope &scope,
                                  std::shared_ptr<IndexBuildBatchConfig> cfg,
                                  IndexBuildBatchResult *out)
                                   -> coro::CoroTask<void> {
                                   *out = co_await BatchBuilder::process(
                                       &scope, std::move(cfg));
                               },
                               batch_config, &batch_result),
                           "indexer-build")
                    .get();
            })) {
            return NULL;
        }

        if (batch_result.failed > 0 && !batch_result.results.empty()) {
            const auto &result = batch_result.results[0];
            if (!result.success) {
                PyErr_SetString(PyExc_RuntimeError,
                                result.error_message.c_str());
                return NULL;
            }
        }
    } else {
        // Simple checkpoint-only build
        std::string error;
        Py_BEGIN_ALLOW_THREADS try {
            (*self->handle)->build();
        } catch (const std::exception &e) {
            error = e.what();
        }
        Py_END_ALLOW_THREADS if (!error.empty()) {
            PyErr_SetString(PyExc_RuntimeError, error.c_str());
            return NULL;
        }
    }

    Py_RETURN_NONE;
}

static PyObject *CheckpointIndexer_need_rebuild(CheckpointIndexerObject *self,
                                                PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    try {
        return PyBool_FromLong((*self->handle)->need_rebuild() ? 1 : 0);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyObject *CheckpointIndexer_exists(CheckpointIndexerObject *self,
                                          PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    try {
        return PyBool_FromLong((*self->handle)->exists() ? 1 : 0);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyObject *CheckpointIndexer_get_max_bytes(CheckpointIndexerObject *self,
                                                 PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    try {
        return PyLong_FromUnsignedLongLong((*self->handle)->get_max_bytes());
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyObject *CheckpointIndexer_get_num_lines(CheckpointIndexerObject *self,
                                                 PyObject *Py_UNUSED(ignored)) {
    if (!self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "Indexer not initialized");
        return NULL;
    }

    try {
        return PyLong_FromUnsignedLongLong((*self->handle)->get_num_lines());
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyObject *CheckpointIndexer_has_bloom(CheckpointIndexerObject *self,
                                             void *) {
    const char *idx = as_utf8(self->index_path);
    const char *gz = as_utf8(self->gz_path);
    if (!idx || !gz) {
        Py_RETURN_FALSE;
    }
    try {
        using namespace dftracer::utils::index::build;
        using namespace dftracer::utils::index::store;
        using namespace dftracer::utils::index::store::internal;
        IndexDatabase db(
            idx, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        std::string logical = get_logical_path(gz);
        int fid = db.get_file_info_id(logical);
        if (fid >= 0 && db.pruning_tier_current(fid)) {
            Py_RETURN_TRUE;
        }
    } catch (...) {
    }
    Py_RETURN_FALSE;
}

static PyObject *CheckpointIndexer_gz_path(CheckpointIndexerObject *self,
                                           void *) {
    Py_INCREF(self->gz_path);
    return self->gz_path;
}

static PyObject *CheckpointIndexer_index_path(CheckpointIndexerObject *self,
                                              void *) {
    Py_INCREF(self->index_path);
    return self->index_path;
}

static PyObject *CheckpointIndexer_checkpoint_size(
    CheckpointIndexerObject *self, void *) {
    return PyLong_FromUnsignedLongLong(self->checkpoint_size);
}

static PyObject *CheckpointIndexer_enter(CheckpointIndexerObject *self,
                                         PyObject *Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *CheckpointIndexer_close(CheckpointIndexerObject *self,
                                         PyObject *Py_UNUSED(ignored)) {
    CheckpointIndexer_release_handle(self);
    Py_RETURN_NONE;
}

static PyObject *CheckpointIndexer_exit(CheckpointIndexerObject *self,
                                        PyObject *) {
    CheckpointIndexer_release_handle(self);
    Py_RETURN_NONE;
}

static PyMethodDef CheckpointIndexer_methods[] = {
    {"build", DFTU_PYCFUNCTION(CheckpointIndexer_build), METH_NOARGS,
     "build()\n"
     "--\n"
     "\n"
     "Build or rebuild the index.\n"},
    {"need_rebuild", DFTU_PYCFUNCTION(CheckpointIndexer_need_rebuild),
     METH_NOARGS, "Check if a rebuild is needed."},
    {"exists", DFTU_PYCFUNCTION(CheckpointIndexer_exists), METH_NOARGS,
     "Check if the .dftindex store exists."},
    {"get_max_bytes", DFTU_PYCFUNCTION(CheckpointIndexer_get_max_bytes),
     METH_NOARGS, "Get the maximum uncompressed bytes in the indexed file."},
    {"get_num_lines", DFTU_PYCFUNCTION(CheckpointIndexer_get_num_lines),
     METH_NOARGS, "Get the total number of lines in the indexed file."},
    {"close", DFTU_PYCFUNCTION(CheckpointIndexer_close), METH_NOARGS,
     "Release this Python wrapper's native indexer handle.\n"
     "\n"
     "The shared RocksDB instance for the same .dftindex path remains managed\n"
     "by the native RocksDBManager cache."},
    {"__enter__", DFTU_PYCFUNCTION(CheckpointIndexer_enter), METH_NOARGS,
     "Enter the runtime context for the with statement."},
    {"__exit__", DFTU_PYCFUNCTION(CheckpointIndexer_exit), METH_VARARGS,
     "Release this Python wrapper on context exit.\n"
     "\n"
     "This does not force-close the shared RocksDB instance for the same\n"
     ".dftindex path."},
    {NULL} /* Sentinel */
};

static PyGetSetDef CheckpointIndexer_getsetters[] = {
    {"gz_path", (getter)CheckpointIndexer_gz_path, NULL,
     "Path to the gzip file", NULL},
    {"index_path", (getter)CheckpointIndexer_index_path, NULL,
     "Path to the .dftindex store", NULL},
    {"checkpoint_size", (getter)CheckpointIndexer_checkpoint_size, NULL,
     "Checkpoint size in bytes", NULL},
    {"has_bloom", (getter)CheckpointIndexer_has_bloom, NULL,
     "Whether bloom data exists in index", NULL},
    {NULL} /* Sentinel */
};

PyTypeObject CheckpointIndexerType = {
    PyVarObject_HEAD_INIT(
        NULL, 0) "dftracer_utils_ext.CheckpointIndexer", /* tp_name */
    sizeof(CheckpointIndexerObject),                     /* tp_basicsize */
    0,                                                   /* tp_itemsize */
    (destructor)CheckpointIndexer_dealloc,               /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    0,                                        /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "CheckpointIndexer(gz_path, index_path=None, checkpoint_size=33554432, "
    "force_rebuild=False, build_bloom=False, runtime=None)\n"
    "--\n"
    "\n"
    "Checkpoint indexer for single-file checkpoint-level operations on a "
    "gzip trace.\n"
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
    "        index.\n"
    "    runtime (Runtime or None): Runtime instance for thread pool\n"
    "        control. If None, uses the default global Runtime.\n", /* tp_doc */
    0,                                /* tp_traverse */
    0,                                /* tp_clear */
    0,                                /* tp_richcompare */
    0,                                /* tp_weaklistoffset */
    0,                                /* tp_iter */
    0,                                /* tp_iternext */
    CheckpointIndexer_methods,        /* tp_methods */
    0,                                /* tp_members */
    CheckpointIndexer_getsetters,     /* tp_getset */
    0,                                /* tp_base */
    0,                                /* tp_dict */
    0,                                /* tp_descr_get */
    0,                                /* tp_descr_set */
    0,                                /* tp_dictoffset */
    (initproc)CheckpointIndexer_init, /* tp_init */
    0,                                /* tp_alloc */
    CheckpointIndexer_new,            /* tp_new */
};

int dftracer::utils::python::init_checkpoint_indexer(PyObject *m) {
    if (register_type(m, &CheckpointIndexerType, "CheckpointIndexer") < 0)
        return -1;

    return 0;
}
