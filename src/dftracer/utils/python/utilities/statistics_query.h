#ifndef DFTRACER_UTILS_PYTHON_STATISTICS_QUERY_H
#define DFTRACER_UTILS_PYTHON_STATISTICS_QUERY_H

#include <Python.h>

typedef struct {
    PyObject_HEAD PyObject *runtime_obj;
} StatisticsQueryObject;

extern PyTypeObject StatisticsQueryUtilityType;

int init_statistics_query(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_STATISTICS_QUERY_H
