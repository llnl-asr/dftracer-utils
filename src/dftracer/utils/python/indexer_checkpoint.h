#ifndef DFTRACER_UTILS_PYTHON_INDEXER_CHECKPOINT_H
#define DFTRACER_UTILS_PYTHON_INDEXER_CHECKPOINT_H

#include <Python.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>

typedef struct {
    PyObject_HEAD dft_indexer_checkpoint_t checkpoint;
} IndexerCheckpointObject;

PyObject *IndexerCheckpoint_new(PyTypeObject *type, PyObject *args,
                                PyObject *kwds);

extern PyTypeObject IndexerCheckpointType;

int init_indexer_checkpoint(PyObject *m);

#endif  // DFTRACER_UTILS_PYTHON_INDEXER_CHECKPOINT_H
