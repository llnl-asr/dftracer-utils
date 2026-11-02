// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_FILE_WRITER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_FILE_WRITER_UTILITY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/utilities/fileio/types/streaming.h>

#include <fstream>
#include <stdexcept>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Streaming file writer that accepts @ref
 * dftracer::utils::dstd::ByteView "ByteView" chunks.
 *
 * Usage:
 * @code
 * StreamingFileWriterUtility writer("/output.gz");
 * co_await writer.process(ByteView(data, len));
 * writer.close();
 * @endcode
 */
class StreamingFileWriterUtility {
   private:
    std::ofstream file_;
    dstd::Path path_;
    bool append_ = false;
    bool create_dirs_ = true;
    std::size_t total_bytes_ = 0;
    std::size_t total_chunks_ = 0;
    bool opened_ = false;

    void open_file() {
        if (opened_) {
            return;
        }

        // Create parent directories if requested
        const dstd::PathView parent = path_.view().parent();
        if (create_dirs_ && !parent.empty()) {
            const auto found = dstd::fs::exists(parent);
            if (!found) throw common::DFTUtilsException(found.error());
            if (!*found) {
                const auto made = dstd::fs::create_directories(parent);
                if (!made) throw common::DFTUtilsException(made.error());
            }
        }

        // Validate parent directory exists if not creating it
        if (!create_dirs_ && !parent.empty()) {
            const auto found = dstd::fs::exists(parent);
            if (!found) throw common::DFTUtilsException(found.error());
            if (!*found) {
                throw common::DFTUtilsException(
                    common::ErrorCode::NotFound,
                    common::str_cat("Parent directory does not exist: ",
                                    parent.str()));
            }
        }

        // Open file
        std::ios::openmode mode = std::ios::binary;
        if (append_) {
            mode |= std::ios::app;
        } else {
            mode |= std::ios::trunc;
        }

        file_.open(path_.c_str(), mode);
        if (!file_) {
            throw common::DFTUtilsException(
                common::ErrorCode::Io,
                common::str_cat("Cannot open file for writing: ", path_.str()));
        }

        opened_ = true;
    }

   public:
    /**
     * @brief Open file for streaming write.
     *
     * @param path Output file path
     * @param append Append to existing file (default: false)
     * @param create_dirs Create parent directories (default: true)
     */
    explicit StreamingFileWriterUtility(dstd::PathView path,
                                        bool append = false,
                                        bool create_dirs = true)
        : path_(path.str()), append_(append), create_dirs_(create_dirs) {
        open_file();
    }

    ~StreamingFileWriterUtility() {
        if (opened_) {
            close();
        }
    }

    /// Non-copyable
    StreamingFileWriterUtility(const StreamingFileWriterUtility&) = delete;
    StreamingFileWriterUtility& operator=(const StreamingFileWriterUtility&) =
        delete;

    /**
     * @brief Write a single chunk immediately.
     *
     * @param chunk Data chunk to write
     * @return @ref StreamWriteResult with current write status
     */
    coro::CoroTask<StreamWriteResult> process(dstd::ByteView chunk) {
        if (!opened_) {
            throw common::DFTUtilsException(common::ErrorCode::Io,
                                            "Cannot write to closed file");
        }

        if (chunk.empty()) {
            co_return StreamWriteResult::success_result(path_, total_bytes_,
                                                        total_chunks_);
        }

        file_.write(chunk.as<char>(),
                    static_cast<std::streamsize>(chunk.size()));

        if (!file_) {
            throw common::DFTUtilsException(
                common::ErrorCode::Io,
                common::str_cat("Error writing to file: ", path_.str()));
        }

        total_bytes_ += chunk.size();
        total_chunks_++;

        co_return StreamWriteResult::success_result(path_, total_bytes_,
                                                    total_chunks_);
    }

    /**
     * @brief Flush and close the file.
     */
    void close() {
        if (opened_) {
            file_.close();
            opened_ = false;
        }
    }

    std::size_t total_bytes() const { return total_bytes_; }
    std::size_t total_chunks() const { return total_chunks_; }
    const dstd::Path& path() const { return path_; }
    bool is_closed() const { return !opened_; }
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_FILE_WRITER_UTILITY_H
