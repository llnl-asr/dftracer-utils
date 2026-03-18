#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_UTILS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_UTILS_H

#include <string>

namespace dftracer::utils::utilities::composites::dft::internal {

/**
 * @brief Determine the index file path for a given data file.
 *
 * When a custom index directory is provided, the index is placed there
 * directly. Otherwise, a unique subdirectory under /tmp is created
 * using a hash of the data file's absolute path, preventing collisions
 * when multiple files share the same basename.
 *
 * @param file_path Path to the data file (e.g., "data/trace.pfw.gz")
 * @param index_dir Optional custom directory for the index file.
 *                  If empty, uses /tmp/dft_<hash>/.
 * @return Complete path to the index file
 *         (e.g., "/tmp/dft_a1b2c3d4/trace.pfw.gz.idx")
 */
std::string determine_index_path(const std::string& file_path,
                                 const std::string& index_dir = "");

/**
 * @brief Determine the provenance index file path for a given data file.
 *
 * Follows the same placement logic as determine_index_path but produces
 * a `.pidx` sidecar instead of `.idx`.
 *
 * @param data_path Path to the data file
 * @param index_dir Optional directory. If empty, places next to data file.
 * @return Complete path to the provenance index file
 */
std::string determine_provenance_index_path(const std::string& data_path,
                                            const std::string& index_dir = "");

}  // namespace dftracer::utils::utilities::composites::dft::internal

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_UTILS_H
