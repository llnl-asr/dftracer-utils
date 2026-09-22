#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/python/batch_indexer.h>
#include <dftracer/utils/python/columnar_eval.h>
#include <dftracer/utils/python/dataframe.h>
#include <dftracer/utils/python/index_database.h>
#include <dftracer/utils/python/indexer.h>
#include <dftracer/utils/python/json.h>
#include <dftracer/utils/python/lazyframe.h>
#include <dftracer/utils/python/memoryview_batch.h>
#include <dftracer/utils/python/op_runner.h>
#include <dftracer/utils/python/ops.h>
#include <dftracer/utils/python/plugin_host.h>
#include <dftracer/utils/python/py_errors.h>
#include <dftracer/utils/python/py_method.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/series.h>
#include <dftracer/utils/python/sst_distribution.h>
#include <dftracer/utils/python/task_handle.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#include <dftracer/utils/python/trace_viewer.h>
#include <dftracer/utils/trace/aggregators/aggregation_serialization.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/arrow_stream_capsule.h>
#include <dftracer/utils/python/streaming_iterator.h>
#endif
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/python/arrow_parallel_reader.h>
#endif

namespace {

PyObject* py_set_log_level(PyObject*, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    auto lvl = dftracer::utils::logger::level_from_name(name);
    if (!lvl) {
        PyErr_Format(PyExc_ValueError, "invalid log level: '%s'", name);
        return nullptr;
    }
    dftracer::utils::logger::set_level(*lvl);
    Py_RETURN_NONE;
}

PyObject* py_get_log_level(PyObject*, PyObject*) {
    return PyUnicode_FromString(dftracer::utils::logger::level_name(
        dftracer::utils::logger::get_level()));
}

PyObject* py_set_log_color(PyObject*, PyObject* args) {
    const char* mode = nullptr;
    if (!PyArg_ParseTuple(args, "s", &mode)) return nullptr;
    using dftracer::utils::logger::ColorMode;
    const std::string_view m(mode);
    ColorMode cm;
    if (m == "auto") {
        cm = ColorMode::Auto;
    } else if (m == "always") {
        cm = ColorMode::Always;
    } else if (m == "never") {
        cm = ColorMode::Never;
    } else {
        PyErr_Format(PyExc_ValueError,
                     "invalid color mode: '%s' (auto|always|never)", mode);
        return nullptr;
    }
    dftracer::utils::logger::set_color(cm);
    Py_RETURN_NONE;
}

PyObject* py_memory_budget_advice(PyObject*, PyObject* args, PyObject* kwds) {
    unsigned long long required = 0, available = 0;
    static const char* kwlist[] = {"required_bytes", "available_bytes",
                                   nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "K|K",
                                     const_cast<char**>(kwlist), &required,
                                     &available))
        return nullptr;
    const auto a = dftracer::utils::memory_budget_advice(
        static_cast<std::size_t>(required),
        static_cast<std::size_t>(available));
    const std::string warning =
        dftracer::utils::format_memory_budget_warning(a);
    return Py_BuildValue(
        "{s:O,s:K,s:K,s:K,s:K,s:s}", "fits", a.fits ? Py_True : Py_False,
        "required_bytes", (unsigned long long)a.required_bytes, "peak_bytes",
        (unsigned long long)a.peak_bytes, "available_bytes",
        (unsigned long long)a.available_bytes, "suggested_nodes",
        (unsigned long long)a.suggested_nodes, "warning", warning.c_str());
}

