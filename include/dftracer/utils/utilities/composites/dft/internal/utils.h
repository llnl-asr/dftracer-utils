#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_UTILS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_UTILS_H

#include <string>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft::internal {

// True when the event's return value represents bytes transferred.
// Checks both category (POSIX/STDIO) and function name.
bool is_data_transfer_op(std::string_view cat, std::string_view name);

/**
 * @brief Determine the root-local RocksDB index path for a given data file.
 *
 * When a custom index directory is provided, the index root is
 * `<index_dir>/.dftindex`. Otherwise, the index root is placed alongside the
 * data file as `<file_dir>/.dftindex`.
 *
 * @param file_path Path to the data file (e.g., "data/trace.pfw.gz")
 * @param index_dir Optional custom directory for the index root.
 * @return Path to the owning `.dftindex` directory.
 */
std::string determine_index_path(const std::string& file_path,
                                 const std::string& index_dir = "");

/**
 * @brief Determine the provenance index file path for a given data file.
 *
 * Provenance now lives in the same root-local `.dftindex` database as
 * the regular index data.
 *
 * @param data_path Path to the data file
 * @param index_dir Optional directory. If empty, places next to data file.
 * @return Path to the owning `.dftindex` directory
 */
std::string determine_provenance_index_path(const std::string& data_path,
                                            const std::string& index_dir = "");

}  // namespace dftracer::utils::utilities::composites::dft::internal

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INTERNAL_UTILS_H
