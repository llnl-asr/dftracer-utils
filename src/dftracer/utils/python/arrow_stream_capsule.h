#ifndef DFTRACER_UTILS_PYTHON_ARROW_STREAM_CAPSULE_H
#define DFTRACER_UTILS_PYTHON_ARROW_STREAM_CAPSULE_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/python/trace_reader_iterator.h>

#include <memory>

typedef struct {
    PyObject_HEAD std::shared_ptr<ArrowIteratorState> state;
    bool consumed;
} ArrowBatchStreamObject;

extern PyTypeObject ArrowBatchStreamType;

int init_arrow_batch_stream(PyObject *m);

PyObject *make_arrow_batch_stream(std::shared_ptr<ArrowIteratorState> state);

#endif
#endif
