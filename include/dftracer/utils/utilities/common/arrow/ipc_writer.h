#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_IPC_WRITER_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_IPC_WRITER_H

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

#include <cstdio>
#include <string>

namespace dftracer::utils::utilities::common::arrow {

/**
 * RAII wrapper for writing Arrow IPC file format (.arrows).
 *
 * Sequence: open() -> write_batch() [1..N] -> close()
 * The first write_batch() call writes the schema; subsequent calls append
 * record batches. close() finalizes the file footer.
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

    // Open path for writing. Returns 0 on success.
    int open(const std::string& path);

    // Write one record batch. First call also writes the schema.
    // Returns 0 on success.
    int write_batch(ArrowExportResult& batch);

    // Finalize footer and close. Returns 0 on success.
    int close();

    bool is_open() const noexcept { return file_ != nullptr; }

   private:
    std::FILE* file_ = nullptr;
    bool schema_written_ = false;
    // Heap-allocated nanoarrow structs stored as void* to avoid pulling
    // nanoarrow_ipc.h into every translation unit that includes this header.
    void* writer_ = nullptr;  // ArrowIpcWriter*
    void* stream_ =
        nullptr;  // ArrowIpcOutputStream* (owned by writer_ after init)

    void reset_state() noexcept;
};

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_IPC_WRITER_H
