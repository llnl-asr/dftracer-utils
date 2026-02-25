#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_HASHER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_HASHER_UTILITY_H

#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/utilities/composites/dft/event_collector_utility.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

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
 * Uses HasherUtility to hash the id, pid, tid fields of each event.
 * Events should be sorted before hashing for consistent results.
 */
class EventHasher : public utilities::Utility<EventHashInput, EventHashOutput> {
   public:
    coro::CoroTask<EventHashOutput> process(
        const EventHashInput& input) override;
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
        utilities::hash::HasherUtility hasher;
        hasher.update(event.id);
        hasher.update(event.pid);
        hasher.update(event.tid);
        hash_ += hasher.get_hash().value;
    }

    void update(const std::vector<EventId>& events) {
        for (const auto& event : events) {
            update(event);
        }
    }

    coro::CoroTask<std::size_t> process(
        const IncrementalEventHashInput& input) override {
        update(input.events);
        co_return hash_;
    }

    std::size_t get_hash() const { return hash_; }
    void reset() { hash_ = 0; }
    void merge(const IncrementalEventHasher& other) { hash_ += other.hash_; }
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_HASHER_UTILITY_H
