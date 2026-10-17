#ifndef DFTRACER_UTILS_MEMORY_HEAP_SYSTEM_H
#define DFTRACER_UTILS_MEMORY_HEAP_SYSTEM_H

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::memory {

/// Sits right before the user pointer of a system block.
struct SystemHeader {
    std::uint64_t size;
    /// Bytes from the start of the C library block to the user pointer.
    std::uint32_t offset;
    std::uint16_t pool_id;
    std::uint16_t reserved;
};
static_assert(sizeof(SystemHeader) == 16);

inline constexpr std::size_t SYSTEM_HEADER_SIZE = sizeof(SystemHeader);

inline SystemHeader* system_header(void* p) noexcept {
    return static_cast<SystemHeader*>(p) - 1;
}

void* system_allocate(std::size_t size, std::size_t align);
void system_deallocate(void* p) noexcept;
void system_deallocate(void* p, std::size_t size) noexcept;

[[noreturn]] void abort_with(const char* message) noexcept;

}  // namespace dftracer::utils::memory

#endif  // DFTRACER_UTILS_MEMORY_HEAP_SYSTEM_H
