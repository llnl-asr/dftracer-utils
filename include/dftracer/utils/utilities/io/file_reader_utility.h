#ifndef DFTRACER_UTILS_UTILITIES_IO_FILE_READER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_IO_FILE_READER_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
#include <dftracer/utils/utilities/text/shared.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dftracer::utils::utilities::io {

/**
 * @brief Utility that reads a file and returns its text content.
 *
 * This utility takes a FileEntry (from DirectoryScanner or manually created)
 * and reads the file content as Text. It composes with existing types.
 *
 * Composition examples:
 * - DirectoryScanner → FileReader → Text
 * - FileEntry → FileReader → Text → LineSplitter → Lines
 * - FileEntry → FileReader → Text → TextHasher → Hash
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
class FileReaderUtility
    : public utilities::Utility<filesystem::FileEntry, text::Text,
                                utilities::tags::Parallelizable> {
   public:
    FileReaderUtility() = default;
    ~FileReaderUtility() = default;

    /**
     * @brief Read file content as text.
     *
     * @param input FileEntry representing the file to read
     * @return Text containing file content
     * @throws std::runtime_error if file cannot be read
     */
    text::Text process(const filesystem::FileEntry& input) override {
        if (!fs::exists(input.path)) {
            throw std::runtime_error("File does not exist: " +
                                     input.path.string());
        }

        if (!input.is_regular_file) {
            throw std::runtime_error("Path is not a regular file: " +
                                     input.path.string());
        }

        std::ifstream file(input.path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Cannot open file: " +
                                     input.path.string());
        }

        std::ostringstream content;
        content << file.rdbuf();

        return text::Text{content.str()};
    }
};

}  // namespace dftracer::utils::utilities::io

#endif  // DFTRACER_UTILS_UTILITIES_IO_FILE_READER_UTILITY_H
