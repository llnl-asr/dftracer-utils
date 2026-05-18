#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_PARTITION_WRITER_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_PARTITION_WRITER_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#include <dftracer/utils/utilities/common/arrow/ipc_writer.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::common::arrow {

struct PartitionWriteStats {
    std::vector<std::string> files;
    std::vector<int64_t> row_counts;
    int64_t total_rows = 0;
    int64_t total_uncompressed_bytes = 0;
};

/**
 * Async wrapper around IpcWriter with automatic file rotation.
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
        const std::string& output_dir, int64_t chunk_size_bytes,
        IpcCompression compression = DEFAULT_ARROW_IPC_COMPRESSION);

    coro::CoroTask<int> write_batch(ArrowExportResult& batch);
    coro::CoroTask<PartitionWriteStats> close();

    bool is_open() const noexcept { return is_open_; }
    int64_t current_file_bytes() const noexcept { return current_file_bytes_; }
    int64_t total_bytes() const noexcept { return total_bytes_; }
    int64_t total_rows() const noexcept { return total_rows_; }
    size_t file_count() const noexcept { return file_index_; }

   private:
    std::string output_dir_;
    int64_t chunk_size_bytes_ = 0;
    IpcCompression compression_ = DEFAULT_ARROW_IPC_COMPRESSION;

    IpcWriter writer_;
    bool is_open_ = false;
    size_t file_index_ = 0;

    int64_t current_file_bytes_ = 0;
    int64_t current_file_rows_ = 0;
    int64_t total_bytes_ = 0;
    int64_t total_rows_ = 0;

    std::vector<std::string> files_;
    std::vector<int64_t> row_counts_;

    std::string generate_filename() const;
    coro::CoroTask<int> rotate_file();
    int64_t calculate_uncompressed_size(ArrowExportResult& batch);
};

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_PARTITION_WRITER_H
