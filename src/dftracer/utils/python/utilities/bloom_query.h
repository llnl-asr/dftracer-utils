#ifndef DFTRACER_UTILS_PYTHON_BLOOM_QUERY_H
#define DFTRACER_UTILS_PYTHON_BLOOM_QUERY_H

#include <Python.h>

typedef struct {
    PyObject_HEAD PyObject *runtime_obj;
} BloomQueryObject;

extern PyTypeObject BloomQueryType;

int init_bloom_query(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_BLOOM_QUERY_H
