#ifndef DFTRACER_UTILS_UTILITIES_IO_STREAMING_FILE_WRITER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_IO_STREAMING_FILE_WRITER_UTILITY_H

#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/io/types/types.h>

#include <fstream>
#include <stdexcept>

namespace dftracer::utils::utilities::io {

/**
 * @brief Streaming file writer for lazy chunk iteration.
 *
 * This writer writes chunks as they are produced by an iterator,
 * maintaining constant memory usage.
 *
 * Usage pattern (true streaming):
 * @code
 * // Read, compress, and write in one pass - constant memory!
 * auto reader = std::make_shared<StreamingFileReader>();
 * gzip::StreamingCompressor compressor(9);
 * StreamingFileWriter writer("/output.gz");
 *
 * ChunkRange chunks = reader->process(StreamReadInput{"/large/file.txt"});
 * for (const auto& chunk : chunks) {
 *     // Compress chunk
 *     auto compressed = compressor.compress_chunk(chunk);
 *
 *     // Write immediately (constant memory!)
 *     for (const auto& out_chunk : compressed) {
 *         writer.write_chunk(RawData{out_chunk.data});
 *     }
 * }
 *
 * // Finalize compression and write remaining data
 * auto final = compressor.finalize();
 * for (const auto& chunk : final) {
 *     writer.write_chunk(RawData{chunk.data});
 * }
 *
 * writer.close();
 * std::cout << "Wrote " << writer.total_bytes() << " bytes\n";
 * @endcode
 */
class StreamingFileWriterUtility
    : public dftracer::utils::utilities::Utility<RawData, StreamWriteResult> {
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
    StreamWriteResult process(const RawData& chunk) override {
        if (!opened_) {
            throw std::runtime_error("Cannot write to closed file");
        }

        if (chunk.empty()) {
            return StreamWriteResult::success_result(path_, total_bytes_,
                                                     total_chunks_);
        }

        file_.write(reinterpret_cast<const char*>(chunk.data.data()),
                    static_cast<std::streamsize>(chunk.size()));

        if (!file_) {
            throw std::runtime_error("Error writing to file: " +
                                     path_.string());
        }

        total_bytes_ += chunk.size();
        total_chunks_++;

        return StreamWriteResult::success_result(path_, total_bytes_,
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

}  // namespace dftracer::utils::utilities::io

#endif  // DFTRACER_UTILS_UTILITIES_IO_STREAMING_FILE_WRITER_UTILITY_H
