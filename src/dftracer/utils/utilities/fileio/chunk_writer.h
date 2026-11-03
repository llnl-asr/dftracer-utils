// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_CHUNK_WRITER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_CHUNK_WRITER_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::utilities::fileio {

struct ChunkWriterConfig {
    dstd::String output_dir;
    dstd::String base_name;
    std::size_t chunk_size_bytes = 256 * 1024 * 1024;
    /// Uncompressed bytes per gzip member; members end at line boundaries.
    /// A chunk rolls over at the first member boundary at or past
    /// chunk_size_bytes of uncompressed line bytes.
    std::size_t member_size_bytes =
        common::constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    /// Coalesce output into writes of this many bytes so I/O granularity suits
    /// a parallel filesystem (Lustre/GPFS) instead of many tiny writes.
    std::size_t io_flush_bytes = 16 * 1024 * 1024;
    bool compress = true;
    int compression_level = 6;
    /// Bytes the gzip writer may hold, as GzipWriterOptions::memory_budget.
    /// The coalescing buffer of io_flush_bytes is on top of it.
    std::uint64_t memory_budget = 0;

    using ChunkRotationCallback = dstd::Function<void(
        std::size_t chunk_index, dstd::StringView chunk_path,
        std::size_t event_count, std::size_t byte_count)>;
    ChunkRotationCallback on_chunk_complete;

    ChunkWriterConfig& with_output_dir(dstd::StringView dir) {
        output_dir = dir;
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
    dstd::String path;
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
    coro::CoroTask<void> write_line(dstd::ByteView line);
    coro::CoroTask<void> close();

    std::size_t total_bytes_written() const { return total_bytes_; }
    std::size_t total_events_written() const { return total_events_; }
    /// Most bytes the gzip writer reserved at once, known after close().
    std::uint64_t peak_reserved_bytes() const { return peak_bytes_; }
    const dstd::Vector<ChunkInfo>& chunks() const { return chunks_; }
    bool is_open() const { return open_; }

   private:
    ChunkWriterConfig config_;
    dstd::Optional<GzipLineWriter> writer_;
    dstd::String pending_;
    bool open_ = false;
    std::size_t total_bytes_ = 0;
    std::size_t total_events_ = 0;
    std::uint64_t peak_bytes_ = 0;
    dstd::Vector<ChunkInfo> chunks_;
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_CHUNK_WRITER_H
