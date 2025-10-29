#ifndef DFTRACER_UTILS_CORE_UTILITIES_TAGS_RETRYABLE_H
#define DFTRACER_UTILS_CORE_UTILITIES_TAGS_RETRYABLE_H

#include <chrono>
#include <cstddef>

namespace dftracer::utils::utilities::tags {

/**
 * @brief Configuration tag indicating that a utility can be retried on failure.
 *
 * This tag enables automatic retry behavior when used with the behavior system.
 * Contains configuration parameters for retry logic.
 *
 * Usage:
 * @code
 * set_tag(Retryable()
 *     .with_max_retries(5)
 *     .with_retry_delay(std::chrono::milliseconds(200))
 *     .with_exponential_backoff(false));
 * @endcode
 */
struct Retryable {
    /** Maximum number of retry attempts before giving up */
    std::size_t max_retries = 3;

    /** Initial delay between retry attempts */
    std::chrono::milliseconds retry_delay{100};

    /** Whether to use exponential backoff (delay *= 2^attempt) */
    bool exponential_backoff = true;

    Retryable& with_max_retries(std::size_t retries) {
        max_retries = retries;
        return *this;
    }

    Retryable& with_retry_delay(std::chrono::milliseconds delay) {
        retry_delay = delay;
        return *this;
    }

    Retryable& with_exponential_backoff(bool enabled) {
        exponential_backoff = enabled;
        return *this;
    }
};

}  // namespace dftracer::utils::utilities::tags

#endif  // DFTRACER_UTILS_CORE_UTILITIES_TAGS_RETRYABLE_H
