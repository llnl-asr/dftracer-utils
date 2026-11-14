// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_CORE_ARROW_STREAM_CAPSULE_H
#define DFTRACER_UTILS_PYTHON_CORE_ARROW_STREAM_CAPSULE_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/python/trace/trace_reader_iterator.h>

typedef struct {
    PyObject_HEAD dftracer::utils::dstd::SharedPtr<ArrowIteratorState> state;
    bool consumed;
} ArrowBatchStreamObject;

extern PyTypeObject ArrowBatchStreamType;

namespace dftracer::utils::python::core {

int init_arrow_batch_stream(PyObject *m);

PyObject *make_arrow_batch_stream(
    dftracer::utils::dstd::SharedPtr<ArrowIteratorState> state);

}  // namespace dftracer::utils::python::core

#endif
