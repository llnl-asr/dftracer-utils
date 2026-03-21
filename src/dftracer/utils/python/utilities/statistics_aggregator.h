#ifndef DFTRACER_UTILS_PYTHON_STATISTICS_AGGREGATOR_H
#define DFTRACER_UTILS_PYTHON_STATISTICS_AGGREGATOR_H

#include <Python.h>

typedef struct {
    PyObject_HEAD PyObject *runtime_obj;
} StatisticsAggregatorObject;

extern PyTypeObject StatisticsAggregatorType;

int init_statistics_aggregator(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_STATISTICS_AGGREGATOR_H
