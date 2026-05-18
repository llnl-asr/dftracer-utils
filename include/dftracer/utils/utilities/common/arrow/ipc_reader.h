#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_IPC_READER_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_IPC_READER_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::common::arrow::detail {
// Store block info separately from decoder state
struct IpcBlock {
    std::int64_t offset;
    std::int32_t metadata_length;
    std::int64_t body_length;
};
}  // namespace dftracer::utils::utilities::common::arrow::detail

namespace dftracer::utils::utilities::common::arrow {

/**
 * RAII reader for Arrow IPC file format (.arrow).
 *
 * Optimized with:
 * - Memory-mapped I/O for zero-copy file access
 * - Shared schema (no deep copy per batch)
 * - Buffer reuse for decompression
 *
 * Supports buffer-level ZSTD decompression compatible with
 * pyarrow, polars, and this library's IpcWriter.
 *
 * Sequence: open() -> num_batches() -> read_batch(i) [or read_all()]
 *
 * Move-only. Not thread-safe.
 */
class IpcReader {
   public:
    IpcReader() = default;
    ~IpcReader();

    IpcReader(const IpcReader&) = delete;
    IpcReader& operator=(const IpcReader&) = delete;
    IpcReader(IpcReader&& other) noexcept;
    IpcReader& operator=(IpcReader&& other) noexcept;

    // Open file for reading. Returns 0 on success.
    int open(const std::string& path);

    // Close the file.
    void close();

    bool is_open() const noexcept { return mapped_data_ != nullptr; }

    // Number of record batches in the file.
    std::size_t num_batches() const noexcept { return num_batches_; }

    // Total rows across all batches.
    std::int64_t total_rows() const noexcept { return total_rows_; }

    // Read a single batch by index. Returns empty result on error.
    ArrowExportResult read_batch(std::size_t index);

    // Read all batches and return as a vector.
    std::vector<ArrowExportResult> read_all();

    // Iterate over all batches, calling callback for each.
    // Returns 0 on success, non-zero if callback returns non-zero or on error.
    int for_each_batch(std::function<int(ArrowExportResult&)> callback);

   private:
    // Memory-mapped file data
    void* mapped_data_ = nullptr;
    std::size_t mapped_size_ = 0;
    int fd_ = -1;

    // Decoder state
    void* decoder_ = nullptr;  // ArrowIpcDecoder*

    // Shared schema (not deep-copied per batch)
    std::shared_ptr<void> shared_schema_;  // ArrowSchema*, ref-counted

    // Block metadata
    std::vector<detail::IpcBlock> blocks_;
    std::size_t num_batches_ = 0;
    std::int64_t total_rows_ = 0;

    void reset_state() noexcept;
    int read_footer();
};

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_IPC_READER_H
