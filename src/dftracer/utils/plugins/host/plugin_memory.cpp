// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/memory/detail/heap.h>
#include <dftracer/utils/memory/detail/pool_access.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace dftracer::utils::plugins::host {

namespace {

constexpr std::uint32_t MAX_ALIGN = 4096;

bool valid_align(std::uint32_t align) {
    return align != 0 && align <= MAX_ALIGN && (align & (align - 1)) == 0;
}

}  // namespace

PluginMemory::PluginMemory(dstd::StringView plugin_name)
    : name_(plugin_name.empty() ? dstd::StringView("plugin") : plugin_name) {
    pool_handle_ = memory::root_pool().child(name_);
    pool_ = pool_handle_ ? pool_handle_.get()
                         : &memory::detail::Manager::get().general();
}

PluginMemory::~PluginMemory() { close(); }

void* PluginMemory::alloc(std::uint64_t size, std::uint32_t align) noexcept {
    if (!valid_align(align)) return nullptr;
    return memory::detail::try_allocate_at(static_cast<std::size_t>(size),
                                           align, *pool_, nullptr);
}

void* PluginMemory::realloc(void* p, std::uint64_t old_size,
                            std::uint64_t new_size,
                            std::uint32_t align) noexcept {
    if (p == nullptr) return alloc(new_size, align);
    void* moved = alloc(new_size, align);
    if (moved == nullptr) return nullptr;
    std::memcpy(moved, p,
                static_cast<std::size_t>(std::min(old_size, new_size)));
    memory::detail::deallocate(p, static_cast<std::size_t>(old_size));
    return moved;
}

void PluginMemory::free(void* p, std::uint64_t size) noexcept {
    if (p == nullptr) return;
    memory::detail::deallocate(p, static_cast<std::size_t>(size));
}

PluginReservation* PluginMemory::reserve(dstd::StringView label,
                                         std::uint64_t cap,
                                         const void* tag) noexcept {
    auto made = dstd::make_shared<PluginReservation>();
    made->owner = this;
    made->label = label.empty() ? dstd::StringView("unnamed") : label;
    made->cap = cap;
    made->tag = tag;
    made->held = memory::detail::PoolAccess::reserve(*pool_, 0);
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return nullptr;
    reservations_.push_back(std::move(made));
    return reservations_.back().get();
}

bool PluginMemory::try_grow(PluginReservation& r, std::uint64_t bytes,
                            dstd::String& why) noexcept {
    if (bytes == 0) return true;
    auto attempt = [&]() -> bool {
        std::uint64_t held = held_total_.load(std::memory_order_relaxed);
        do {
            if (r.cap != 0 && (bytes > r.cap || held > r.cap - bytes)) {
                why = dstd::fmt<
                    "{} bytes more would pass the {} byte memory "
                    "budget with {} held">(bytes, r.cap, held);
                return false;
            }
        } while (!held_total_.compare_exchange_weak(held, held + bytes,
                                                    std::memory_order_relaxed));
        const memory::Status status =
            memory::detail::PoolAccess::try_grow(r.held, bytes);
        if (status != memory::Status::Ok) {
            held_total_.fetch_sub(bytes, std::memory_order_relaxed);
            why = dstd::fmt<
                "{} bytes more would pass the memory budget of "
                "the process with {} held">(bytes, held);
            return false;
        }
        return true;
    };
    if (attempt()) return true;
    if (r.reclaimer == nullptr || r.reclaiming) return false;
    const std::uint64_t held = held_total_.load(std::memory_order_relaxed);
    const std::uint64_t room = r.cap != 0 && r.cap > held ? r.cap - held : 0;
    const std::uint64_t want =
        r.cap != 0 && bytes > room ? bytes - room : bytes;
    r.reclaiming = true;
    const std::uint64_t freed = r.reclaimer(r.reclaimer_ud, want);
    r.reclaiming = false;
    shrink(r, freed);
    return attempt();
}

