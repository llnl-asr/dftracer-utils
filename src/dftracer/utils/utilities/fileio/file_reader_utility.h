// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_FILE_READER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_FILE_READER_UTILITY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/utilities/text/shared.h>

#include <stdexcept>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Utility that reads a file and returns its text content.
 *
 * Features:
 * - Reads entire file into memory as text
 * - Can be tagged with Cacheable, Retryable, Monitored behaviors
 * - Composes with text utilities for processing
 *
 * Usage:
 * @code
 * FileReaderUtility reader;
 * text::Text content = reader(dstd::Path("/path/to/file.txt")).get();
 * @endcode
 */
class FileReaderUtility {
   public:
    /**
     * @brief Read file content as text.
     *
     * @param input Path of the regular file to read
     * @return Text containing file content
     * @throws common::DFTUtilsException if the file is missing, is not a
     * regular file or cannot be read
     */
    coro::CoroTask<text::Text> operator()(const dstd::Path& input) const {
        const auto found = dstd::fs::stat(input.view());
        if (!found) {
            if (found.error().is(dstd::Condition::NotFound)) {
                throw common::DFTUtilsException(
                    common::make_error<"File does not exist: {}">(
                        common::ErrorCode::NotFound, input.str())
                        .error());
            }
            throw common::DFTUtilsException(found.error());
        }
        if (found->kind != dstd::fs::FileKind::File) {
            throw common::DFTUtilsException(
                common::make_error<"Path is not a regular file: {}">(
                    common::ErrorCode::Io, input.str())
                    .error());
        }

        auto content = dstd::fs::read_file(input.view());
        if (!content) {
            throw common::DFTUtilsException(
                common::make_error<"Cannot open file: {}">(
                    common::ErrorCode::Io, input.str())
                    .error());
        }

        co_return text::Text{std::move(*content)};
    }
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_FILE_READER_UTILITY_H
