#ifndef DFTRACER_UTILS_PYTHON_UTILITIES_COMPARATOR_H
#define DFTRACER_UTILS_PYTHON_UTILITIES_COMPARATOR_H

#include <Python.h>

typedef struct {
    PyObject_HEAD PyObject *runtime_obj;
} ComparatorObject;

extern PyTypeObject ComparatorType;

int init_comparator(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_UTILITIES_COMPARATOR_H
