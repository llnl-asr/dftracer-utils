#ifndef DFTRACER_UTILS_PYTHON_REORGANIZATION_PLANNER_H
#define DFTRACER_UTILS_PYTHON_REORGANIZATION_PLANNER_H

#include <Python.h>

typedef struct {
    PyObject_HEAD PyObject *runtime_obj;
} ReorganizationPlannerObject;

extern PyTypeObject ReorganizationPlannerType;

int init_reorganization_planner(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_REORGANIZATION_PLANNER_H
