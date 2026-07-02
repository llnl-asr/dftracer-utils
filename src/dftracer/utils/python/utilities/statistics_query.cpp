#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/py_runtime_mixin.h>
#include <dftracer/utils/python/py_type_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/utilities/statistics_query.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_query_utility.h>

#include <string>

using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::utilities::composites::dft::statistics;

DFTRACER_UTILS_RUNTIME_BACKED_SLOTS(StatisticsQuery, StatisticsQueryObject)

static PyObject *StatisticsQuery_query(StatisticsQueryObject *self,
                                       PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"file_path", "query_type", "top_n",
                                   "index_dir", NULL};
    const char *file_path;
    const char *query_type_str = "summary";
    Py_ssize_t top_n = 10;
    const char *index_dir = "";

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|sns", (char **)kwlist,
                                     &file_path, &query_type_str, &top_n,
                                     &index_dir)) {
        return NULL;
    }

    StatisticsQueryType qt;
    if (strcmp(query_type_str, "summary") == 0) {
        qt = StatisticsQueryType::SUMMARY;
    } else if (strcmp(query_type_str, "categories") == 0) {
        qt = StatisticsQueryType::CATEGORIES;
    } else if (strcmp(query_type_str, "names") == 0) {
        qt = StatisticsQueryType::NAMES;
    } else if (strcmp(query_type_str, "pid_tids") == 0) {
        qt = StatisticsQueryType::PID_TIDS;
    } else if (strcmp(query_type_str, "time_range") == 0) {
        qt = StatisticsQueryType::TIME_RANGE;
    } else if (strcmp(query_type_str, "duration_stats") == 0) {
        qt = StatisticsQueryType::DURATION_STATS;
    } else if (strcmp(query_type_str, "top_n_names") == 0) {
        qt = StatisticsQueryType::TOP_N_NAMES;
    } else if (strcmp(query_type_str, "top_n_categories") == 0) {
        qt = StatisticsQueryType::TOP_N_CATEGORIES;
    } else if (strcmp(query_type_str, "detailed") == 0) {
        qt = StatisticsQueryType::DETAILED;
    } else {
        PyErr_Format(PyExc_ValueError, "unknown query_type: '%s'",
                     query_type_str);
        return NULL;
    }

    std::string file_path_str(file_path);
    std::string index_dir_str(index_dir);
    TraceStatistics stats;
    StatisticsQueryOutput output;
    auto qt_copy = qt;
    auto top_n_copy = static_cast<std::uint64_t>(top_n);

    if (!run_blocking([&] {
            Runtime *rt = resolve_runtime(self);

            StatisticsAggregatorInput agg_input;
            agg_input.file_path = file_path_str;
            agg_input.index_dir = index_dir_str;
            agg_input.index_path = dftracer::utils::utilities::composites::dft::
                internal::determine_index_path(file_path_str, index_dir_str);

            auto *stats_p = &stats;
            auto agg_task = [stats_p, agg_input]() -> CoroTask<void> {
                StatisticsAggregatorUtility util;
                *stats_p = co_await util.process(agg_input);
            };
            rt->submit(agg_task(), "stats-agg").get();

            StatisticsQueryInput query_input;
            query_input.stats = std::move(stats);
            query_input.query_type = qt_copy;
            query_input.top_n = top_n_copy;

            auto *out_p = &output;
            auto query_task = [out_p, query_input]() -> CoroTask<void> {
                StatisticsQueryUtility util;
                *out_p = co_await util.process(query_input);
            };
            rt->submit(query_task(), "stats-query").get();
        })) {
        return NULL;
    }

    PyObject *results_list =
        PyList_New(static_cast<Py_ssize_t>(output.results.size()));
    if (!results_list) return NULL;

    for (std::size_t i = 0; i < output.results.size(); ++i) {
        PyObject *tup = PyTuple_New(2);
        if (!tup) {
            Py_DECREF(results_list);
            return NULL;
        }
        PyTuple_SET_ITEM(tup, 0,
                         PyUnicode_FromString(output.results[i].first.c_str()));
        PyTuple_SET_ITEM(tup, 1,
                         PyLong_FromUnsignedLongLong(output.results[i].second));
        PyList_SET_ITEM(results_list, static_cast<Py_ssize_t>(i), tup);
    }

    PyObject *d = PyDict_New();
    if (!d) {
        Py_DECREF(results_list);
        return NULL;
    }

    int rc = 0;
    rc |= dict_set_str(d, "query_type", output.query_type_name.c_str());
    rc |= dict_set_u64(d, "total_events", output.total_events);
    rc |= dict_set_u64(d, "min_timestamp_us", output.min_timestamp_us);
    rc |= dict_set_u64(d, "max_timestamp_us", output.max_timestamp_us);
    rc |= dict_set_f64(d, "time_span_seconds", output.time_span_seconds);
    rc |= dict_set_u64(d, "duration_count", output.duration_count);
    rc |= dict_set_f64(d, "duration_mean_us", output.duration_mean_us);
    rc |= dict_set_f64(d, "duration_stddev_us", output.duration_stddev_us);
    rc |= dict_set_u64(d, "duration_min_us", output.duration_min_us);
    rc |= dict_set_u64(d, "duration_max_us", output.duration_max_us);
    // Steals (and always releases) the results_list reference.
    rc |= dict_set_steal(d, "results", results_list);

    if (rc != 0) {
        Py_DECREF(d);
        return NULL;
    }

    return d;
}

