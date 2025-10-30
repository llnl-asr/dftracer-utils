#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_HASHER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_HASHER_UTILITY_H

#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/utilities/composites/dft/event_collector_utility.h>

#include <cstdint>
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

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_HASHER_UTILITY_H
