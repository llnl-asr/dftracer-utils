// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_SCOPED_FD_H
#define DFTRACER_UTILS_DSTD_SCOPED_FD_H

#include <unistd.h>

#include <cstdio>

namespace dftracer::utils::dstd {

/// Move-only owner of a raw file descriptor that closes it on destruction.
/// reset() closes any current fd before taking the new one, so a descriptor is
/// closed exactly once across move, reset and destroy.
struct ScopedFd {
    int value = -1;

    ScopedFd() = default;
    explicit ScopedFd(int fd) noexcept : value(fd) {}

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : value(other.value) {
        other.value = -1;
    }

    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset(other.value);
            other.value = -1;
        }
        return *this;
    }

    ~ScopedFd() { reset(); }

    /// Closes the current fd, if any, and adopts @p fd.
    void reset(int fd = -1) noexcept {
        if (value >= 0) ::close(value);
        value = fd;
    }

    int release() noexcept {
        const int fd = value;
        value = -1;
        return fd;
    }

    int get() const noexcept { return value; }
    bool valid() const noexcept { return value >= 0; }
};

/// Move-only owner of a FILE* that fcloses it on destruction. A null handle
/// closes nothing.
class ScopedFile {
   public:
    ScopedFile() = default;
    explicit ScopedFile(std::FILE* f) noexcept : f_(f) {}
    ~ScopedFile() { reset(); }

    ScopedFile(ScopedFile&& other) noexcept : f_(other.f_) {
        other.f_ = nullptr;
    }
    ScopedFile& operator=(ScopedFile&& other) noexcept {
        if (this != &other) {
            reset(other.f_);
            other.f_ = nullptr;
        }
        return *this;
    }

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    /// Closes the current file, if any, and adopts @p f.
    void reset(std::FILE* f = nullptr) noexcept {
        if (f_) std::fclose(f_);
        f_ = f;
    }

    std::FILE* release() noexcept {
        std::FILE* f = f_;
        f_ = nullptr;
        return f;
    }

    std::FILE* get() const noexcept { return f_; }
    explicit operator bool() const noexcept { return f_ != nullptr; }

   private:
    std::FILE* f_ = nullptr;
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_SCOPED_FD_H
