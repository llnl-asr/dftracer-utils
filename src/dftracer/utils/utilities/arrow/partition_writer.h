// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_ARROW_PARTITION_WRITER_H
#define DFTRACER_UTILS_UTILITIES_ARROW_PARTITION_WRITER_H

#include <dftracer/utils/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/arrow/arrow_export.h>
#include <dftracer/utils/utilities/arrow/ipc_writer.h>

#include <cstdint>

namespace dftracer::utils::utilities::arrow {

struct PartitionWriteStats {
    dstd::Vector<dstd::String> files;
    dstd::Vector<int64_t> row_counts;
    int64_t total_rows = 0;
    int64_t total_uncompressed_bytes = 0;
};

/**
 * Async wrapper around @ref IpcWriter with automatic file rotation.
 * Writes part-NNNNN.arrow files, rotating when size threshold is exceeded.
 */
class PartitionWriter {
   public:
    PartitionWriter() = default;
    ~PartitionWriter();

    PartitionWriter(const PartitionWriter&) = delete;
    PartitionWriter& operator=(const PartitionWriter&) = delete;
    PartitionWriter(PartitionWriter&& other) noexcept;
    PartitionWriter& operator=(PartitionWriter&& other) noexcept;

    coro::CoroTask<int> open(
        dstd::StringView output_dir, int64_t chunk_size_bytes,
        IpcCompression compression = DEFAULT_ARROW_IPC_COMPRESSION);

    coro::CoroTask<int> write_batch(ArrowExportResult& batch);
    coro::CoroTask<PartitionWriteStats> close();

    bool is_open() const noexcept { return is_open_; }
    int64_t total_bytes() const noexcept { return total_bytes_; }
    int64_t total_rows() const noexcept { return total_rows_; }
    size_t file_count() const noexcept { return file_index_; }

   private:
    dstd::String output_dir_;
    int64_t chunk_size_bytes_ = 0;
    IpcCompression compression_ = DEFAULT_ARROW_IPC_COMPRESSION;

    IpcWriter writer_;
    bool is_open_ = false;
    size_t file_index_ = 0;

    int64_t current_file_bytes_ = 0;
    int64_t current_file_rows_ = 0;
    int64_t total_bytes_ = 0;
    int64_t total_rows_ = 0;

    dstd::Vector<dstd::String> files_;
    dstd::Vector<int64_t> row_counts_;

    dstd::String generate_filename() const;
    coro::CoroTask<int> rotate_file();
    int64_t calculate_uncompressed_size(ArrowExportResult& batch);
};

}  // namespace dftracer::utils::utilities::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
#endif  // DFTRACER_UTILS_UTILITIES_ARROW_PARTITION_WRITER_H
