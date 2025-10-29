#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFT_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFT_H

/**
 * @file dft.h
 * @brief Convenience header for all DFTracer composites.
 *
 * This header provides a single include for all DFTracer-specific composites
 * and related types.
 */

// Core composites
#include <dftracer/utils/utilities/composites/dft/chunk_extractor.h>
#include <dftracer/utils/utilities/composites/dft/chunk_manifest_mapper.h>
#include <dftracer/utils/utilities/composites/dft/event_collector.h>
#include <dftracer/utils/utilities/composites/dft/event_hasher.h>
#include <dftracer/utils/utilities/composites/dft/index_builder.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector.h>

// DFTracer-specific types
#include <dftracer/utils/utilities/composites/dft/chunk_manifest.h>
#include <dftracer/utils/utilities/composites/dft/chunk_spec.h>

// Utilities
#include <dftracer/utils/utilities/composites/dft/utils.h>

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFT_H
