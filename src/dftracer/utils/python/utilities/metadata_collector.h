#ifndef DFTRACER_UTILS_PYTHON_METADATA_COLLECTOR_H
#define DFTRACER_UTILS_PYTHON_METADATA_COLLECTOR_H

#include <Python.h>

typedef struct {
    PyObject_HEAD PyObject *runtime_obj;
} MetadataCollectorObject;

extern PyTypeObject MetadataCollectorType;

int init_metadata_collector(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_METADATA_COLLECTOR_H
