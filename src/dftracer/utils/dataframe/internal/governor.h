// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_GOVERNOR_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_GOVERNOR_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/memory/detail/pool_access.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace dftracer::utils::dataframe {

// Governor state of a stateful cursor. It holds a Scratch pool below the
// query run's pool, a Reservation following the resident state and a
// ReclaimSlot for requests. The cursor spills on its own thread when check()
// returns true. Inactive (every call a no-op) when the pool table is full.
class Governor final : public memory::detail::Reclaimer {
   public:
    explicit Governor(dstd::StringView name) {
        pool_ = memory::detail::current_pool().child(name);
        if (!pool_) return;
        reservation_ = memory::detail::PoolAccess::reserve(*pool_, 0);
        slot_ = memory::detail::PoolAccess::attach(*pool_, *this);
    }
    Governor(const Governor&) = delete;
    Governor& operator=(const Governor&) = delete;

    // `resident` is what the cursor holds and a spill would free. Returns the
    // bytes to free now, zero for none. That is what another holder asked for,
    // or all of `resident` when the reservation could not grow.
    std::uint64_t check(std::uint64_t resident) noexcept {
        return check(resident, resident);
    }

    // As check(resident), when a spill would free only `spillable` of it. With
    // zero, the state is reserved but never offered, and the call returns
    // nonzero only when the grow is refused.
    std::uint64_t check(std::uint64_t resident,
                        std::uint64_t spillable) noexcept {
        if (!slot_) return 0;
        slot_.set_reclaimable(spillable);
        const std::uint64_t asked =
            std::min<std::uint64_t>(slot_.take_request(), spillable);
        const std::uint64_t held = reservation_.bytes();
        // A failed grow posts requests and trims the heap, so after one
        // denial the next try waits until a spill has freed some of it.
        if (resident > held && denied_at_ == 0) {
            // Grow a quarter ahead, so a reservation that tracks a growing
            // state reserves once per quarter, not once per morsel.
            const std::uint64_t ahead = resident + resident / 4 - held;
            if (memory::detail::PoolAccess::grow(reservation_, ahead) !=
                    memory::Status::Ok &&
                memory::detail::PoolAccess::grow(
                    reservation_, resident - held) != memory::Status::Ok) {
                denied_at_ = resident;
                // The failed grows posted part of their shortfall to this
                // slot. The spill the caller makes now answers it.
                slot_.take_request();
                return resident;
            }
        }
        return asked;
    }

    // After a spill or a drop that leaves `resident` bytes held.
    void spilled(std::uint64_t resident) noexcept {
        if (!slot_) return;
        if (resident < denied_at_) denied_at_ = 0;
        slot_.set_reclaimable(resident);
        if (reservation_.bytes() <= resident) return;
        reservation_.release();
        reservation_ = memory::detail::PoolAccess::reserve(*pool_, resident);
        if (!reservation_)
            reservation_ = memory::detail::PoolAccess::reserve(*pool_, 0);
    }

    std::uint64_t reserved() const noexcept { return reservation_.bytes(); }
    /// The Scratch pool, for state that never leaves the cursor. Null when
    /// the pool table was full.
    memory::Pool* pool() const noexcept { return pool_.get(); }

    // Nothing calls reclaim(), as the cursor spills from check() on its own
    // thread.
    std::size_t reclaimable() const override { return 0; }
    std::size_t reclaim(std::size_t) override { return 0; }

   private:
    memory::PoolHandle pool_;
    memory::Reservation reservation_;
    memory::detail::ReclaimSlot slot_;
    // The resident size whose grow was refused, zero when none was.
    std::uint64_t denied_at_ = 0;
};

// A plugin cursor's state is what its `bytes` slot reports, and it frees it
// through its own `reclaim` slot, called at a morsel boundary on the thread
// that pulls the cursor. A plugin without both slots is not governed.
class PluginGovernor {
   public:
    explicit PluginGovernor(const ::dftu_cursor_vt* vt) {
        if (vt->bytes && vt->reclaim) gov_.emplace("plugin");
    }

    void at_morsel(const ::dftu_cursor_vt* vt, void* self) {
        if (!gov_) return;
        const std::uint64_t held = vt->bytes(self);
        const std::uint64_t want = gov_->check(held);
        if (want == 0) return;
        const std::uint64_t freed = vt->reclaim(self, want);
        gov_->spilled(held - std::min(held, freed));
    }

   private:
    dstd::Optional<Governor> gov_;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_GOVERNOR_H
