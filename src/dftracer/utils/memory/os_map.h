#ifndef DFTRACER_UTILS_MEMORY_OS_MAP_H
#define DFTRACER_UTILS_MEMORY_OS_MAP_H

#include <sys/mman.h>

#include <cstddef>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace dftracer::utils::memory {

// The heap maps its memory with system calls, not through the libc symbols.
// A tracer that wraps munmap (DFTracer) allocates in its wrapper, which
// reaches this heap again before the first call returns. macOS has no
// supported syscall(); a library there does not take another's calls.
#if defined(__linux__)
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

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_OS_MAP_H
