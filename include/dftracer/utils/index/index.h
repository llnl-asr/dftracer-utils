#ifndef DFTRACER_UTILS_INDEX_INDEX_H
#define DFTRACER_UTILS_INDEX_INDEX_H

/**
 * @file index.h
 * @brief Convenience header for all DFTracer bloom/stats indexing components.
 *
 * This header provides a single include for the multigranular indexer
 * subsystem: bloom filters, chunk statistics, chunk indexer, and bloom queries.
 */

#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/extensions/bloom_filter.h>
#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>

#endif  // DFTRACER_UTILS_INDEX_INDEX_H
