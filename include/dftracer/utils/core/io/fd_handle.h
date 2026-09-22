#ifndef DFTRACER_UTILS_CORE_IO_FD_HANDLE_H
#define DFTRACER_UTILS_CORE_IO_FD_HANDLE_H

#include <unistd.h>

namespace dftracer::utils::io {

/// Move-only owner of a raw file descriptor that closes it on destruction.
/// reset() closes any current fd before taking the new one, so a descriptor is
/// closed exactly once across move, reset, and destroy.
class FdHandle {
   public:
    FdHandle() = default;
    explicit FdHandle(int fd) noexcept : fd_(fd) {}
    ~FdHandle() { reset(); }

    FdHandle(FdHandle&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FdHandle& operator=(FdHandle&& other) noexcept {
        if (this != &other) {
            reset(other.fd_);
            other.fd_ = -1;
        }
        return *this;
    }

    FdHandle(const FdHandle&) = delete;
    FdHandle& operator=(const FdHandle&) = delete;

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

    /// Close the current fd (if any) and adopt @p fd (default: none).
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

    int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

   private:
    int fd_ = -1;
};

}  // namespace dftracer::utils::io

#endif  // DFTRACER_UTILS_CORE_IO_FD_HANDLE_H
