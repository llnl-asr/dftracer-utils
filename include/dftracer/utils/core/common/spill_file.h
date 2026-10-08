#ifndef DFTRACER_UTILS_CORE_COMMON_SPILL_FILE_H
#define DFTRACER_UTILS_CORE_COMMON_SPILL_FILE_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/scoped_fd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace dftracer::utils {

/// One temp file in spill_dir(), unlinked at creation so a crash leaves
/// nothing behind. append() and read() may be called from many threads; the
/// bytes are in native endianness and never leave the machine.
class SpillFile {
   public:
    /// IO error naming the directory and the variable when the file cannot be
    /// created.
    static Result<std::unique_ptr<SpillFile>> create();

    SpillFile(const SpillFile&) = delete;
    SpillFile& operator=(const SpillFile&) = delete;

    /// Writes `n` bytes at a fresh offset and returns it. IO error when the
    /// write fails (a full disk included).
    Result<std::uint64_t> append(const void* data, std::size_t n);

    /// Reads exactly `n` bytes at `offset`. IO error on a short read.
    Result<void> read(std::uint64_t offset, void* out, std::size_t n) const;

    std::uint64_t size() const noexcept {
        return end_.load(std::memory_order_relaxed);
    }

   private:
    SpillFile() = default;

    ScopedFd fd_;
    std::atomic<std::uint64_t> end_{0};
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_SPILL_FILE_H
