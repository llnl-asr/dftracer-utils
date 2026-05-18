#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/python/arrow_helpers.h>
#include <dftracer/utils/python/trace_reader_iterator.h>

namespace dftracer::utils::python {

PyObject *wrap_arrow_result(ArrowExportResult result) {
    if (!result.valid()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Cannot wrap invalid ArrowExportResult");
        return NULL;
    }

    auto *cap = (ArrowBatchCapsuleObject *)ArrowBatchCapsuleType.tp_alloc(
        &ArrowBatchCapsuleType, 0);
    if (!cap) return NULL;

    cap->result = new ArrowExportResult(std::move(result));
    return (PyObject *)cap;
}

PyObject *wrap_arrow_table(PyObject *batch_list) {
    if (!batch_list) {
        PyErr_SetString(PyExc_RuntimeError, "batch_list is NULL");
        return NULL;
    }

    PyObject *mod = PyImport_ImportModule("dftracer.utils.arrow");
    if (!mod) {
        Py_DECREF(batch_list);
        return NULL;
    }

    PyObject *cls = PyObject_GetAttrString(mod, "ArrowTable");
    Py_DECREF(mod);
    if (!cls) {
        Py_DECREF(batch_list);
        return NULL;
    }

    PyObject *table = PyObject_CallFunctionObjArgs(cls, batch_list, NULL);
    Py_DECREF(cls);
    Py_DECREF(batch_list);
    return table;
}

PyObject *wrap_arrow_stream_table(PyObject *stream_obj) {
    if (!stream_obj) {
        PyErr_SetString(PyExc_RuntimeError, "stream_obj is NULL");
        return NULL;
    }

    PyObject *mod = PyImport_ImportModule("dftracer.utils.arrow");
    if (!mod) {
        Py_DECREF(stream_obj);
        return NULL;
    }

    PyObject *cls = PyObject_GetAttrString(mod, "ArrowTable");
    Py_DECREF(mod);
    if (!cls) {
        Py_DECREF(stream_obj);
        return NULL;
    }

    PyObject *table = PyObject_CallFunctionObjArgs(cls, stream_obj, NULL);
    Py_DECREF(cls);
    Py_DECREF(stream_obj);
    return table;
}

PyObject *arrow_result_to_table(ArrowExportResult result) {
    PyObject *capsule = wrap_arrow_result(std::move(result));
    if (!capsule) return NULL;

    PyObject *list = PyList_New(1);
    if (!list) {
        Py_DECREF(capsule);
        return NULL;
    }
    PyList_SET_ITEM(list, 0, capsule);  // steals ref

    return wrap_arrow_table(list);
}

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_ENABLE_ARROW
