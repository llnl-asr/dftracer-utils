#define PY_SSIZE_T_CLEAN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/python/arrow_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#include <dftracer/utils/python/utilities/aggregator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregator_utility.h>

#include <string>
#include <vector>

using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::utilities::composites::dft::aggregators;

using dftracer::utils::python::wrap_arrow_result;
using dftracer::utils::python::wrap_arrow_table;

#ifdef DFTRACER_UTILS_ENABLE_ARROW
using dftracer::utils::utilities::common::arrow::ArrowExportResult;
#endif

static Runtime *get_runtime(AggregatorObject *self) {
    if (self->runtime_obj)
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    return get_default_runtime();
}

static void Aggregator_dealloc(AggregatorObject *self) {
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *Aggregator_new(PyTypeObject *type, PyObject *args,
                                PyObject *kwds) {
    AggregatorObject *self = (AggregatorObject *)type->tp_alloc(type, 0);
    if (self) {
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int Aggregator_init(AggregatorObject *self, PyObject *args,
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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int parse_str_list(PyObject *obj, std::vector<std::string> &out,
                          const char *param_name) {
    if (!obj || obj == Py_None) return 0;
    if (!PyList_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "%s must be a list of str", param_name);
        return -1;
    }
    Py_ssize_t n = PyList_Size(obj);
    for (Py_ssize_t i = 0; i < n; i++) {
        const char *s = PyUnicode_AsUTF8(PyList_GetItem(obj, i));
        if (!s) return -1;
        out.emplace_back(s);
    }
    return 0;
}

static int parse_aggregator_args(PyObject *args, PyObject *kwds,
                                 AggregatorInput &input) {
    static const char *kwlist[] = {"directory",
                                   "time_interval_ms",
                                   "group_keys",
                                   "categories",
                                   "names",
                                   "index_dir",
                                   "checkpoint_size",
                                   "force_rebuild",
                                   "chunk_size_mb",
                                   "batch_size_mb",
                                   "event_batch_size",
                                   "custom_metric_fields",
                                   "compute_percentiles",
                                   NULL};

    const char *directory = NULL;
    double time_interval_ms = 5000.0;
    PyObject *group_keys_obj = Py_None;
    PyObject *categories_obj = Py_None;
    PyObject *names_obj = Py_None;
    const char *index_dir = "";
    Py_ssize_t checkpoint_size = 32 * 1024 * 1024;
    int force_rebuild = 0;
    Py_ssize_t chunk_size_mb = 64;
    Py_ssize_t batch_size_mb = 4;
    Py_ssize_t event_batch_size = 10000;
    PyObject *custom_metrics_obj = Py_None;
    int compute_percentiles = 0;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "s|dOOOsnpnnnOp", (char **)kwlist, &directory,
            &time_interval_ms, &group_keys_obj, &categories_obj, &names_obj,
            &index_dir, &checkpoint_size, &force_rebuild, &chunk_size_mb,
            &batch_size_mb, &event_batch_size, &custom_metrics_obj,
            &compute_percentiles))
        return -1;

    input.directory = directory;
    input.config.time_interval_us =
        static_cast<std::uint64_t>(time_interval_ms * 1000.0);
    input.index_dir = index_dir;
    input.checkpoint_size = static_cast<std::size_t>(checkpoint_size);
    input.force_rebuild = force_rebuild != 0;
    input.chunk_size_mb = static_cast<std::size_t>(chunk_size_mb);
    input.batch_size_mb = static_cast<std::size_t>(batch_size_mb);
    input.event_batch_size = static_cast<std::size_t>(event_batch_size);
    input.config.compute_percentiles = compute_percentiles != 0;

    if (parse_str_list(group_keys_obj, input.config.extra_group_keys,
                       "group_keys") < 0)
        return -1;
    if (parse_str_list(custom_metrics_obj, input.config.custom_metric_fields,
                       "custom_metric_fields") < 0)
        return -1;

    return 0;
}

static int run_aggregator_pipeline(AggregatorObject *self,
                                   const AggregatorInput &input,
                                   std::vector<AggregationBatch> &batches,
                                   std::string &error_msg) {
    auto *bp = &batches;
    AggregatorInput input_copy = input;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);
        auto task = [bp, input_copy]() -> CoroTask<void> {
            AggregatorUtility util;
            auto gen = util.process(input_copy);
            while (auto batch = co_await gen.next()) {
                bp->push_back(std::move(*batch));
            }
        };
        rt->submit(task(), "aggregator").get();
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        return error_msg.empty()
        ? 0
        : -1;
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#endif  // DFTRACER_UTILS_ENABLE_ARROW

// ---------------------------------------------------------------------------
// process() — returns ArrowTable (materialized)
// ---------------------------------------------------------------------------

static PyObject *Aggregator_process(AggregatorObject *self, PyObject *args,
                                    PyObject *kwds) {
    AggregatorInput input;
    if (parse_aggregator_args(args, kwds, input) < 0) return NULL;

    std::vector<AggregationBatch> batches;
    std::string error_msg;
    if (run_aggregator_pipeline(self, input, batches, error_msg) < 0) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    PyObject *batch_list = PyList_New(0);
    if (!batch_list) return NULL;

    for (const auto &batch : batches) {
        if (batch.entries.empty()) continue;

        auto arrow_result = batch.to_arrow();
        if (!arrow_result.valid()) continue;

        PyObject *cap = wrap_arrow_result(std::move(arrow_result));
        if (!cap) {
            Py_DECREF(batch_list);
            return NULL;
        }
        int rc = PyList_Append(batch_list, cap);
        Py_DECREF(cap);
        if (rc < 0) {
            Py_DECREF(batch_list);
            return NULL;
        }
    }

    return wrap_arrow_table(batch_list);
#else
    PyErr_SetString(PyExc_RuntimeError,
                    "dftracer-utils was built without Arrow support");
    return NULL;
#endif
}

// ---------------------------------------------------------------------------
// iter_arrow() — returns list iterator of ArrowBatch capsules
// ---------------------------------------------------------------------------

static PyObject *Aggregator_iter_arrow(AggregatorObject *self, PyObject *args,
                                       PyObject *kwds) {
    AggregatorInput input;
    if (parse_aggregator_args(args, kwds, input) < 0) return NULL;

    std::vector<AggregationBatch> batches;
    std::string error_msg;
    if (run_aggregator_pipeline(self, input, batches, error_msg) < 0) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    PyObject *batch_list = PyList_New(0);
    if (!batch_list) return NULL;

    for (const auto &batch : batches) {
        if (batch.entries.empty()) continue;

        auto arrow_result = batch.to_arrow();
        if (!arrow_result.valid()) continue;

        PyObject *cap = wrap_arrow_result(std::move(arrow_result));
        if (!cap) {
            Py_DECREF(batch_list);
            return NULL;
        }

        int rc = PyList_Append(batch_list, cap);
        Py_DECREF(cap);
        if (rc < 0) {
            Py_DECREF(batch_list);
            return NULL;
        }
    }

    PyObject *it = PyObject_GetIter(batch_list);
    Py_DECREF(batch_list);
    return it;
#else
    PyErr_SetString(PyExc_RuntimeError,
                    "dftracer-utils was built without Arrow support");
    return NULL;
#endif
}

static PyObject *Aggregator_call(PyObject *self, PyObject *args,
                                 PyObject *kwds) {
    return Aggregator_process((AggregatorObject *)self, args, kwds);
}

static PyMethodDef Aggregator_methods[] = {
    {"process", (PyCFunction)Aggregator_process, METH_VARARGS | METH_KEYWORDS,
     "process(directory, time_interval_ms=5000.0, group_keys=None,\n"
     "        categories=None, names=None, index_dir='',\n"
     "        checkpoint_size=33554432, force_rebuild=False,\n"
     "        chunk_size_mb=64, batch_size_mb=4, event_batch_size=10000,\n"
     "        custom_metric_fields=None, compute_percentiles=False)\n"
     "--\n"
     "\n"
     "Run aggregation pipeline, return materialized ArrowTable.\n"
     "\n"
     "Args:\n"
     "    directory (str): Directory containing .pfw/.pfw.gz files.\n"
     "    time_interval_ms (float): Time bucket in milliseconds (default "
     "5000).\n"
     "    group_keys (list[str] or None): Extra grouping dims (default None).\n"
     "    categories (list[str] or None): Category filter (default None).\n"
     "    names (list[str] or None): Name filter (default None).\n"
     "    index_dir (str): Index sidecar directory (default '').\n"
     "    checkpoint_size (int): Checkpoint size (default 33554432).\n"
     "    force_rebuild (bool): Force index rebuild (default False).\n"
     "    chunk_size_mb (int): Target chunk size in MB (default 64).\n"
     "    batch_size_mb (int): Batch read size in MB (default 4).\n"
     "    event_batch_size (int): Entries per batch (default 10000).\n"
     "    custom_metric_fields (list[str] or None): Extra numeric args\n"
     "        fields to aggregate into *_total/*_min/*_max/*_mean/*_std\n"
     "        columns (default None).\n"
     "    compute_percentiles (bool): Enable percentile sketch collection\n"
     "        during aggregation (default False).\n"
     "\n"
     "Returns:\n"
     "    ArrowTable: Aggregated results.\n"},
    {"iter_arrow", (PyCFunction)Aggregator_iter_arrow,
     METH_VARARGS | METH_KEYWORDS,
     "iter_arrow(directory, time_interval_ms=5000.0, group_keys=None,\n"
     "           categories=None, names=None, index_dir='',\n"
     "           checkpoint_size=33554432, force_rebuild=False,\n"
     "           chunk_size_mb=64, batch_size_mb=4, event_batch_size=10000,\n"
     "           custom_metric_fields=None, compute_percentiles=False)\n"
     "--\n"
     "\n"
     "Run aggregation pipeline, stream Arrow batches.\n"
     "\n"
     "Args:\n"
     "    directory (str): Directory containing .pfw/.pfw.gz files.\n"
     "    time_interval_ms (float): Time bucket in milliseconds (default "
     "5000).\n"
     "    group_keys (list[str] or None): Extra grouping dims (default None).\n"
     "    categories (list[str] or None): Category filter (default None).\n"
     "    names (list[str] or None): Name filter (default None).\n"
     "    index_dir (str): Index sidecar directory (default '').\n"
     "    checkpoint_size (int): Checkpoint size (default 33554432).\n"
     "    force_rebuild (bool): Force index rebuild (default False).\n"
     "    chunk_size_mb (int): Target chunk size in MB (default 64).\n"
     "    batch_size_mb (int): Batch read size in MB (default 4).\n"
     "    event_batch_size (int): Entries per batch (default 10000).\n"
     "    custom_metric_fields (list[str] or None): Extra numeric args\n"
     "        fields to aggregate into *_total/*_min/*_max/*_mean/*_std\n"
     "        columns (default None).\n"
     "    compute_percentiles (bool): Enable percentile sketch collection\n"
     "        during aggregation (default False).\n"
     "\n"
     "Returns:\n"
     "    Iterator[ArrowBatch]: Arrow record batches.\n"},
    {NULL}};

PyTypeObject AggregatorType = {
    PyVarObject_HEAD_INIT(
        NULL, 0) "dftracer_utils_ext.AggregatorUtility", /* tp_name */
    sizeof(AggregatorObject),                            /* tp_basicsize */
    0,                                                   /* tp_itemsize */
    (destructor)Aggregator_dealloc,                      /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    Aggregator_call,                          /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "AggregatorUtility(runtime: Runtime | None = None)\n"
    "--\n\n"
    "High-level aggregation pipeline for DFTracer trace files.\n\n"
    "Args:\n"
    "    runtime (Runtime or None): Runtime for thread pool control.\n"
    "        If None, uses the default global Runtime.\n\n"
    "process(directory, time_interval_ms=5000.0, ...) -> ArrowTable\n"
    "    Run aggregation and return a materialized Arrow table.\n\n"
    "iter_arrow(directory, time_interval_ms=5000.0, ...) -> "
    "Iterator[ArrowBatch]\n"
    "    Run aggregation and stream Arrow batches.\n", /* tp_doc */
    0,                                                 /* tp_traverse */
    0,                                                 /* tp_clear */
    0,                                                 /* tp_richcompare */
    0,                                                 /* tp_weaklistoffset */
    0,                                                 /* tp_iter */
    0,                                                 /* tp_iternext */
    Aggregator_methods,                                /* tp_methods */
    0,                                                 /* tp_members */
    0,                                                 /* tp_getset */
    0,                                                 /* tp_base */
    0,                                                 /* tp_dict */
    0,                                                 /* tp_descr_get */
    0,                                                 /* tp_descr_set */
    0,                                                 /* tp_dictoffset */
    (initproc)Aggregator_init,                         /* tp_init */
    0,                                                 /* tp_alloc */
    Aggregator_new,                                    /* tp_new */
};

int init_aggregator(PyObject *m) {
    if (PyType_Ready(&AggregatorType) < 0) return -1;

    Py_INCREF(&AggregatorType);
    if (PyModule_AddObject(m, "AggregatorUtility",
                           (PyObject *)&AggregatorType) < 0) {
        Py_DECREF(&AggregatorType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
