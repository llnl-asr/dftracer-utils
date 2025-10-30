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
#include <dftracer/utils/utilities/composites/types.h>

// Generic composites
#include <dftracer/utils/utilities/composites/batch_processor_utility.h>
#include <dftracer/utils/utilities/composites/chunk_verifier_utility.h>
#include <dftracer/utils/utilities/composites/directory_file_processor_utility.h>
#include <dftracer/utils/utilities/composites/file_compressor_utility.h>
#include <dftracer/utils/utilities/composites/file_decompressor_utility.h>
#include <dftracer/utils/utilities/composites/file_merger_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/line_batch_processor_utility.h>

// DFTracer-specific composites
#include <dftracer/utils/utilities/composites/dft/dft.h>

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_COMPOSITES_H
