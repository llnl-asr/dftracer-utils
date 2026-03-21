#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/utilities/bloom_query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_query_utility.h>

#include <string>
#include <unordered_map>
#include <vector>

using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::utilities::composites::dft::indexing;

static Runtime *get_runtime(BloomQueryObject *self) {
    if (self->runtime_obj)
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    return get_default_runtime();
}

static void BloomQuery_dealloc(BloomQueryObject *self) {
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *BloomQuery_new(PyTypeObject *type, PyObject *args,
                                PyObject *kwds) {
    BloomQueryObject *self = (BloomQueryObject *)type->tp_alloc(type, 0);
    if (self) {
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int BloomQuery_init(BloomQueryObject *self, PyObject *args,
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

static PyObject *BloomQuery_query(BloomQueryObject *self, PyObject *args,
                                  PyObject *kwds) {
    static const char *kwlist[] = {"file_path", "predicates", "index_dir",
                                   NULL};
    const char *file_path;
    PyObject *predicates_obj;
    const char *index_dir = "";

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO!|s", (char **)kwlist,
                                     &file_path, &PyDict_Type, &predicates_obj,
                                     &index_dir)) {
        return NULL;
    }

    std::unordered_map<std::string, std::vector<std::string>> predicates_map;

    PyObject *key, *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(predicates_obj, &pos, &key, &value)) {
        if (!PyUnicode_Check(key)) {
            PyErr_SetString(PyExc_TypeError, "predicates keys must be strings");
            return NULL;
        }
        if (!PyList_Check(value)) {
            PyErr_SetString(PyExc_TypeError,
                            "predicates values must be lists of strings");
            return NULL;
        }

        const char *k = PyUnicode_AsUTF8(key);
        if (!k) return NULL;

        std::vector<std::string> vals;
        Py_ssize_t n = PyList_GET_SIZE(value);
        vals.reserve(static_cast<std::size_t>(n));
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject *item = PyList_GET_ITEM(value, i);
            if (!PyUnicode_Check(item)) {
                PyErr_SetString(PyExc_TypeError,
                                "predicates values must be lists of strings");
                return NULL;
            }
            const char *s = PyUnicode_AsUTF8(item);
            if (!s) return NULL;
            vals.emplace_back(s);
        }
        predicates_map.emplace(k, std::move(vals));
    }

    std::string file_path_str(file_path);
    std::string error_msg;
    BloomQueryOutput output;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);

        BloomQueryInput input;
        input.file_path = file_path_str;
        input.idx_path = file_path_str + ".idx";
        input.predicates = std::move(predicates_map);

        auto *out_p = &output;
        auto input_copy = input;
        auto task = [out_p, input_copy]() -> CoroTask<void> {
            BloomQueryUtility util;
            *out_p = co_await util.process(input_copy);
        };
        rt->submit(task(), "bloom-query").get();
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

    PyObject *checkpoints_list = PyList_New(
        static_cast<Py_ssize_t>(output.candidate_checkpoints.size()));
    if (!checkpoints_list) return NULL;

    for (std::size_t i = 0; i < output.candidate_checkpoints.size(); ++i) {
        PyObject *v =
            PyLong_FromUnsignedLongLong(output.candidate_checkpoints[i]);
        if (!v) {
            Py_DECREF(checkpoints_list);
            return NULL;
        }
        PyList_SET_ITEM(checkpoints_list, static_cast<Py_ssize_t>(i), v);
    }

    PyObject *d = PyDict_New();
    if (!d) {
        Py_DECREF(checkpoints_list);
        return NULL;
    }

#define SET_BOOL(k, v)                                   \
    do {                                                 \
        PyObject *_v = PyBool_FromLong(v ? 1 : 0);       \
        if (!_v || PyDict_SetItemString(d, k, _v) < 0) { \
            Py_XDECREF(_v);                              \
            Py_DECREF(checkpoints_list);                 \
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
            Py_DECREF(checkpoints_list);                 \
            Py_DECREF(d);                                \
            return NULL;                                 \
        }                                                \
        Py_DECREF(_v);                                   \
    } while (0)

    SET_BOOL("file_may_match", output.file_may_match);
    SET_ULL("total_checkpoints", output.total_checkpoints);
    SET_BOOL("success", output.success);

    if (PyDict_SetItemString(d, "candidate_checkpoints", checkpoints_list) <
        0) {
        Py_DECREF(checkpoints_list);
        Py_DECREF(d);
        return NULL;
    }
    Py_DECREF(checkpoints_list);

#undef SET_BOOL
#undef SET_ULL

    return d;
}

static PyObject *BloomQuery_call(PyObject *self, PyObject *args,
                                 PyObject *kwds) {
    return BloomQuery_query((BloomQueryObject *)self, args, kwds);
}

static PyMethodDef BloomQuery_methods[] = {
    {"process", (PyCFunction)BloomQuery_query, METH_VARARGS | METH_KEYWORDS,
     "process(file_path, predicates, index_dir='')\n"
     "--\n"
     "\n"
     "Query bloom filters for matching checkpoints.\n"
     "\n"
     "Args:\n"
     "    file_path (str): Path to the trace file.\n"
     "    predicates (dict): Bloom dimension filters {dim: [values]}.\n"
     "    index_dir (str): Directory for index sidecars (default '').\n"
     "\n"
     "Returns:\n"
     "    dict: Matching checkpoints and file_may_match flag.\n"},
    {NULL}};

PyTypeObject BloomQueryType = {
    PyVarObject_HEAD_INIT(
        NULL, 0) "dftracer_utils_ext.BloomQueryUtility", /* tp_name */
    sizeof(BloomQueryObject),                            /* tp_basicsize */
    0,                                                   /* tp_itemsize */
    (destructor)BloomQuery_dealloc,                      /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    BloomQuery_call,                          /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "BloomQueryUtility(runtime: Runtime | None = None)\n"
    "--\n\n"
    "Query bloom filters in an index to find candidate checkpoints.\n\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool control.\n",
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    BloomQuery_methods,        /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)BloomQuery_init, /* tp_init */
    0,                         /* tp_alloc */
    BloomQuery_new,            /* tp_new */
};

int init_bloom_query(PyObject *m) {
    if (PyType_Ready(&BloomQueryType) < 0) return -1;

    Py_INCREF(&BloomQueryType);
    if (PyModule_AddObject(m, "BloomQueryUtility",
                           (PyObject *)&BloomQueryType) < 0) {
        Py_DECREF(&BloomQueryType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
