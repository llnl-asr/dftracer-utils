#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_FILE_WRITER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_FILE_WRITER_UTILITY_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/fileio/types/streaming.h>

#include <fstream>
#include <stdexcept>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Streaming file writer that accepts ByteView chunks.
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
    fs::path path_;
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
        if (create_dirs_ && path_.has_parent_path()) {
            fs::path parent = path_.parent_path();
            if (!fs::exists(parent)) {
                fs::create_directories(parent);
            }
        }

        // Validate parent directory exists if not creating it
        if (!create_dirs_ && path_.has_parent_path()) {
            fs::path parent = path_.parent_path();
            if (!fs::exists(parent)) {
                throw std::runtime_error("Parent directory does not exist: " +
                                         parent.string());
            }
        }

        // Open file
        std::ios::openmode mode = std::ios::binary;
        if (append_) {
            mode |= std::ios::app;
        } else {
            mode |= std::ios::trunc;
        }

        file_.open(path_, mode);
        if (!file_) {
            throw std::runtime_error("Cannot open file for writing: " +
                                     path_.string());
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
    explicit StreamingFileWriterUtility(fs::path path, bool append = false,
                                        bool create_dirs = true)
        : path_(std::move(path)), append_(append), create_dirs_(create_dirs) {
        open_file();
    }

    ~StreamingFileWriterUtility() {
        if (opened_) {
            close();
        }
    }

    // Non-copyable
    StreamingFileWriterUtility(const StreamingFileWriterUtility&) = delete;
    StreamingFileWriterUtility& operator=(const StreamingFileWriterUtility&) =
        delete;

    /**
     * @brief Write a single chunk immediately.
     *
     * @param chunk Data chunk to write
     * @return StreamWriteResult with current write status
     */
    coro::CoroTask<StreamWriteResult> process(ByteView chunk) {
        if (!opened_) {
            throw std::runtime_error("Cannot write to closed file");
        }

        if (chunk.empty()) {
            co_return StreamWriteResult::success_result(path_, total_bytes_,
                                                        total_chunks_);
        }

        file_.write(chunk.as<char>(),
                    static_cast<std::streamsize>(chunk.size()));

        if (!file_) {
            throw std::runtime_error("Error writing to file: " +
                                     path_.string());
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

    bool append_mode() const { return append_; }
    bool create_dirs_mode() const { return create_dirs_; }
    std::size_t total_bytes() const { return total_bytes_; }
    std::size_t total_chunks() const { return total_chunks_; }
    const fs::path& path() const { return path_; }
    bool is_opened() const { return opened_; }
    bool is_closed() const { return !opened_; }
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_FILE_WRITER_UTILITY_H
