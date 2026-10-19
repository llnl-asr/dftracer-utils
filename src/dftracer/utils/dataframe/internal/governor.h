#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_GOVERNOR_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_GOVERNOR_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/memory/manager.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dftracer::utils::dataframe {

// A stateful cursor's place in the memory governor: a Scratch pool below the
// pool that is current when the cursor is made (the query run's pool), one
// Reservation that follows the cursor's resident state and a ReclaimSlot that
// other reservations post requests to. The cursor calls check() at each
// morsel boundary and spills on its own thread when it returns true; the
// manager never calls the cursor. Inactive (every call a no-op) when the pool
// table is full.
class Governor final : public memory::Reclaimer {
   public:
    explicit Governor(std::string_view name) {
        pool_ = memory::current_pool().child(name);
        if (!pool_) return;
        reservation_ = pool_->reserve(0);
        slot_ = pool_->attach(*this);
    }
    Governor(const Governor&) = delete;
    Governor& operator=(const Governor&) = delete;

    // `resident` is what the cursor holds now and what a spill would free.
    // Returns the bytes the cursor should free now, zero for none: what a
    // holder over the budget asked for, or all of `resident` when the
    // reservation could not grow to it.
    std::uint64_t check(std::uint64_t resident) noexcept {
        return check(resident, resident);
    }

    // As check(resident), when a spill would free only `spillable` of it: the
    // slot reports that much, and a request is answered from it alone. With
    // zero, state that cannot leave memory is reserved, never offered, and
    // the call returns nonzero only when the grow is refused.
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
            if (reservation_.grow(ahead) != memory::Status::Ok &&
                reservation_.grow(resident - held) != memory::Status::Ok) {
                denied_at_ = resident;
                // The failed grows posted part of their shortfall to this
                // slot; the spill the caller makes now answers it.
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
        reservation_ = pool_->reserve(resident);
        if (!reservation_) reservation_ = pool_->reserve(0);
    }

    std::uint64_t reserved() const noexcept { return reservation_.bytes(); }
    /// The Scratch pool, for state that never leaves the cursor; null when
    /// the pool table was full.
    memory::Pool* pool() const noexcept { return pool_.get(); }

    // The slot starts at zero; check() reports what the cursor holds. The
    // cursor spills from check() on its own thread, so nothing calls
    // reclaim().
    std::size_t reclaimable() const override { return 0; }
    std::size_t reclaim(std::size_t) override { return 0; }

   private:
    memory::PoolHandle pool_;
    memory::Reservation reservation_;
    memory::ReclaimSlot slot_;
    // The resident size whose grow was refused; zero when none was.
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
    std::optional<Governor> gov_;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_GOVERNOR_H
