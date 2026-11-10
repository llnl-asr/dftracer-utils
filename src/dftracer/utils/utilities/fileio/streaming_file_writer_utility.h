// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_FILE_WRITER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_FILE_WRITER_UTILITY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/utilities/fileio/types/streaming.h>

#include <cstdio>
#include <utility>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Streaming file writer that accepts @ref
 * dftracer::utils::dstd::ByteView "ByteView" chunks.
 *
 * Usage:
 * @code
 * auto writer = StreamingFileWriterUtility::open("/output.gz");
 * if (!writer) return writer.error();
 * co_await writer->process(ByteView(data, len));
 * writer->close();
 * @endcode
 */
class StreamingFileWriterUtility {
   public:
    /**
     * @brief Open file for streaming write.
     *
     * @param path Output file path
     * @param append Append to existing file (default: false)
     * @param create_dirs Create parent directories (default: true)
     * @return The writer, a NotFound error when the parent directory is
     * missing and `create_dirs` is false, or an Io error when the file cannot
     * be opened.
     */
    static dstd::Result<StreamingFileWriterUtility> open(
        dstd::PathView path, bool append = false, bool create_dirs = true) {
        StreamingFileWriterUtility writer(path);
        const dstd::PathView parent = writer.path_.view().parent();
        if (!parent.empty()) {
            const auto found = dstd::fs::exists(parent);
            if (!found.has_value()) return found.error();
            if (!*found) {
                if (!create_dirs) {
                    return common::make_error<
                        "Parent directory does not exist: {}">(
                        common::ErrorCode::NotFound, parent.str());
                }
                const auto made = dstd::fs::create_directories(parent);
                if (!made.has_value()) return made.error();
            }
        }

        writer.file_.reset(
            std::fopen(writer.path_.c_str(), append ? "ab" : "wb"));
        if (!writer.file_) {
            return common::make_error<"Cannot open file for writing: {}">(
                common::ErrorCode::Io, writer.path_.str());
        }
        writer.opened_ = true;
        return writer;
    }

    ~StreamingFileWriterUtility() { close(); }

    /// Non-copyable
    StreamingFileWriterUtility(const StreamingFileWriterUtility&) = delete;
    StreamingFileWriterUtility& operator=(const StreamingFileWriterUtility&) =
        delete;

    StreamingFileWriterUtility(StreamingFileWriterUtility&& other) noexcept
        : file_(std::move(other.file_)),
          path_(std::move(other.path_)),
          total_bytes_(other.total_bytes_),
          total_chunks_(other.total_chunks_),
          opened_(other.opened_) {
        other.opened_ = false;
    }

    StreamingFileWriterUtility& operator=(StreamingFileWriterUtility&&) =
        delete;

    /**
     * @brief Write a single chunk immediately.
     *
     * @param chunk Data chunk to write
     * @return @ref StreamWriteResult with current write status, or an Io error
     * when the file is closed or the write fails.
     */
    coro::CoroTask<dstd::Result<StreamWriteResult>> process(
        dstd::ByteView chunk) {
        if (!opened_) {
            co_return common::make_error(common::ErrorCode::Io,
                                         "Cannot write to closed file");
        }

        if (chunk.empty()) {
            co_return StreamWriteResult::success_result(path_, total_bytes_,
                                                        total_chunks_);
        }

        if (std::fwrite(chunk.as<char>(), 1, chunk.size(), file_.get()) !=
            chunk.size()) {
            co_return common::make_error<"Error writing to file: {}">(
                common::ErrorCode::Io, path_.str());
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
            file_.reset();
            opened_ = false;
        }
    }

    std::size_t total_bytes() const { return total_bytes_; }
    std::size_t total_chunks() const { return total_chunks_; }
    const dstd::Path& path() const { return path_; }
    bool is_closed() const { return !opened_; }

   private:
    explicit StreamingFileWriterUtility(dstd::PathView path)
        : path_(path.str()) {}

    dstd::ScopedFile file_;
    dstd::Path path_;
    std::size_t total_bytes_ = 0;
    std::size_t total_chunks_ = 0;
    bool opened_ = false;
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_FILE_WRITER_UTILITY_H
