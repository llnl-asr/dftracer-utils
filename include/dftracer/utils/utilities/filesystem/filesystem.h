#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_H

/**
 * @file filesystem.h
 * @brief Convenience header for filesystem component utilities.
 *
 * This header provides composable utilities for filesystem operations:
 * - DirectoryScanner: Scan directories and enumerate files
 * - PatternDirectoryScanner: Scan directories with pattern/extension filtering
 *
 * Usage:
 * @code
 * #include <dftracer/utils/utilities/filesystem/filesystem.h>
 *
 * using namespace dftracer::utils::utilities::filesystem;
 *
 * auto scanner = std::make_shared<DirectoryScanner>();
 * auto files = scanner->process(Directory{"/path/to/dir"});
 *
 * auto pattern_scanner = std::make_shared<PatternDirectoryScanner>();
 * auto pfw_files = pattern_scanner->process(
 *     PatternDirectory{"/path/to/dir", {".pfw", ".pfw.gz"}}
 * );
 * @endcode
 */

#include <dftracer/utils/utilities/filesystem/directory_scanner.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner.h>

#endif  // DFTRACER_UTILS_UTILITIES_FILESYSTEM_H
