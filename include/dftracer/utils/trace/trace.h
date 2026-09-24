#ifndef DFTRACER_UTILS_TRACE_TRACE_H
#define DFTRACER_UTILS_TRACE_TRACE_H

/**
 * @file trace.h
 * @brief Umbrella header for the DFTracer trace library.
 *
 * A single include for the trace domain: aggregators, indexing, views,
 * statistics, and the trace-specific utilities and types.
 */

#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregators.h>
#include <dftracer/utils/trace/chunk_extractor_utility.h>
#include <dftracer/utils/trace/chunk_manifest_mapper_utility.h>
#include <dftracer/utils/trace/chunk_verifier_utility.h>
#include <dftracer/utils/trace/event_collector_utility.h>
#include <dftracer/utils/trace/internal/chunk_manifest.h>
#include <dftracer/utils/trace/internal/chunk_spec.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/trace/statistics/statistics.h>

#endif  // DFTRACER_UTILS_TRACE_TRACE_H
