#ifndef DFTRACER_UTILS_DSTD_INTERN_CACHE_H
#define DFTRACER_UTILS_DSTD_INTERN_CACHE_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/dstd/string_intern.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

namespace dftracer::utils::dstd {

/// A thread-local front for one StringIntern that answers a short string it
/// has seen before without touching the shared table. For a loop that interns
/// the same few keys and values for every record.
///
/// Not thread safe, and bound to the table it was made for: ids it returns
/// are that table's. A string longer than MAX_BYTES goes to the table.
class InternCache {
   public:
    static constexpr std::size_t MAX_BYTES = 24;
    static constexpr std::size_t SLOTS = 256;

    explicit InternCache(StringIntern& intern) : intern_(&intern) {}

    std::uint32_t get_or_insert(std::string_view sv) {
        if (sv.size() > MAX_BYTES) return intern_->get_or_insert(sv);
        if (!slots_) slots_ = std::make_unique<std::array<Slot, SLOTS>>();
        Words key{};
        std::memcpy(key.words, sv.data(), sv.size());
        const std::uint64_t h =
            ankerl::unordered_dense::hash<std::string_view>{}(sv);
        Slot& slot = (*slots_)[h & (SLOTS - 1)];
        if (slot.size == sv.size() && slot.key.words[0] == key.words[0] &&
            slot.key.words[1] == key.words[1] &&
            slot.key.words[2] == key.words[2])
            return slot.id;
        const std::uint32_t id = intern_->get_or_insert(sv);
        slot.key = key;
        slot.size = static_cast<std::uint32_t>(sv.size());
        slot.id = id;
        return id;
    }

   private:
    struct Words {
        std::uint64_t words[MAX_BYTES / 8];
    };
    struct Slot {
        Words key{};
        std::uint32_t size = UINT32_MAX;
        std::uint32_t id = StringIntern::NO_ID;
    };

    StringIntern* intern_;
    std::unique_ptr<std::array<Slot, SLOTS>> slots_;
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_INTERN_CACHE_H
