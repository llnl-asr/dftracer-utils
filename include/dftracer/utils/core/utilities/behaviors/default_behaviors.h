#ifndef DFTRACER_UTILS_UTILITIES_BEHAVIORS_DEFAULT_BEHAVIORS_H
#define DFTRACER_UTILS_UTILITIES_BEHAVIORS_DEFAULT_BEHAVIORS_H

#include <dftracer/utils/core/utilities/behaviors/behavior_factory.h>
#include <dftracer/utils/core/utilities/behaviors/caching_behavior.h>
#include <dftracer/utils/core/utilities/behaviors/monitoring_behavior.h>
#include <dftracer/utils/core/utilities/behaviors/retry_behavior.h>
#include <dftracer/utils/core/utilities/tags/tags.h>

namespace dftracer {
namespace utils {
namespace utilities {
namespace behaviors {

/**
 * @brief Register default behavior creators for standard tags.
 *
 * Registers creators for:
 * - tags::Cacheable -> CachingBehavior
 * - tags::Retryable -> RetryBehavior
 * - tags::Monitored -> MonitoringBehavior
 *
 * This function should be called once during initialization to set up
 * the standard tag-to-behavior mappings.
 *
 * @tparam I Input type
 * @tparam O Output type
 *
 * Usage:
 * @code
 * auto& factory = get_behavior_factory<int, std::string>();
 * register_default_behaviors(factory);
 * @endcode
 */
template <typename I, typename O>
void register_default_behaviors(
    [[maybe_unused]] BehaviorFactory<I, O>& factory) {
    // Register Cacheable -> CachingBehavior
    // factory.template register_behavior<tags::Cacheable>(
    //     [](const tags::Cacheable& tag) {
    //         return std::make_shared<CachingBehavior<I, O>>(
    //             tag.max_cache_size, tag.ttl, tag.use_lru);
    //     });

    // Register Retryable -> RetryBehavior
    // factory.template register_behavior<tags::Retryable>(
    //     [](const tags::Retryable& tag) {
    //         return std::make_shared<RetryBehavior<I, O>>(
    //             tag.max_retries, tag.retry_delay, tag.exponential_backoff);
    //     });

    // // Register Monitored -> MonitoringBehavior
    // factory.template register_behavior<tags::Monitored>(
    //     [](const tags::Monitored& tag) {
    //         return std::make_shared<MonitoringBehavior<I,
    //         O>>(tag.log_callback,
    //                                                           tag.utility_name);
    //     });
}

/**
 * @brief Get or create behavior factory with default behaviors registered.
 *
 * This is a convenience function that gets the singleton factory and
 * ensures default behaviors are registered exactly once.
 *
 * @tparam I Input type
 * @tparam O Output type
 * @return Reference to behavior factory with defaults registered
 *
 * Usage:
 * @code
 * auto& factory = get_default_behavior_factory<int, std::string>();
 * auto behavior = factory.create(cacheable_tag);
 * @endcode
 */
template <typename I, typename O>
BehaviorFactory<I, O>& get_default_behavior_factory() {
    static bool initialized = false;
    auto& factory = get_behavior_factory<I, O>();

    if (!initialized) {
        register_default_behaviors(factory);
        initialized = true;
    }

    return factory;
}

}  // namespace behaviors
}  // namespace utilities
}  // namespace utils
}  // namespace dftracer

#endif  // DFTRACER_UTILS_UTILITIES_BEHAVIORS_DEFAULT_BEHAVIORS_H
