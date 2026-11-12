// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_MEMORY_OS_MAP_H
#define DFTRACER_UTILS_MEMORY_OS_MAP_H

#include <sys/mman.h>

#include <cstddef>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace dftracer::utils::memory::detail {

#if defined(__SANITIZE_THREAD__)
#define DFTRACER_UTILS_MEMORY_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DFTRACER_UTILS_MEMORY_TSAN 1
#endif
#endif

// The heap maps with system calls, not the libc symbols. A tracer that wraps
// munmap (DFTracer) allocates in its wrapper and reaches this heap again.
// ThreadSanitizer forgets a range's history only through the libc calls it
// intercepts, so a sanitized build uses those. macOS has no syscall().
#if defined(__linux__) && !defined(DFTRACER_UTILS_MEMORY_TSAN)
inline void* os_mmap(void* addr, std::size_t len, int prot, int flags, int fd,
                     off_t off) noexcept {
    return reinterpret_cast<void*>(
        ::syscall(SYS_mmap, addr, len, prot, flags, fd, off));
}
inline int os_munmap(void* addr, std::size_t len) noexcept {
    return static_cast<int>(::syscall(SYS_munmap, addr, len));
}
inline int os_madvise(void* addr, std::size_t len, int advice) noexcept {
    return static_cast<int>(::syscall(SYS_madvise, addr, len, advice));
}
#else
inline void* os_mmap(void* addr, std::size_t len, int prot, int flags, int fd,
                     off_t off) noexcept {
    return ::mmap(addr, len, prot, flags, fd, off);
}
inline int os_munmap(void* addr, std::size_t len) noexcept {
    return ::munmap(addr, len);
}
inline int os_madvise(void* addr, std::size_t len, int advice) noexcept {
    return ::madvise(addr, len, advice);
}
#endif

}  // namespace dftracer::utils::memory::detail

#endif  // DFTRACER_UTILS_MEMORY_OS_MAP_H
