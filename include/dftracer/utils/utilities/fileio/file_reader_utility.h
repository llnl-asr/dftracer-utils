// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_FILE_READER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_FILE_READER_UTILITY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
#include <dftracer/utils/utilities/text/shared.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Utility that reads a file and returns its text content.
 *
 * This utility takes a @ref dftracer::utils::utilities::filesystem::FileEntry
 * "FileEntry" (from DirectoryScanner or manually created) and reads the file
 * content as Text. It composes with existing types.
 *
 * Composition examples:
 * - DirectoryScanner -> FileReader -> Text
 * - @ref dftracer::utils::utilities::filesystem::FileEntry "FileEntry" ->
 * FileReader -> Text -> LineSplitter -> Lines
 * - @ref dftracer::utils::utilities::filesystem::FileEntry "FileEntry" ->
 * FileReader -> Text -> TextHasher -> Hash
 *
 * Features:
 * - Reads entire file into memory as text
 * - Can be tagged with Cacheable, Retryable, Monitored behaviors
 * - Composes with text utilities for processing
 *
 * Usage:
 * @code
 * auto reader = std::make_shared<FileReader>();
 *
 * FileEntry file{"/path/to/file.txt"};
 * text::Text content = reader->process(file);
 *
 * std::cout << "Read " << content.size() << " bytes\n";
 * @endcode
 *
 * Composition with DirectoryScanner:
 * @code
 * auto scanner = std::make_shared<DirectoryScanner>();
 * auto reader = std::make_shared<FileReader>();
 *
 * auto files = scanner->process(Directory{"."});
 * for (const auto& file : files) {
 *     if (file.is_regular_file) {
 *         text::Text content = reader->process(file);
 *         // Process content...
 *     }
 * }
 * @endcode
 */
class FileReaderUtility {
   public:
    /**
     * @brief Read file content as text.
     *
     * @param input @ref dftracer::utils::utilities::filesystem::FileEntry
     * "FileEntry" representing the file to read
     * @return Text containing file content
     * @throws std::runtime_error if file cannot be read
     */
    coro::CoroTask<text::Text> operator()(
        const filesystem::FileEntry& input) const {
        const auto found = dstd::fs::exists(input.path.view());
        if (!found) throw common::DFTUtilsException(found.error());
        if (!*found) {
            throw common::DFTUtilsException(
                common::ErrorCode::NotFound,
                common::str_cat("File does not exist: ", input.path.str()));
        }

        if (!input.is_regular_file) {
            throw common::DFTUtilsException(
                common::ErrorCode::Io,
                common::str_cat("Path is not a regular file: ",
                                input.path.str()));
        }

        std::ifstream file(input.path.c_str(), std::ios::binary);
        if (!file) {
            throw common::DFTUtilsException(
                common::ErrorCode::Io,
                common::str_cat("Cannot open file: ", input.path.str()));
        }

        std::ostringstream content;
        content << file.rdbuf();

        co_return text::Text{content.str()};
    }
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_FILE_READER_UTILITY_H
