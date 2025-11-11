#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_HASHER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_HASHER_UTILITY_H

#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/utilities/composites/dft/event_collector_utility.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace dftracer::utils::utilities::composites::dft {

/**
 * @brief Input for event hashing.
 */
struct EventHashInput {
    std::vector<EventId> events;

    static EventHashInput from_events(std::vector<EventId> event_list) {
        EventHashInput input;
        input.events = std::move(event_list);
        return input;
    }
};

/**
 * @brief Output: 64-bit hash of events.
 */
using EventHashOutput = std::uint64_t;

/**
 * @brief Workflow for computing a hash from a collection of EventIds.
 *
 * Uses XXH3 to hash the id, pid, tid fields of each event in order.
 * Events should be sorted before hashing for consistent results.
 */
class EventHasher : public utilities::Utility<EventHashInput, EventHashOutput> {
   public:
    EventHashOutput process(const EventHashInput& input) override;
};

/**
 * Input for incremental event hashing.
 */
struct IncrementalEventHashInput {
    std::vector<EventId> events;

    static IncrementalEventHashInput from_events(std::vector<EventId> evts) {
        return IncrementalEventHashInput{std::move(evts)};
    }
};

/**
 * Incremental event hasher with order-independent (additive) hashing.
 */
class IncrementalEventHasher
    : public utilities::Utility<IncrementalEventHashInput, std::size_t> {
   private:
    std::size_t hash_ = 0;

   public:
    IncrementalEventHasher() = default;

    void update(const EventId& event) {
        std::size_t event_hash = std::hash<std::uint64_t>{}(event.id);
        event_hash ^= std::hash<std::int64_t>{}(event.pid) + 0x9e3779b9 +
                      (event_hash << 6) + (event_hash >> 2);
        event_hash ^= std::hash<std::int64_t>{}(event.tid) + 0x9e3779b9 +
                      (event_hash << 6) + (event_hash >> 2);
        hash_ += event_hash;
    }

    void update(const std::vector<EventId>& events) {
        for (const auto& event : events) {
            update(event);
        }
    }

    std::size_t process(const IncrementalEventHashInput& input) override {
        update(input.events);
        return hash_;
    }

    std::size_t get_hash() const { return hash_; }
    void reset() { hash_ = 0; }
    void merge(const IncrementalEventHasher& other) { hash_ += other.hash_; }
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_HASHER_UTILITY_H
