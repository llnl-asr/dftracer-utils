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
#include <dftracer/utils/utilities/composites/dft/chunk_extractor_utility.h>
#include <dftracer/utils/utilities/composites/dft/chunk_manifest_mapper_utility.h>
#include <dftracer/utils/utilities/composites/dft/event_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/event_hasher_utility.h>
#include <dftracer/utils/utilities/composites/dft/index_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>

// DFTracer-specific types
#include <dftracer/utils/utilities/composites/dft/internal/chunk_manifest.h>
#include <dftracer/utils/utilities/composites/dft/internal/chunk_spec.h>

// Utilities
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFT_H
