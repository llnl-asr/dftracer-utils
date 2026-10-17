#include <dftracer/utils/memory/constants.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/heap_page.h>
#include <dftracer/utils/memory/heap_system.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace dftracer::utils::memory {

namespace {

constexpr std::uint8_t BACKEND_UNRESOLVED = 0xff;

std::atomic<std::uint8_t> g_backend{BACKEND_UNRESOLVED};
std::atomic<std::uint32_t> g_decay_ms{constants::DEFAULT_DECAY_MS};

constexpr Backend compiled_backend() noexcept {
#ifdef DFTRACER_UTILS_MEMORY_BACKEND_SYSTEM
    return Backend::System;
#else
    return Config{}.backend;
#endif
}

// The one getenv function in memory: the backend and the decay time must be
// fixed before the first allocation, which can come before common reads the
// environment.
Config resolve_environment() noexcept {
    Config config;
    const char* backend = std::getenv(constants::MEMORY_BACKEND_ENV);
    if (backend == nullptr || backend[0] == '\0') {
        config.backend = compiled_backend();
    } else if (std::strcmp(backend, "page_heap") == 0) {
        config.backend = Backend::PageHeap;
    } else if (std::strcmp(backend, "system") == 0) {
        config.backend = Backend::System;
    } else {
        abort_with(
            "dftracer-utils memory: DFTRACER_UTILS_MEMORY_BACKEND must be "
            "page_heap or system\n");
    }
    const char* decay = std::getenv(constants::DECAY_MS_ENV);
    if (decay != nullptr && decay[0] != '\0') {
        char* end = nullptr;
        errno = 0;
        unsigned long long value = std::strtoull(decay, &end, 10);
        if (decay[0] < '0' || decay[0] > '9' || *end != '\0' || errno != 0 ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            abort_with(
                "dftracer-utils memory: DFTRACER_UTILS_MEM_DECAY_MS must be "
                "a whole number of milliseconds up to 4294967295\n");
        }
        config.decay_ms = static_cast<std::uint32_t>(value);
    }
    return config;
}

// Racing first callers resolve the same values. The decay time is stored
// before the backend, which readers load with acquire.
Backend backend() noexcept {
    std::uint8_t b = g_backend.load(std::memory_order_acquire);
    if (b == BACKEND_UNRESOLVED) [[unlikely]] {
        Config resolved = resolve_environment();
        g_decay_ms.store(resolved.decay_ms, std::memory_order_relaxed);
        b = static_cast<std::uint8_t>(resolved.backend);
        g_backend.store(b, std::memory_order_release);
    }
    return static_cast<Backend>(b);
}

}  // namespace

Config config() noexcept {
    Backend resolved = backend();
    return Config{resolved, g_decay_ms.load(std::memory_order_relaxed)};
}

void* allocate(std::size_t size, std::size_t align) {
    switch (backend()) {
        case Backend::PageHeap:
            return page_allocate(size, align);
        case Backend::System:
            return system_allocate(size, align);
    }
    abort_with("dftracer-utils memory: unknown backend\n");
}

void deallocate(void* p) noexcept {
    switch (backend()) {
        case Backend::PageHeap:
            page_deallocate(p);
            return;
        case Backend::System:
            system_deallocate(p);
            return;
    }
}

void deallocate(void* p, std::size_t size) noexcept {
    switch (backend()) {
        case Backend::PageHeap:
            page_deallocate(p, size);
            return;
        case Backend::System:
            system_deallocate(p, size);
            return;
    }
}

void trim(Mode mode) noexcept {
    switch (backend()) {
        case Backend::PageHeap:
            page_trim(mode);
            return;
        case Backend::System:
            return;
    }
}

}  // namespace dftracer::utils::memory
