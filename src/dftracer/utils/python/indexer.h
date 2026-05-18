#ifndef DFTRACER_UTILS_PYTHON_INDEXER_H
#define DFTRACER_UTILS_PYTHON_INDEXER_H

#include <Python.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <cstdint>

typedef struct {
    PyObject_HEAD dft_indexer_handle_t handle;
    PyObject *gz_path;
    PyObject *index_path;
    std::uint64_t checkpoint_size;
    int build_bloom;
    int build_manifest;
    PyObject *runtime_obj;  // RuntimeObject* or NULL (uses default)
} CheckpointIndexerObject;

extern PyTypeObject CheckpointIndexerType;

int init_checkpoint_indexer(PyObject *m);

#endif
