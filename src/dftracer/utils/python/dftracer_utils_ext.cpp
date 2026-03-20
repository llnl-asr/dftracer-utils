#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/python/indexer.h>
#include <dftracer/utils/python/indexer_checkpoint.h>
#include <dftracer/utils/python/json.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/task_handle.h>
#include <dftracer/utils/python/trace_reader.h>
#include <dftracer/utils/python/trace_reader_iterator.h>

static PyModuleDef dftracer_utils_module = {
    PyModuleDef_HEAD_INIT,
    "dftracer_utils_ext", /* m_name */
    "DFTracer utils module with indexer, reader, and lazy JSON "
    "functionality",      /* m_doc */
    -1,                   /* m_size */
    NULL,                 /* m_methods */
    NULL,                 /* m_slots */
    NULL,                 /* m_traverse */
    NULL,                 /* m_clear */
    NULL                  /* m_free */
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
    return m;
}
