#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_IPC_WRITER_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_IPC_WRITER_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::common::arrow {

/**
 * Compression type for Arrow IPC buffer-level compression.
 *
 * Buffer-level compression means each buffer in a record batch is compressed
 * independently. This is the standard Arrow IPC compression format and is
 * readable by pyarrow, polars, and other Arrow implementations.
 */
enum class IpcCompression {
    NONE,  // Uncompressed (maximum compatibility)
#ifdef DFTRACER_UTILS_ENABLE_ZSTD
    ZSTD,  // zstd compression (best ratio/speed)
#endif
};

#ifdef DFTRACER_UTILS_ENABLE_ZSTD
constexpr IpcCompression DEFAULT_ARROW_IPC_COMPRESSION = IpcCompression::ZSTD;
#else
constexpr IpcCompression DEFAULT_ARROW_IPC_COMPRESSION = IpcCompression::NONE;
#endif

class BufferPool {
   public:
    static constexpr std::size_t DEFAULT_BUFFER_CAPACITY = 4 * 1024 * 1024;

    struct Slot {
        std::vector<uint8_t> data;
        std::atomic<bool> in_use{false};
    };

    explicit BufferPool(std::size_t num_slots = 4,
                        std::size_t initial_capacity = DEFAULT_BUFFER_CAPACITY);
    ~BufferPool() = default;

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&) = default;
    BufferPool& operator=(BufferPool&&) = default;

    Slot* acquire(std::size_t min_capacity = 0);
    void release(Slot* slot);
    std::size_t size() const { return slots_.size(); }

   private:
    std::vector<std::unique_ptr<Slot>> slots_;
};

/**
 * Async Arrow IPC file writer (.arrow).
 *
 * Uses Executor::current() for async I/O - must be called from within executor.
 * Supports buffer-level compression (zstd) compatible with pyarrow, polars,
 * nanoarrow, and other Arrow IPC readers.
 *
 * Usage: open() -> write_batch() [1..N] -> close()
 *
 * Move-only. Not thread-safe.
 */
class IpcWriter {
   public:
    IpcWriter() = default;
    ~IpcWriter();

    IpcWriter(const IpcWriter&) = delete;
    IpcWriter& operator=(const IpcWriter&) = delete;
    IpcWriter(IpcWriter&& other) noexcept;
    IpcWriter& operator=(IpcWriter&& other) noexcept;

    coro::CoroTask<int> open(
        const std::string& path,
        IpcCompression compression = DEFAULT_ARROW_IPC_COMPRESSION,
        std::size_t pool_slots = 4);

    coro::CoroTask<int> write_batch(ArrowExportResult& batch);
    coro::CoroTask<int> write_batches(std::vector<ArrowExportResult>& batches);
    coro::CoroTask<int> close();

    bool is_open() const noexcept { return fd_ >= 0; }

   private:
    int fd_ = -1;
    off_t write_offset_ = 0;
    BufferPool buffer_pool_;
    bool schema_written_ = false;
    IpcCompression compression_ = DEFAULT_ARROW_IPC_COMPRESSION;
    void* batch_blocks_ = nullptr;
    void* schema_copy_ = nullptr;

    void reset_state() noexcept;

    struct CompressedBatch {
        std::vector<uint8_t> header;
        BufferPool::Slot* body_slot;
        std::size_t body_size;
        std::int32_t metadata_length;
        std::int64_t body_length;
    };

    coro::CoroTask<CompressedBatch> compress_batch(ArrowExportResult& batch);
    coro::CoroTask<int> write_compressed(CompressedBatch& cb);
    coro::CoroTask<int> write_schema(ArrowExportResult& batch);
    coro::CoroTask<int> write_footer();
};

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_IPC_WRITER_H
