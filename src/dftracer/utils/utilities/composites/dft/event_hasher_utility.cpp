// #include <algorithm>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/dft/event_hasher_utility.h>
#include <dftracer/utils/utilities/hash/hash.h>

namespace dftracer::utils::utilities::composites::dft {

EventHashOutput EventHasher::process(const EventHashInput& input) {
    hash::HasherUtility hasher;

    // Sort events by id, pid, tid for consistent hashing
    // std::vector<EventId> sorted_events = input.events;
    // std::sort(sorted_events.begin(), sorted_events.end(),
    //           [](const EventId& a, const EventId& b) {
    //               if (a.id != b.id) return a.id < b.id;
    //               if (a.pid != b.pid) return a.pid < b.pid;
    //               return a.tid < b.tid;
    //           });

    for (const auto& event : input.events) {
        hasher.process(event.id, event.pid, event.tid);
    }

    return hasher.get_hash().value;
}

}  // namespace dftracer::utils::utilities::composites::dft
