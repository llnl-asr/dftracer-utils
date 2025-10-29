#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/dft/event_hasher.h>
#include <dftracer/utils/utilities/hash/hasher.h>

namespace dftracer::utils::utilities::composites::dft {

EventHashOutput EventHasher::process(const EventHashInput& input) {
    hash::HasherUtility hasher;

    for (const auto& event : input.events) {
        hasher.process(event.id, event.pid, event.tid);
    }

    return hasher.get_hash().value;
}

}  // namespace dftracer::utils::utilities::composites::dft
