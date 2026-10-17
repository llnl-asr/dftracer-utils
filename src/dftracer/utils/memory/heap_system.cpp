#include <dftracer/utils/memory/heap_system.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace dftracer::utils::memory {

void abort_with(const char* message) noexcept {
    std::size_t left = std::strlen(message);
    while (left > 0) {
        ssize_t n = ::write(STDERR_FILENO, message, left);
        if (n <= 0) break;
        message += n;
        left -= static_cast<std::size_t>(n);
    }
    std::abort();
}

// posix_memalign, not aligned_alloc: aligned_alloc rounds the size up to the
// alignment, and the slack would hide an overrun from ASan.
void* system_allocate(std::size_t size, std::size_t align) {
    if (align == 0 || (align & (align - 1)) != 0) {
        abort_with("dftracer-utils memory: alignment is not a power of two\n");
    }
    std::size_t offset =
        align > SYSTEM_HEADER_SIZE ? align : SYSTEM_HEADER_SIZE;
    if (offset > std::numeric_limits<std::uint32_t>::max() ||
        size > std::numeric_limits<std::size_t>::max() - offset) {
        throw std::bad_alloc();
    }
    void* base = nullptr;
    if (::posix_memalign(&base, offset, offset + size) != 0) {
        throw std::bad_alloc();
    }
    void* p = static_cast<char*>(base) + offset;
    *system_header(p) = SystemHeader{static_cast<std::uint64_t>(size),
                                     static_cast<std::uint32_t>(offset), 0, 0};
    return p;
}

void system_deallocate(void* p) noexcept {
    if (p == nullptr) return;
    std::free(static_cast<char*>(p) - system_header(p)->offset);
}

void system_deallocate(void* p, std::size_t size) noexcept {
    if (p == nullptr) return;
#ifdef DFTRACER_UTILS_MEMORY_CHECKS
    if (system_header(p)->size != size) {
        abort_with(
            "dftracer-utils memory: sized free does not match the allocated "
            "size\n");
    }
#else
    (void)size;
#endif
    system_deallocate(p);
}

}  // namespace dftracer::utils::memory
