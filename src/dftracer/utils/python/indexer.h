// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_INDEXER_H
#define DFTRACER_UTILS_PYTHON_INDEXER_H

#include <Python.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>

#include <cstdint>

typedef struct {
    PyObject_HEAD dftracer::utils::dstd::SharedPtr<
        dftracer::utils::index::gzip::CheckpointIndexer> *handle;
    PyObject *gz_path;
    PyObject *index_path;
    std::uint64_t checkpoint_size;
    int build_bloom;
    PyObject *runtime_obj;  // RuntimeObject* or NULL (uses default)
} CheckpointIndexerObject;

extern PyTypeObject CheckpointIndexerType;

namespace dftracer::utils::python {

int init_checkpoint_indexer(PyObject *m);

}  // namespace dftracer::utils::python

#endif
