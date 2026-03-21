#ifndef DFTRACER_UTILS_PYTHON_VIEW_READER_H
#define DFTRACER_UTILS_PYTHON_VIEW_READER_H

#include <Python.h>

typedef struct {
    PyObject_HEAD PyObject *runtime_obj;
} ViewReaderObject;

extern PyTypeObject ViewReaderType;

int init_view_reader(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_VIEW_READER_H
