#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_UTILS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_UTILS_H

#include <string>

namespace dftracer::utils::utilities::composites::dft {

/**
 * @brief Determine the index file path for a given data file.
 *
 * This utility function constructs the index file path based on the data file
 * path and an optional custom index directory. If no custom index directory
 * is provided, the system's temporary directory is used.
 *
 * The index file is created by:
 * 1. Taking the base filename from the data file
 * 2. Appending the standard index extension (.idx)
 * 3. Placing it in the specified index directory (or temp directory if empty)
 *
 * @param file_path Path to the data file (e.g., "trace.pfw.gz")
 * @param index_dir Optional custom directory for the index file.
 *                  If empty, uses the system temporary directory.
 * @return Complete path to the index file (e.g., "/tmp/trace.pfw.gz.idx")
 *
 * @example
 * @code
 * // Using temporary directory
 * std::string idx_path = determine_index_path("data/trace.pfw.gz", "");
 * // Returns: "/tmp/trace.pfw.gz.idx"
 *
 * // Using custom directory
 * std::string idx_path = determine_index_path("data/trace.pfw.gz",
 * "/custom/idx");
 * // Returns: "/custom/idx/trace.pfw.gz.idx"
 * @endcode
 */
std::string determine_index_path(const std::string& file_path,
                                 const std::string& index_dir = "");

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_UTILS_H
