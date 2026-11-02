// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_TRACE_H
#define DFTRACER_UTILS_TRACE_TRACE_H

/**
 * @file trace.h
 * @brief Umbrella header for the DFTracer trace library.
 *
 * A single include for the trace domain: aggregation partials, indexing, views,
 * statistics, and the trace-specific utilities and types.
 */

#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/extensions/bloom_filter.h>
#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/trace/chunk_extractor_utility.h>
#include <dftracer/utils/trace/chunk_verifier_utility.h>
#include <dftracer/utils/trace/event_collector_utility.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/trace/statistics/statistics.h>

#endif  // DFTRACER_UTILS_TRACE_TRACE_H
