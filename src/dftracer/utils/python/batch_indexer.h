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
    int build_bloom;
    int require_aggregation;

    // Bloom tier: args fields indexed for chunk pruning (list, or None for
    // the default set), false-positive rate and expected entries per chunk.
    PyObject* bloom_fields;
    double false_positive_rate;
    std::size_t expected_entries;
    std::size_t path_budget;
    double stats_share;
    std::size_t auto_max_distinct;
    // Pruning extensions to build (list of str), or None for the default.
    PyObject* extensions;
    std::uint64_t memory_budget;
    PyObject* schema;  // str or nullptr (detect)

    // Aggregation config (stored for rebuild)
    double time_interval_ms;
    PyObject* group_keys;            // Python list or None
    PyObject* custom_metric_fields;  // Python list or None
    int compute_percentiles;
    int group_by_file;

    std::size_t checkpoint_size;
    std::size_t parallelism;
    int force_rebuild;
};

extern PyTypeObject IndexerType;

namespace dftracer::utils::python {

int init_indexer(PyObject* m);

}  // namespace dftracer::utils::python

#endif  // DFTRACER_UTILS_PYTHON_BATCH_INDEXER_H
