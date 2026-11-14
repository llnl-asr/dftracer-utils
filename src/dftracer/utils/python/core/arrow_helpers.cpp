// SPDX-License-Identifier: MIT

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/python/core/arrow_helpers.h>
#include <dftracer/utils/python/trace/trace_reader_iterator.h>

namespace dftracer::utils::python::core {

PyObject *wrap_arrow_result(Batch result) {
    if (!result.is_valid()) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot wrap invalid Batch");
        return NULL;
    }

    auto *cap = (ArrowBatchCapsuleObject *)ArrowBatchCapsuleType.tp_alloc(
        &ArrowBatchCapsuleType, 0);
    if (!cap) return NULL;

    cap->result = new Batch(std::move(result));
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

}  // namespace dftracer::utils::python::core
