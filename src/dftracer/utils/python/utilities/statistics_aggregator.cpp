#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/utilities/statistics_aggregator.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/trace_statistics.h>

#include <string>

using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::utilities::composites::dft::statistics;

static Runtime *get_runtime(StatisticsAggregatorObject *self) {
    if (self->runtime_obj)
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    return get_default_runtime();
}

static void StatisticsAggregator_dealloc(StatisticsAggregatorObject *self) {
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *StatisticsAggregator_new(PyTypeObject *type, PyObject *args,
                                          PyObject *kwds) {
    StatisticsAggregatorObject *self =
        (StatisticsAggregatorObject *)type->tp_alloc(type, 0);
    if (self) {
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int StatisticsAggregator_init(StatisticsAggregatorObject *self,
                                     PyObject *args, PyObject *kwds) {
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

static PyObject *StatisticsAggregator_compute(StatisticsAggregatorObject *self,
                                              PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"file_path", "index_dir", NULL};
    const char *file_path;
    const char *index_dir = "";
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|s", (char **)kwlist,
                                     &file_path, &index_dir))
        return NULL;

    std::string file_path_str(file_path);
    std::string index_dir_str(index_dir);
    std::string error_msg;
    TraceStatistics stats;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);

        StatisticsAggregatorInput input;
        input.file_path = file_path_str;
        input.index_dir = index_dir_str;
        input.index_path = dftracer::utils::utilities::composites::dft::
            internal::determine_index_path(file_path_str, index_dir_str);

        auto *stats_p = &stats;
        auto input_copy = input;
        auto task = [stats_p, input_copy]() -> CoroTask<void> {
            StatisticsAggregatorUtility util;
            *stats_p = co_await util.process(input_copy);
        };
        rt->submit(task(), "stats-aggregator").get();
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

    SET_STR("file_path", stats.file_path.c_str());
    SET_ULL("total_events", stats.total_events());
    SET_ULL("num_chunks", stats.num_chunks);
    SET_BOOL("success", stats.success);
    SET_STR("error_message", stats.error_message.c_str());
    SET_DBL("time_span_seconds", stats.time_span_seconds());
    SET_DBL("duration_mean_us", stats.duration_mean_us());
    SET_DBL("duration_stddev_us", stats.duration_stddev_us());
    SET_SZT("num_categories", stats.num_categories());
    SET_SZT("num_unique_names", stats.num_unique_names());
    SET_SZT("num_pid_tids", stats.num_pid_tids());
    SET_ULL("min_timestamp_us", stats.merged.min_timestamp_us);
    SET_ULL("max_timestamp_us", stats.merged.max_timestamp_us);

#undef SET_STR
#undef SET_ULL
#undef SET_SZT
#undef SET_DBL
#undef SET_BOOL

    return d;
}

static PyObject *StatisticsAggregator_call(PyObject *self, PyObject *args,
                                           PyObject *kwds) {
    return StatisticsAggregator_compute((StatisticsAggregatorObject *)self,
                                        args, kwds);
}

static PyMethodDef StatisticsAggregator_methods[] = {
    {"process", (PyCFunction)StatisticsAggregator_compute,
     METH_VARARGS | METH_KEYWORDS,
     "process(file_path, index_dir='')\n"
     "--\n"
     "\n"
     "Compute aggregated statistics from a trace file.\n"
     "\n"
     "Args:\n"
     "    file_path (str): Path to the trace file.\n"
     "    index_dir (str): Directory for .dftindex stores (default '').\n"
     "\n"
     "Returns:\n"
     "    dict: Aggregated statistics.\n"},
    {NULL}};

PyTypeObject StatisticsAggregatorType = {
    PyVarObject_HEAD_INIT(
        NULL, 0) "dftracer_utils_ext.StatisticsAggregatorUtility", /* tp_name */
    sizeof(StatisticsAggregatorObject),       /* tp_basicsize */
    0,                                        /* tp_itemsize */
    (destructor)StatisticsAggregator_dealloc, /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    StatisticsAggregator_call,                /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "StatisticsAggregatorUtility(runtime: Runtime | None = None)\n"
    "--\n\n"
    "Aggregate statistics from an indexed trace file.\n\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool control.\n",
    0,                                   /* tp_traverse */
    0,                                   /* tp_clear */
    0,                                   /* tp_richcompare */
    0,                                   /* tp_weaklistoffset */
    0,                                   /* tp_iter */
    0,                                   /* tp_iternext */
    StatisticsAggregator_methods,        /* tp_methods */
    0,                                   /* tp_members */
    0,                                   /* tp_getset */
    0,                                   /* tp_base */
    0,                                   /* tp_dict */
    0,                                   /* tp_descr_get */
    0,                                   /* tp_descr_set */
    0,                                   /* tp_dictoffset */
    (initproc)StatisticsAggregator_init, /* tp_init */
    0,                                   /* tp_alloc */
    StatisticsAggregator_new,            /* tp_new */
};

int init_statistics_aggregator(PyObject *m) {
    if (PyType_Ready(&StatisticsAggregatorType) < 0) return -1;

    Py_INCREF(&StatisticsAggregatorType);
    if (PyModule_AddObject(m, "StatisticsAggregatorUtility",
                           (PyObject *)&StatisticsAggregatorType) < 0) {
        Py_DECREF(&StatisticsAggregatorType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
