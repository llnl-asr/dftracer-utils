#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/utilities/metadata_collector.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>

#include <string>

using dftracer::utils::get_format_name;
using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::utilities::composites::dft;

static Runtime *get_runtime(MetadataCollectorObject *self) {
    if (self->runtime_obj)
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    return get_default_runtime();
}

static void MetadataCollector_dealloc(MetadataCollectorObject *self) {
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *MetadataCollector_new(PyTypeObject *type, PyObject *args,
                                       PyObject *kwds) {
    MetadataCollectorObject *self =
        (MetadataCollectorObject *)type->tp_alloc(type, 0);
    if (self) {
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int MetadataCollector_init(MetadataCollectorObject *self, PyObject *args,
                                  PyObject *kwds) {
    static const char *kwlist[] = {"runtime", NULL};
    PyObject *runtime_arg = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", (char **)kwlist,
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

    return 0;
}

static PyObject *MetadataCollector_collect(MetadataCollectorObject *self,
                                           PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"file_path", "index_dir", NULL};
    const char *file_path;
    const char *index_dir = "";
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|s", (char **)kwlist,
                                     &file_path, &index_dir))
        return NULL;

    std::string file_path_str(file_path);
    std::string error_msg;
    MetadataCollectorUtilityOutput output;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);

        MetadataCollectorUtilityInput input;
        input.file_path = file_path_str;
        input.idx_path = file_path_str + ".idx";

        auto *out_p = &output;
        auto input_copy = input;
        auto task = [out_p, input_copy]() -> CoroTask<void> {
            MetadataCollectorUtility util;
            *out_p = co_await util.process(input_copy);
        };
        rt->submit(task(), "metadata-collector").get();
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

    PyObject *d = PyDict_New();
    if (!d) return NULL;

#define SET_STR(k, v)                                    \
    do {                                                 \
        PyObject *_v = PyUnicode_FromString(v);          \
        if (!_v || PyDict_SetItemString(d, k, _v) < 0) { \
            Py_XDECREF(_v);                              \
            Py_DECREF(d);                                \
            return NULL;                                 \
        }                                                \
        Py_DECREF(_v);                                   \
    } while (0)

#define SET_DBL(k, v)                                    \
    do {                                                 \
        PyObject *_v = PyFloat_FromDouble(v);            \
        if (!_v || PyDict_SetItemString(d, k, _v) < 0) { \
            Py_XDECREF(_v);                              \
            Py_DECREF(d);                                \
            return NULL;                                 \
        }                                                \
        Py_DECREF(_v);                                   \
    } while (0)

#define SET_SZT(k, v)                                    \
    do {                                                 \
        PyObject *_v = PyLong_FromSize_t(v);             \
        if (!_v || PyDict_SetItemString(d, k, _v) < 0) { \
            Py_XDECREF(_v);                              \
            Py_DECREF(d);                                \
            return NULL;                                 \
        }                                                \
        Py_DECREF(_v);                                   \
    } while (0)

#define SET_ULL(k, v)                                    \
    do {                                                 \
        PyObject *_v = PyLong_FromUnsignedLongLong(v);   \
        if (!_v || PyDict_SetItemString(d, k, _v) < 0) { \
            Py_XDECREF(_v);                              \
            Py_DECREF(d);                                \
            return NULL;                                 \
        }                                                \
        Py_DECREF(_v);                                   \
    } while (0)

#define SET_BOOL(k, v)                                   \
    do {                                                 \
        PyObject *_v = PyBool_FromLong(v ? 1 : 0);       \
        if (!_v || PyDict_SetItemString(d, k, _v) < 0) { \
            Py_XDECREF(_v);                              \
            Py_DECREF(d);                                \
            return NULL;                                 \
        }                                                \
        Py_DECREF(_v);                                   \
    } while (0)

    SET_STR("file_path", output.file_path.c_str());
    SET_STR("idx_path", output.idx_path.c_str());
    SET_DBL("size_mb", output.size_mb);
    SET_SZT("start_line", output.start_line);
    SET_SZT("end_line", output.end_line);
    SET_SZT("valid_events", output.valid_events);
    SET_DBL("size_per_line", output.size_per_line);
    SET_BOOL("success", output.success);
    SET_BOOL("has_index", output.has_index);
    SET_BOOL("index_valid", output.index_valid);
    SET_ULL("compressed_size", output.compressed_size);
    SET_ULL("uncompressed_size", output.uncompressed_size);
    SET_ULL("num_lines", output.num_lines);
    SET_ULL("checkpoint_size", output.checkpoint_size);
    SET_SZT("num_checkpoints", output.num_checkpoints);
    SET_STR("format", get_format_name(output.format));
    SET_STR("error_message", output.error_message.c_str());

#undef SET_STR
#undef SET_DBL
#undef SET_SZT
#undef SET_ULL
#undef SET_BOOL

    return d;
}

static PyObject *MetadataCollector_call(PyObject *self, PyObject *args,
                                        PyObject *kwds) {
    return MetadataCollector_collect((MetadataCollectorObject *)self, args,
                                     kwds);
}

static PyMethodDef MetadataCollector_methods[] = {
    {"process", (PyCFunction)MetadataCollector_collect,
     METH_VARARGS | METH_KEYWORDS,
     "Collect metadata from a trace file.\n"
     "\n"
     "Args:\n"
     "    file_path (str): Path to the trace file.\n"
     "    index_dir (str): Directory for index sidecars.\n"},
    {NULL}};

PyTypeObject MetadataCollectorType = {
    PyVarObject_HEAD_INIT(
        NULL, 0) "dftracer_utils_ext.MetadataCollectorUtility", /* tp_name */
    sizeof(MetadataCollectorObject),          /* tp_basicsize */
    0,                                        /* tp_itemsize */
    (destructor)MetadataCollector_dealloc,    /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    MetadataCollector_call,                   /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "MetadataCollectorUtility(runtime: Runtime | None = None)\n"
    "--\n\n"
    "Collect metadata from a DFTracer trace file.\n\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool control.\n"
    "\n"
    "process(file_path, index_dir='') -> dict\n"
    "    file_path (str): Path to the trace file.\n"
    "    index_dir (str): Directory for index sidecar files.\n",
    0,                                /* tp_traverse */
    0,                                /* tp_clear */
    0,                                /* tp_richcompare */
    0,                                /* tp_weaklistoffset */
    0,                                /* tp_iter */
    0,                                /* tp_iternext */
    MetadataCollector_methods,        /* tp_methods */
    0,                                /* tp_members */
    0,                                /* tp_getset */
    0,                                /* tp_base */
    0,                                /* tp_dict */
    0,                                /* tp_descr_get */
    0,                                /* tp_descr_set */
    0,                                /* tp_dictoffset */
    (initproc)MetadataCollector_init, /* tp_init */
    0,                                /* tp_alloc */
    MetadataCollector_new,            /* tp_new */
};

int init_metadata_collector(PyObject *m) {
    if (PyType_Ready(&MetadataCollectorType) < 0) return -1;

    Py_INCREF(&MetadataCollectorType);
    if (PyModule_AddObject(m, "MetadataCollectorUtility",
                           (PyObject *)&MetadataCollectorType) < 0) {
        Py_DECREF(&MetadataCollectorType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
