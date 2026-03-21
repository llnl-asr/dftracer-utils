#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_INDEXING_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_INDEXING_H

/**
 * @file indexing.h
 * @brief Convenience header for all DFTracer bloom/stats indexing components.
 *
 * This header provides a single include for the multigranular indexer
 * subsystem: bloom filters, chunk statistics, chunk indexer, and bloom queries.
 */

#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_INDEXING_H
