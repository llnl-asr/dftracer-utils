#ifndef DFTRACER_UTILS_PYTHON_BATCH_INDEXER_H
#define DFTRACER_UTILS_PYTHON_BATCH_INDEXER_H

#include <Python.h>

#include <cstddef>
#include <cstdint>

struct IndexerObject {
    PyObject_HEAD

        PyObject* runtime_obj;
    PyObject* directory;
    PyObject* files;  // Python list of file paths or None
    PyObject* index_dir;

    // Tier requirements
    int require_checkpoint;
    int require_bloom;
    int require_manifest;
    int require_aggregation;

    // Aggregation config (stored for rebuild)
    double time_interval_ms;
    PyObject* group_keys;            // Python list or None
    PyObject* custom_metric_fields;  // Python list or None
    int compute_percentiles;

    std::size_t checkpoint_size;
    std::size_t parallelism;
    int force_rebuild;
};

extern PyTypeObject IndexerType;

int init_indexer(PyObject* m);

#endif  // DFTRACER_UTILS_PYTHON_BATCH_INDEXER_H
