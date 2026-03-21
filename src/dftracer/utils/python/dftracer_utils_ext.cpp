#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/python/indexer.h>
#include <dftracer/utils/python/indexer_checkpoint.h>
#include <dftracer/utils/python/json.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/task_handle.h>
#include <dftracer/utils/python/trace_reader.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#include <dftracer/utils/python/utilities/aggregator.h>
#include <dftracer/utils/python/utilities/bloom_query.h>
#include <dftracer/utils/python/utilities/metadata_collector.h>
#include <dftracer/utils/python/utilities/reconstruction_planner.h>
#include <dftracer/utils/python/utilities/reorganization_planner.h>
#include <dftracer/utils/python/utilities/statistics_aggregator.h>
#include <dftracer/utils/python/utilities/statistics_query.h>
#include <dftracer/utils/python/utilities/view_builder.h>
#include <dftracer/utils/python/utilities/view_reader.h>

static PyModuleDef dftracer_utils_module = {
    PyModuleDef_HEAD_INIT,
    "dftracer_utils_ext",   /* m_name */
    "DFTracer utils module with indexer, reader, lazy JSON, "
    "and utility bindings", /* m_doc */
    -1,                     /* m_size */
    NULL,                   /* m_methods */
    NULL,                   /* m_slots */
    NULL,                   /* m_traverse */
    NULL,                   /* m_clear */
    NULL                    /* m_free */
};

PyMODINIT_FUNC PyInit_dftracer_utils_ext(void) {
    PyObject *m;
    m = PyModule_Create(&dftracer_utils_module);
    if (m == NULL) return NULL;
    if (init_indexer_checkpoint(m) < 0) return NULL;
    if (init_json(m) < 0) return NULL;
    if (init_indexer(m) < 0) return NULL;
    if (init_task_handle(m) < 0) return NULL;
    if (init_runtime(m) < 0) return NULL;
    if (init_trace_reader_iterator(m) < 0) return NULL;
    if (init_trace_reader(m) < 0) return NULL;
    if (init_statistics_query(m) < 0) return NULL;
    if (init_bloom_query(m) < 0) return NULL;
    if (init_statistics_aggregator(m) < 0) return NULL;
    if (init_metadata_collector(m) < 0) return NULL;
    if (init_view_builder(m) < 0) return NULL;
    if (init_view_reader(m) < 0) return NULL;
    if (init_reorganization_planner(m) < 0) return NULL;
    if (init_reconstruction_planner(m) < 0) return NULL;
    if (init_aggregator(m) < 0) return NULL;
    return m;
}