PyMethodDef dftracer_utils_methods[] = {
    {"memory_budget_advice", DFTU_PYCFUNCTION(py_memory_budget_advice),
     METH_VARARGS | METH_KEYWORDS,
     "memory_budget_advice(required_bytes, available_bytes=0) -> dict\n\n"
     "Whether an aggregated workload of `required_bytes` fits in process, and\n"
     "advice if not. `available_bytes=0` detects it (cgroup-aware). Returns\n"
     "{fits, required_bytes, peak_bytes, available_bytes, suggested_nodes,\n"
     "warning}: peak is PEAK_MEMORY_FACTOR x required; `warning` is a ready\n"
     "message (empty when it fits)."},
    {"set_log_level", py_set_log_level, METH_VARARGS,
     "set_log_level(level: str) -> None\n\n"
     "Set the C++ logger level (trace|debug|info|warn|error|off)."},
    {"get_log_level", py_get_log_level, METH_NOARGS,
     "get_log_level() -> str\n\nReturn the current C++ logger level."},
    {"set_log_color", py_set_log_color, METH_VARARGS,
     "set_log_color(mode: str) -> None\n\n"
     "Set the logger color mode (auto|always|never)."},
    {NULL, NULL, 0, NULL},
};

}  // namespace

static PyModuleDef dftracer_utils_module = {
    PyModuleDef_HEAD_INIT,
    "dftracer_utils_ext",                        /* m_name */
    "DFTracer utils module with indexer, reader, "
    "and utility bindings",                      /* m_doc */
    -1,                                          /* m_size */
    dftracer_utils_methods,                      /* m_methods */
    NULL,                                        /* m_slots */
    NULL,                                        /* m_traverse */
    NULL,                                        /* m_clear */
    NULL                                         /* m_free */
};

PyMODINIT_FUNC PyInit_dftracer_utils_ext(void);  // declared before use

PyMODINIT_FUNC PyInit_dftracer_utils_ext(void) {
    PyObject* m;
    m = PyModule_Create(&dftracer_utils_module);
    if (m == NULL) return NULL;
    // The aggregation CF shard count, so distributed callers split the shard
    // space without hardcoding it (single source of truth).
    PyModule_AddIntConstant(
        m, "NUM_SHARDS",
        dftracer::utils::trace::aggregators::AGG_KEY_NUM_SHARDS);
    // Configure the C++ logger for the extension: picks up
    // DFTRACER_UTILS_LOG_LEVEL and auto color (on only when stderr is a TTY),
    // matching the CLI binaries. Without this the logger runs on bare defaults.
    dftracer::utils::logger::init();
    if (dftracer::utils::python::init_py_errors(m) < 0) return NULL;
    if (dftracer::utils::python::init_checkpoint_indexer(m) < 0) return NULL;
    if (dftracer::utils::python::init_indexer(m) < 0) return NULL;
    if (dftracer::utils::python::init_task_handle(m) < 0) return NULL;
    if (dftracer::utils::python::init_runtime(m) < 0) return NULL;
    if (dftracer::utils::python::init_memoryview_batch(m) < 0) return NULL;
    if (dftracer::utils::python::init_json_dict_value(m) < 0) return NULL;
    if (dftracer::utils::python::init_trace_reader_iterator(m) < 0) return NULL;
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    if (dftracer::utils::python::init_arrow_streaming_iterator(m) < 0)
        return NULL;
    if (dftracer::utils::python::init_arrow_batch_stream(m) < 0) return NULL;
#endif
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    if (dftracer::utils::python::init_arrow_parallel_reader(m) < 0) return NULL;
#endif
    if (dftracer::utils::python::init_trace_viewer(m) < 0) return NULL;
    if (dftracer::utils::python::init_plugin_host(m) < 0) return NULL;
    if (dftracer::utils::python::init_op_runner(m) < 0) return NULL;
    if (dftracer::utils::python::init_ops(m) < 0) return NULL;
    if (dftracer::utils::python::init_index_database(m) < 0) return NULL;
    if (dftracer::utils::python::init_sst_distribution(m) < 0) return NULL;
    if (dftracer::utils::python::init_series(m) < 0) return NULL;
    if (dftracer::utils::python::init_dataframe(m) < 0) return NULL;
    if (dftracer::utils::python::init_lazyframe(m) < 0) return NULL;
    if (dftracer::utils::python::init_columnar_eval(m) < 0) return NULL;
    return m;
}
