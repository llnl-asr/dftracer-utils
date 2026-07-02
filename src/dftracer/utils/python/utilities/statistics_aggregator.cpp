#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/utilities/statistics_aggregator.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/trace_statistics.h>

#include <string>

using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::utilities::composites::dft::statistics;

DFTRACER_UTILS_RUNTIME_BACKED_SLOTS(StatisticsAggregator,
                                    StatisticsAggregatorObject)

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
    TraceStatistics stats;

    if (!run_blocking([&] {
            Runtime *rt = resolve_runtime(self);

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
        })) {
        return NULL;
    }

    PyObject *d = PyDict_New();
    if (!d) return NULL;

    int rc = 0;
    rc |= dict_set_str(d, "file_path", stats.file_path.c_str());
    rc |= dict_set_u64(d, "total_events", stats.total_events());
    rc |= dict_set_u64(d, "num_chunks", stats.num_chunks);
    rc |= dict_set_bool(d, "success", stats.success);
    rc |= dict_set_str(d, "error_message", stats.error_message.c_str());
    rc |= dict_set_f64(d, "time_span_seconds", stats.time_span_seconds());
    rc |= dict_set_f64(d, "duration_mean_us", stats.duration_mean_us());
    rc |= dict_set_f64(d, "duration_stddev_us", stats.duration_stddev_us());
    rc |= dict_set_size(d, "num_categories", stats.num_categories());
    rc |= dict_set_size(d, "num_unique_names", stats.num_unique_names());
    rc |= dict_set_size(d, "num_pid_tids", stats.num_pid_tids());
    rc |= dict_set_u64(d, "min_timestamp_us", stats.merged.min_timestamp_us);
    rc |= dict_set_u64(d, "max_timestamp_us", stats.merged.max_timestamp_us);

    if (rc != 0) {
        Py_DECREF(d);
        return NULL;
    }

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
    if (register_type(m, &StatisticsAggregatorType,
                      "StatisticsAggregatorUtility") < 0)
        return -1;

    return 0;
}
