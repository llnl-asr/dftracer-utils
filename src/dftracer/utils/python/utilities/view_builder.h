#ifndef DFTRACER_UTILS_PYTHON_VIEW_BUILDER_H
#define DFTRACER_UTILS_PYTHON_VIEW_BUILDER_H

#include <Python.h>

typedef struct {
    PyObject_HEAD PyObject *runtime_obj;
} ViewBuilderObject;

extern PyTypeObject ViewBuilderType;

int init_view_builder(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_VIEW_BUILDER_H
