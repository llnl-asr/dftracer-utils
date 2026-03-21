#ifndef DFTRACER_UTILS_PYTHON_UTILITIES_AGGREGATOR_H
#define DFTRACER_UTILS_PYTHON_UTILITIES_AGGREGATOR_H

#include <Python.h>

typedef struct {
    PyObject_HEAD PyObject *runtime_obj;
} AggregatorObject;

extern PyTypeObject AggregatorType;

int init_aggregator(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_UTILITIES_AGGREGATOR_H
