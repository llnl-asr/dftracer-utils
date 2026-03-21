#ifndef DFTRACER_UTILS_PYTHON_RECONSTRUCTION_PLANNER_H
#define DFTRACER_UTILS_PYTHON_RECONSTRUCTION_PLANNER_H

#include <Python.h>

typedef struct {
    PyObject_HEAD PyObject *runtime_obj;
} ReconstructionPlannerObject;

extern PyTypeObject ReconstructionPlannerType;

int init_reconstruction_planner(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_RECONSTRUCTION_PLANNER_H
