// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGIN_MEMORY_H
#define DFTRACER_UTILS_PLUGINS_PLUGIN_MEMORY_H

#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugin/map.h>
#include <dftracer/utils/plugins/plugin/types.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::plugins {

/// A claim on the memory budget, held in the plugin's pool and given back when
/// it is destroyed. Move only; see abi/memory.h for what a grow and a
/// reclaimer do. A reservation must be destroyed before the plugin unloads and
/// is not thread safe.
class Reservation {
   public:
    Reservation(Reservation&& other) noexcept : e_(other.e_), r_(other.r_) {
        other.r_ = nullptr;
    }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    Reservation& operator=(Reservation&&) = delete;
    ~Reservation() { release(); }

    /// Adds `bytes`. DFTU_E_NO_MEMORY with the message on the host's ctx when
    /// the budget has no room, and the reservation is as it was.
    dstd::Result<void> try_grow(const Host& host, std::uint64_t bytes) const {
        return detail::reported(e_->try_grow(host.ctx(), r_, bytes));
    }
    /// Gives back at most what the reservation holds.
    void shrink(std::uint64_t bytes) const noexcept { e_->shrink(r_, bytes); }

    /// Sets the function the host calls when a grow needs room; `fn` frees up
    /// to `want` bytes and returns how many it freed. `ud` must outlive the
    /// reservation.
    void set_reclaimer(std::uint64_t (*fn)(void* ud, std::uint64_t want),
                       void* ud) const noexcept {
        e_->set_reclaimer(r_, fn, ud);
    }
    /// Uses `owner->reclaim(want)`, which returns the bytes freed, as the
    /// reclaimer.
    template <class T>
    void set_reclaimer(T* owner) const noexcept {
        set_reclaimer(
            [](void* ud, std::uint64_t want) noexcept -> std::uint64_t {
                return static_cast<T*>(ud)->reclaim(want);
            },
            owner);
    }

    /// Gives every byte back now; the destructor does the same.
    void release() noexcept {
        if (r_) e_->release(r_);
        r_ = nullptr;
    }

   private:
    friend class Memory;
    Reservation(const dftu_svc_memory* e, dftu_reservation* r) noexcept
        : e_(e), r_(r) {}

    const dftu_svc_memory* e_;
    dftu_reservation* r_;
};

/// Typed wrapper over the dftu.svc.memory service (see abi/memory.h). A host
/// without the service returns DFTU_E_UNSUPPORTED, or nullptr from alloc.
class Memory {
   public:
    Memory(const dftu_plugin_host* h, dftu_ctx* ctx)
        : h_(h),
          ctx_(ctx),
          e_(h && h->get_service ? static_cast<const dftu_svc_memory*>(
                                       h->get_service(h->h, DFTU_SVC_MEMORY))
                                 : nullptr) {}

    /// A block charged to the plugin's pool, or nullptr. Free it with the same
    /// size and alignment.
    void* alloc(std::uint64_t size,
                std::uint32_t align = alignof(std::max_align_t)) const {
        return e_ ? e_->alloc(h_->h, size, align) : nullptr;
    }
    void* realloc(void* p, std::uint64_t old_size, std::uint64_t new_size,
                  std::uint32_t align = alignof(std::max_align_t)) const {
        return e_ ? e_->realloc(h_->h, p, old_size, new_size, align) : nullptr;
    }
    void free(void* p, std::uint64_t size,
              std::uint32_t align = alignof(std::max_align_t)) const {
        if (e_) e_->free(h_->h, p, size, align);
    }

    /// An empty reservation; `label` names it in the host's messages.
    dstd::Result<Reservation> reserve(const char* label = nullptr) const {
        if (!e_)
            return make_error(DFTU_E_UNSUPPORTED,
                              "host lacks the memory service");
        dftu_reservation* r = nullptr;
        DFTRACER_UTILS_TRY_VOID(
            detail::reported(e_->reserve(ctx_, h_->h, &r, label)));
        return Reservation(e_, r);
    }

   private:
    const dftu_plugin_host* h_;
    dftu_ctx* ctx_;
    const dftu_svc_memory* e_;
};

inline Memory Host::memory() const { return Memory{h_, ctx_}; }

}  // namespace dftracer::utils::plugins

#endif /* DFTRACER_UTILS_PLUGINS_PLUGIN_MEMORY_H */