void PluginMemory::shrink(PluginReservation& r, std::uint64_t bytes) noexcept {
    const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(bytes, r.held.bytes()));
    if (take == 0) return;
    memory::Reservation given;
    memory::detail::PoolAccess::take_from(given, r.held, take);
    held_total_.fetch_sub(take, std::memory_order_relaxed);
}

void PluginMemory::release(PluginReservation* r) noexcept {
    if (r == nullptr) return;
    dstd::SharedPtr<PluginReservation> gone;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = reservations_.begin(); it != reservations_.end(); ++it) {
            if (it->get() != r) continue;
            gone = std::move(*it);
            reservations_.erase(it);
            break;
        }
    }
    if (gone) {
        gone->dead.store(true);
        held_total_.fetch_sub(gone->held.bytes(), std::memory_order_relaxed);
        gone->held.release();
    }
}

void PluginMemory::retag(const void* from, const void* to) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : reservations_)
        if (r->tag == from) r->tag = to;
}

std::uint64_t PluginMemory::held_by(const void* tag) noexcept {
    std::uint64_t total = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& r : reservations_)
        if (r->tag == tag && r->reclaimer != nullptr) total += r->held.bytes();
    return total;
}

std::uint64_t PluginMemory::reclaim(const void* tag,
                                    std::uint64_t want) noexcept {
    dstd::Vector<dstd::SharedPtr<PluginReservation>> mine;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& r : reservations_)
            if (r->tag == tag && r->reclaimer != nullptr) mine.push_back(r);
    }
    std::uint64_t freed = 0;
    for (auto& r : mine) {
        if (freed >= want) break;
        if (r->reclaiming || r->dead.load()) continue;
        r->reclaiming = true;
        const std::uint64_t got = r->reclaimer(r->reclaimer_ud, want - freed);
        r->reclaiming = false;
        if (r->dead.load()) continue;
        const std::uint64_t held = r->held.bytes();
        const std::uint64_t taken = got < held ? got : held;
        shrink(*r, taken);
        freed += taken;
    }
    return freed;
}

std::size_t PluginMemory::release_tagged(const void* tag) noexcept {
    dstd::Vector<dstd::SharedPtr<PluginReservation>> left;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = reservations_.begin(); it != reservations_.end();) {
            if ((*it)->tag != tag) {
                ++it;
                continue;
            }
            left.push_back(std::move(*it));
            it = reservations_.erase(it);
        }
    }
    for (auto& r : left) {
        DFTRACER_UTILS_LOG_ERROR(
            "[plugin:{}] released the reservation '{}' of {} bytes that the "
            "plugin did not release",
            name_, r->label, static_cast<unsigned long long>(r->held.bytes()));
        r->dead.store(true);
        held_total_.fetch_sub(r->held.bytes(), std::memory_order_relaxed);
        r->held.release();
    }
    return left.size();
}

std::size_t PluginMemory::close() noexcept {
    dstd::Vector<dstd::SharedPtr<PluginReservation>> left;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return 0;
        closed_ = true;
        left = std::move(reservations_);
    }
    for (auto& r : left) {
        DFTRACER_UTILS_LOG_ERROR(
            "[plugin:{}] released the reservation '{}' of {} bytes that the "
            "plugin did not release",
            name_, r->label, static_cast<unsigned long long>(r->held.bytes()));
        r->dead.store(true);
        held_total_.fetch_sub(r->held.bytes(), std::memory_order_relaxed);
        r->held.release();
    }
    pool_handle_.close();
    return left.size();
}

void MemoryWatch::watch(PluginMemory& memory) {
    if (slot_) return;
    slot_ = memory::detail::PoolAccess::attach(memory.pool(), holder_);
}

void MemoryWatch::poll(PluginMemory& memory, const void* tag) {
    if (!slot_) return;
    slot_.set_reclaimable(memory.held_by(tag));
    const std::size_t asked = slot_.take_request();
    if (asked == 0) return;
    auto reclaiming = slot_.begin_reclaim();
    memory.reclaim(tag, asked);
    slot_.set_reclaimable(memory.held_by(tag));
}

}  // namespace dftracer::utils::plugins::host
