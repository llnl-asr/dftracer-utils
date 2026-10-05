#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_CHUNK_WRITER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_CHUNK_WRITER_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::fileio {

struct ChunkWriterConfig {
    std::string output_dir;
    std::string base_name;
    std::size_t chunk_size_bytes = 256 * 1024 * 1024;
    /// Uncompressed bytes per gzip member; members end at line boundaries.
    /// A chunk rolls over at the first member boundary at or past
    /// chunk_size_bytes of uncompressed line bytes.
    std::size_t member_size_bytes = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    /// Coalesce output into writes of this many bytes so I/O granularity suits
    /// a parallel filesystem (Lustre/GPFS) instead of many tiny writes.
    std::size_t io_flush_bytes = 16 * 1024 * 1024;
    bool compress = true;
    int compression_level = 6;
    bool json_array_wrapper = true;

    using ChunkRotationCallback = std::function<void(
        std::size_t chunk_index, const std::string& chunk_path,
        std::size_t event_count, std::size_t byte_count)>;
    ChunkRotationCallback on_chunk_complete;

    ChunkWriterConfig& with_output_dir(std::string dir) {
        output_dir = std::move(dir);
        return *this;
    }
    ChunkWriterConfig& with_member_size(std::size_t bytes) {
        member_size_bytes = bytes;
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
    coro::CoroTask<void> close();

    std::size_t total_bytes_written() const { return total_bytes_; }
    std::size_t total_events_written() const { return total_events_; }
    const std::vector<ChunkInfo>& chunks() const { return chunks_; }
    bool is_open() const { return open_; }

   private:
    ChunkWriterConfig config_;
    std::optional<GzipLineWriter> writer_;
    std::string pending_;
    bool open_ = false;
    std::size_t total_bytes_ = 0;
    std::size_t total_events_ = 0;
    std::vector<ChunkInfo> chunks_;
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_CHUNK_WRITER_H
