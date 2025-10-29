#ifndef DFTRACER_UTILS_CORE_UTILITIES_TAGS_MONITORED_H
#define DFTRACER_UTILS_CORE_UTILITIES_TAGS_MONITORED_H

#include <functional>
#include <string>

namespace dftracer::utils::utilities::tags {

/**
 * @brief Configuration tag indicating that utility execution should be
 * monitored/logged.
 *
 * This tag enables automatic monitoring behavior when used with the behavior
 * system. Provides a callback for logging and metrics collection.
 *
 * Usage:
 * @code
 * set_tag(Monitored()
 *     .with_log_callback([](const std::string& msg) { std::cout << msg << "\n";
 * }) .with_utility_name("MyUtility"));
 * @endcode
 */
struct Monitored {
    /** Callback function for logging/monitoring events */
    std::function<void(const std::string&)> log_callback;

    /** Optional name of the utility being monitored (for logging) */
    std::string utility_name = "Utility";

    Monitored& with_log_callback(
        std::function<void(const std::string&)> callback) {
        log_callback = std::move(callback);
        return *this;
    }

    Monitored& with_utility_name(std::string name) {
        utility_name = std::move(name);
        return *this;
    }
};

}  // namespace dftracer::utils::utilities::tags

#endif  // DFTRACER_UTILS_CORE_UTILITIES_TAGS_MONITORED_H
