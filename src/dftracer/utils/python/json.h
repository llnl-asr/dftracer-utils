// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_JSON_H
#define DFTRACER_UTILS_PYTHON_JSON_H

#include <Python.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/python/trace_reader_iterator.h>

typedef struct {
    PyObject_HEAD dftracer::utils::dstd::SharedPtr<JsonDictBatch> batch;
    std::size_t event_index;
    bool is_args;
} JsonDictValueObject;

extern PyTypeObject JsonDictValueType;

namespace dftracer::utils::python {

int init_json_dict_value(PyObject *m);

PyObject *args_value_to_pyobject(const ArgsValue &v);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_JSON_H
