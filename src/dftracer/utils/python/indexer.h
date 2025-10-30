#ifndef DFTRACER_UTILS_PYTHON_INDEXER_H
#define DFTRACER_UTILS_PYTHON_INDEXER_H

#include <Python.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
typedef struct {
    PyObject_HEAD dft_indexer_handle_t handle;
    PyObject *gz_path;
    PyObject *idx_path;
    std::uint64_t checkpoint_size;
} IndexerObject;

extern PyTypeObject IndexerType;

int init_indexer(PyObject *m);

#endif
