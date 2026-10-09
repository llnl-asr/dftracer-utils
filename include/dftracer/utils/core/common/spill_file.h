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

    /// Takes `bytes` at an offset that is a multiple of `align` without
    /// writing, for a caller that writes it with write_at() and maps it.
    std::uint64_t reserve(std::size_t bytes, std::size_t align = 1);

    /// Writes `n` bytes at `offset`, one taken by reserve(). IO error when the
    /// write fails (a full disk included).
    Result<void> write_at(std::uint64_t offset, const void* data,
                          std::size_t n);

    /// The descriptor, for a caller that maps the file.
    int fd() const noexcept { return fd_.get(); }

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
