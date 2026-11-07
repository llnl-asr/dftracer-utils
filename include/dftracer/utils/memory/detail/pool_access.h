// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_MEMORY_DETAIL_POOL_ACCESS_H
#define DFTRACER_UTILS_MEMORY_DETAIL_POOL_ACCESS_H

#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/memory/pool.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::memory::detail {

/// The engine's door to the members of Pool that users do not see.
struct PoolAccess {
    static Reservation reserve(Pool& p, std::size_t bytes) noexcept {
        return p.reserve(bytes);
    }
    static Status grow(Reservation& r, std::size_t bytes) noexcept {
        return r.grow(bytes);
    }
    static Status try_grow(Reservation& r, std::size_t bytes) noexcept {
        return r.try_grow(bytes);
    }
    static std::size_t take_from(Reservation& r, Reservation& other,
                                 std::size_t bytes) noexcept {
        return r.take_from(other, bytes);
    }
    static void raise_limit(Pool& p, std::size_t bytes) noexcept {
        p.raise_limit(bytes);
    }
    static ReclaimSlot attach(Pool& p, Reclaimer& reclaimer) {
        return p.attach(reclaimer);
    }
    static std::uint16_t id(const Pool& p) noexcept { return p.id(); }
    static std::uint32_t generation(const Pool& p) noexcept {
        return p.generation();
    }
    static std::uint16_t parent(const Pool& p) noexcept { return p.parent(); }
    static std::size_t adopted(const Pool& p) { return p.adopted(); }
    static std::size_t exported(const Pool& p) noexcept { return p.exported(); }
    static void add_exported(Pool& p, std::size_t bytes) noexcept {
        p.add_exported(bytes);
    }
    static void sub_exported(Pool& p, std::size_t bytes) noexcept {
        p.sub_exported(bytes);
    }
};

}  // namespace dftracer::utils::memory::detail

#endif  // DFTRACER_UTILS_MEMORY_DETAIL_POOL_ACCESS_H
