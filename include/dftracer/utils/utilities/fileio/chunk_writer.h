#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_CHUNK_WRITER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_CHUNK_WRITER_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::fileio {

struct ChunkWriterConfig {
    std::string output_dir;
    std::string base_name;
    std::size_t chunk_size_bytes = 256 * 1024 * 1024;
    bool compress = true;
    int compression_level = Z_DEFAULT_COMPRESSION;
    bool json_array_wrapper = true;

    ChunkWriterConfig& with_output_dir(std::string dir) {
        output_dir = std::move(dir);
        return *this;
    }
    ChunkWriterConfig& with_base_name(std::string name) {
        base_name = std::move(name);
        return *this;
    }
    ChunkWriterConfig& with_chunk_size(std::size_t bytes) {
        chunk_size_bytes = bytes;
        return *this;
    }
    ChunkWriterConfig& with_compression(bool enabled) {
        compress = enabled;
        return *this;
    }
    ChunkWriterConfig& with_compression_level(int level) {
        compression_level = level;
        return *this;
    }
    ChunkWriterConfig& with_json_array_wrapper(bool enabled) {
        json_array_wrapper = enabled;
        return *this;
    }
};

struct ChunkInfo {
    std::string path;
    std::size_t bytes_written = 0;
    std::size_t events_written = 0;
    int chunk_index = 0;
};

class ChunkWriter {
   public:
    explicit ChunkWriter(ChunkWriterConfig config);
    ~ChunkWriter();

    ChunkWriter(const ChunkWriter&) = delete;
    ChunkWriter& operator=(const ChunkWriter&) = delete;

    coro::CoroTask<void> open();
    coro::CoroTask<void> write_line(ByteView line);
    coro::CoroTask<void> write_bytes(ByteView data);
    coro::CoroTask<void> close();

    std::size_t total_bytes_written() const { return total_bytes_; }
    std::size_t total_events_written() const { return total_events_; }
    int current_chunk_index() const { return chunk_index_; }
    const std::vector<ChunkInfo>& chunks() const { return chunks_; }
    bool is_open() const { return open_; }

   private:
    coro::CoroTask<void> flush_buffer();
    coro::CoroTask<void> flush_raw(const char* data, std::size_t len);
    coro::CoroTask<void> finalize_current_chunk();
    coro::CoroTask<void> open_next_chunk();
    std::string chunk_path(int index) const;

    ChunkWriterConfig config_;
    int fd_ = -1;
    bool open_ = false;
    int chunk_index_ = 0;
    std::size_t current_chunk_bytes_ = 0;
    std::size_t current_chunk_events_ = 0;
    std::size_t total_bytes_ = 0;
    std::size_t total_events_ = 0;

    static constexpr std::size_t WRITE_BUFFER_SIZE = 256 * 1024;
    std::vector<char> write_buffer_;

    std::unique_ptr<compression::zlib::ManualStreamingCompressorUtility>
        compressor_;

    std::vector<ChunkInfo> chunks_;
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_CHUNK_WRITER_H
