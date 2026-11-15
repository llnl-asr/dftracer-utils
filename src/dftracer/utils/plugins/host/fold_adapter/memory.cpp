// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/cursor_host.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/host_status.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>

#include <cstdint>

namespace dftracer::utils::plugins::host {

namespace {

PluginReservation* reservation_of(::dftu_reservation* r) {
    return reinterpret_cast<PluginReservation*>(r);
}

// The service functions of one kind of host: `h` is the PluginFold of a fold's
// host or the CursorHost of a cursor's. A Scope has memory(), memory_budget()
// and watch_memory_requests().
template <class Scope>
struct MemoryService {
    static PluginMemory* memory_of(void* h) {
        return h ? &static_cast<Scope*>(h)->memory() : nullptr;
    }

    static void* alloc(void* h, std::uint64_t size, std::uint32_t align) {
        PluginMemory* memory = memory_of(h);
        return memory ? memory->alloc(size, align) : nullptr;
    }

    static void* realloc(void* h, void* p, std::uint64_t old_size,
                         std::uint64_t new_size, std::uint32_t align) {
        PluginMemory* memory = memory_of(h);
        return memory ? memory->realloc(p, old_size, new_size, align) : nullptr;
    }

    static void free(void* h, void* p, std::uint64_t size, std::uint32_t) {
        if (PluginMemory* memory = memory_of(h)) memory->free(p, size);
    }

    static dftu_status reserve(::dftu_ctx* ctx, void* h, ::dftu_reservation** r,
                               const char* label) {
        PluginMemory* memory = memory_of(h);
        if (memory == nullptr || r == nullptr)
            return host_fail(ctx, DFTU_E_INVALID,
                             "reserve: the host and the out slot must be set");
        const std::uint64_t budget = static_cast<Scope*>(h)->memory_budget();
        const std::uint64_t cap =
            dftracer::utils::memory::resolve_spill_budget(budget);
        PluginReservation* made = memory->reserve(
            label ? dstd::StringView(label) : dstd::StringView(),
            cap == dftracer::utils::memory::NO_SPILL_BUDGET ? 0 : cap, h);
        if (made == nullptr)
            return host_fail(ctx, DFTU_E_NO_MEMORY,
                             "reserve: the plugin's memory is closed");
        static_cast<Scope*>(h)->watch_memory_requests();
        *r = reinterpret_cast<::dftu_reservation*>(made);
        return DFTU_OK;
    }

    static dftu_status try_grow(::dftu_ctx* ctx, ::dftu_reservation* r,
                                std::uint64_t bytes) {
        PluginReservation* res = reservation_of(r);
        if (res == nullptr)
            return host_fail(ctx, DFTU_E_INVALID,
                             "try_grow: the reservation is NULL");
        dstd::String why;
        if (res->owner->try_grow(*res, bytes, why)) return DFTU_OK;
        return host_fail(
            ctx, DFTU_E_NO_MEMORY,
            dstd::fmt<"try_grow '{}': {}">(dstd::StringView(res->label),
                                           dstd::StringView(why)));
    }

    static void shrink(::dftu_reservation* r, std::uint64_t bytes) {
        if (PluginReservation* res = reservation_of(r))
            res->owner->shrink(*res, bytes);
    }

    static void release(::dftu_reservation* r) {
        if (PluginReservation* res = reservation_of(r))
            res->owner->release(res);
    }

    static void set_reclaimer(::dftu_reservation* r,
                              std::uint64_t (*fn)(void* ud, std::uint64_t want),
                              void* ud) {
        PluginReservation* res = reservation_of(r);
        if (res == nullptr) return;
        res->reclaimer = fn;
        res->reclaimer_ud = fn ? ud : nullptr;
    }

    static constexpr dftu_svc_memory VTABLE = {sizeof(dftu_svc_memory),
                                               alloc,
                                               realloc,
                                               free,
                                               reserve,
                                               try_grow,
                                               shrink,
                                               release,
                                               set_reclaimer};
};

}  // namespace

const void* detail::memory_ext_vtable() {
    return &MemoryService<PluginFold>::VTABLE;
}

const void* detail::cursor_memory_ext_vtable() {
    return &MemoryService<CursorHost>::VTABLE;
}

}  // namespace dftracer::utils::plugins::host
