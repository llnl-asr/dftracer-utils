#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_COMPOSITES_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_COMPOSITES_H

/**
 * @file composites.h
 * @brief Convenience header for all composites.
 *
 * This header provides a single include for all general-purpose composites,
 * DFTracer-specific composites, and related types.
 */

// Core workflow types
#include <dftracer/utils/utilities/composites/composite_types.h>

// Generic composites
#include <dftracer/utils/utilities/composites/batch_processor.h>
#include <dftracer/utils/utilities/composites/chunk_verifier.h>
#include <dftracer/utils/utilities/composites/directory_file_processor.h>
#include <dftracer/utils/utilities/composites/file_compressor.h>
#include <dftracer/utils/utilities/composites/file_decompressor.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader.h>
#include <dftracer/utils/utilities/composites/line_batch_processor.h>

// DFTracer-specific composites
#include <dftracer/utils/utilities/composites/dft/dft.h>

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_COMPOSITES_H
