#ifndef DFTRACER_UTILS_PYTHON_JSON_H
#define DFTRACER_UTILS_PYTHON_JSON_H

#include <Python.h>
#include <dftracer/utils/python/trace_reader_iterator.h>

#include <memory>

typedef struct {
    PyObject_HEAD std::shared_ptr<JsonDictBatch> batch;
    std::size_t event_index;
    bool is_args;
} JsonDictValueObject;

extern PyTypeObject JsonDictValueType;
int init_json_dict_value(PyObject *m);

PyObject *args_value_to_pyobject(const ArgsValue &v);

#endif  // DFTRACER_UTILS_PYTHON_JSON_H