static PyObject *StatisticsQuery_call(PyObject *self, PyObject *args,
                                      PyObject *kwds) {
    return StatisticsQuery_query((StatisticsQueryObject *)self, args, kwds);
}

static PyMethodDef StatisticsQuery_methods[] = {
    {"process", (PyCFunction)StatisticsQuery_query,
     METH_VARARGS | METH_KEYWORDS,
     "process(file_path, query_type='summary', top_n=10, index_dir='')\n"
     "--\n"
     "\n"
     "Query statistics from an indexed trace file.\n"
     "\n"
     "Args:\n"
     "    file_path (str): Path to the trace file.\n"
     "    query_type (str): Query type (default 'summary'). One of\n"
     "        'summary', 'categories', 'names', 'pid_tids',\n"
     "        'time_range', 'duration_stats', 'top_n_names',\n"
     "        'top_n_categories', 'detailed'.\n"
     "    top_n (int): Top results for ranked queries (default 10).\n"
     "    index_dir (str): Directory for .dftindex stores (default '').\n"
     "\n"
     "Returns:\n"
     "    dict: Query results.\n"},
    {NULL}};

PyTypeObject StatisticsQueryUtilityType = {
    PyVarObject_HEAD_INIT(
        NULL, 0) "dftracer_utils_ext.StatisticsQueryUtility", /* tp_name */
    sizeof(StatisticsQueryObject),                            /* tp_basicsize */
    0,                                                        /* tp_itemsize */
    (destructor)StatisticsQuery_dealloc,                      /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    StatisticsQuery_call,                     /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "StatisticsQueryUtility(runtime: Runtime | None = None)\n"
    "--\n\n"
    "Query pre-computed statistics from an indexed trace file.\n\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool control.\n",
    0,                              /* tp_traverse */
    0,                              /* tp_clear */
    0,                              /* tp_richcompare */
    0,                              /* tp_weaklistoffset */
    0,                              /* tp_iter */
    0,                              /* tp_iternext */
    StatisticsQuery_methods,        /* tp_methods */
    0,                              /* tp_members */
    0,                              /* tp_getset */
    0,                              /* tp_base */
    0,                              /* tp_dict */
    0,                              /* tp_descr_get */
    0,                              /* tp_descr_set */
    0,                              /* tp_dictoffset */
    (initproc)StatisticsQuery_init, /* tp_init */
    0,                              /* tp_alloc */
    StatisticsQuery_new,            /* tp_new */
};

int init_statistics_query(PyObject *m) {
    if (register_type(m, &StatisticsQueryUtilityType,
                      "StatisticsQueryUtility") < 0)
        return -1;

    return 0;
}
