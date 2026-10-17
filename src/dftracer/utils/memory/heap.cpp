#include <dftracer/utils/memory/attrib.h>
#include <dftracer/utils/memory/constants.h>
#include <dftracer/utils/memory/debug.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/heap_page.h>
#include <dftracer/utils/memory/heap_system.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/memory/settings.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace dftracer::utils::memory {

namespace {

// The debug values wrap the backend that the same name without Debug selects,
// so the two backends keep their own case and debug costs them nothing.
enum class Dispatch : std::uint8_t {
    PageHeap,
    System,
    DebugPageHeap,
    DebugSystem,
    Unresolved = 0xff,
};

std::atomic<std::uint8_t> g_dispatch{
    static_cast<std::uint8_t>(Dispatch::Unresolved)};
std::atomic<bool> g_exit_check{constants::DEFAULT_EXIT_CHECK};
std::atomic<std::uint32_t> g_decay_ms{constants::DEFAULT_DECAY_MS};
std::atomic<std::size_t> g_hard_cap{0};

constexpr Backend compiled_backend() noexcept {
#ifdef DFTRACER_UTILS_MEMORY_BACKEND_SYSTEM
    return Backend::System;
#else
    return Config{}.backend;
#endif
}

struct Resolved {
    Config config;
    bool debug = constants::DEFAULT_DEBUG;
    bool exit_check = constants::DEFAULT_EXIT_CHECK;
};

bool read_flag(const char* name, bool fallback) noexcept {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return fallback;
    if (std::strcmp(value, "1") == 0) return true;
    if (std::strcmp(value, "0") == 0) return false;
    char message[160];
    std::snprintf(message, sizeof(message),
                  "dftracer-utils memory: %s must be 0 or 1\n", name);
    abort_with(message);
}

// The one getenv function in memory: the backend, the decay time and the
// debug and exit check switches must be fixed before the first allocation,
// which can come before common reads the environment.
Resolved resolve_environment() noexcept {
    Resolved resolved;
    Config& config = resolved.config;
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
    resolved.debug = read_flag(constants::DEBUG_ENV, resolved.debug);
    resolved.exit_check =
        read_flag(constants::EXIT_CHECK_ENV, resolved.exit_check);
    return resolved;
}

constexpr Dispatch dispatch_of(Backend backend, bool debug) noexcept {
    if (backend == Backend::PageHeap) {
        return debug ? Dispatch::DebugPageHeap : Dispatch::PageHeap;
    }
    return debug ? Dispatch::DebugSystem : Dispatch::System;
}

// Racing first callers resolve the same values. The other settings are stored
// before the dispatch value, which readers load with acquire.
[[gnu::noinline]] Dispatch resolve_dispatch() noexcept {
    Resolved resolved = resolve_environment();
    g_decay_ms.store(resolved.config.decay_ms, std::memory_order_relaxed);
    g_exit_check.store(resolved.exit_check, std::memory_order_relaxed);
    Dispatch d = dispatch_of(resolved.config.backend, resolved.debug);
    g_dispatch.store(static_cast<std::uint8_t>(d), std::memory_order_release);
    return d;
}

Dispatch dispatch() noexcept {
    auto d = static_cast<Dispatch>(g_dispatch.load(std::memory_order_acquire));
    if (d == Dispatch::Unresolved) [[unlikely]]
        return resolve_dispatch();
    return d;
}

// The cases that leave the page heap and system paths below tail calls, so
// those two run without a frame of their own. The site is the return address
// of the public entry, which a tail call from there keeps intact.
[[gnu::noinline]] void* allocate_other(std::size_t size, std::size_t align,
                                       void* site) {
    switch (dispatch()) {
        case Dispatch::PageHeap:
            return page_allocate(size, align);
        case Dispatch::System:
            return system_allocate(size, align);
        case Dispatch::DebugPageHeap:
        case Dispatch::DebugSystem:
            return debug_allocate(size, align, site);
        case Dispatch::Unresolved:
            break;
    }
    abort_with("dftracer-utils memory: unknown backend\n");
}

[[gnu::noinline]] void deallocate_other(void* p, std::size_t size,
                                        bool sized) noexcept {
    switch (dispatch()) {
        case Dispatch::PageHeap:
            sized ? page_deallocate(p, size) : page_deallocate(p);
            return;
        case Dispatch::System:
            sized ? system_deallocate(p, size) : system_deallocate(p);
            return;
        case Dispatch::DebugPageHeap:
        case Dispatch::DebugSystem:
            debug_deallocate(p, size, sized);
            return;
        case Dispatch::Unresolved:
            break;
    }
    abort_with("dftracer-utils memory: unknown backend\n");
}

Backend backend() noexcept {
    switch (dispatch()) {
        case Dispatch::PageHeap:
        case Dispatch::DebugPageHeap:
            return Backend::PageHeap;
        case Dispatch::System:
        case Dispatch::DebugSystem:
            return Backend::System;
        case Dispatch::Unresolved:
            break;
    }
    abort_with("dftracer-utils memory: unknown backend\n");
}

}  // namespace

Backend active_backend() noexcept { return backend(); }

bool debug_enabled() noexcept {
    Dispatch d = dispatch();
    return d == Dispatch::DebugPageHeap || d == Dispatch::DebugSystem;
}

bool exit_check_env() noexcept {
    dispatch();
    return g_exit_check.load(std::memory_order_relaxed);
}

std::uint32_t decay_ms() noexcept {
    backend();
    return g_decay_ms.load(std::memory_order_relaxed);
}

std::size_t hard_cap() noexcept {
    return g_hard_cap.load(std::memory_order_relaxed);
}

void set_hard_cap(std::size_t bytes) noexcept {
    g_hard_cap.store(bytes, std::memory_order_relaxed);
}

Config config() noexcept {
    Config out;
    out.backend = backend();
    out.decay_ms = g_decay_ms.load(std::memory_order_relaxed);
    out.budget = Manager::get().budget();
    out.hard_cap = hard_cap();
    out.limit_headroom = Manager::get().limit_headroom();
    out.spill_headroom = Manager::get().spill_headroom();
    return out;
}

AttribCache& attrib_local() {
    switch (backend()) {
        case Backend::PageHeap:
            return page_attrib();
        case Backend::System:
            return system_attrib();
    }
    abort_with("dftracer-utils memory: unknown backend\n");
}

void* allocate(std::size_t size, std::size_t align) {
    switch (static_cast<Dispatch>(g_dispatch.load(std::memory_order_acquire))) {
        case Dispatch::PageHeap:
            return page_allocate(size, align);
        case Dispatch::System:
            return system_allocate(size, align);
        default:
            return allocate_other(size, align, __builtin_return_address(0));
    }
}

void* try_allocate(std::size_t size, std::size_t align) noexcept {
    try {
        if (debug_enabled()) {
            return debug_allocate(size, align, __builtin_return_address(0));
        }
        return allocate(size, align);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void deallocate(void* p) noexcept {
    switch (static_cast<Dispatch>(g_dispatch.load(std::memory_order_acquire))) {
        case Dispatch::PageHeap:
            page_deallocate(p);
            return;
        case Dispatch::System:
            system_deallocate(p);
            return;
        default:
            deallocate_other(p, 0, false);
            return;
    }
}

void deallocate(void* p, std::size_t size) noexcept {
    switch (static_cast<Dispatch>(g_dispatch.load(std::memory_order_acquire))) {
        case Dispatch::PageHeap:
            page_deallocate(p, size);
            return;
        case Dispatch::System:
            system_deallocate(p, size);
            return;
        default:
            deallocate_other(p, size, true);
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
