#ifndef DFTRACER_UTILS_UTILITIES_BEHAVIORS_MONITORING_BEHAVIOR_H
#define DFTRACER_UTILS_UTILITIES_BEHAVIORS_MONITORING_BEHAVIOR_H

#include <dftracer/utils/core/utilities/behaviors/behavior.h>

#include <chrono>
#include <functional>
#include <optional>
#include <sstream>
#include <string>

namespace dftracer {
namespace utils {
namespace utilities {
namespace behaviors {

/**
 * @brief Monitoring behavior for logging and metrics.
 *
 * Provides hooks for:
 * - Logging before/after processing
 * - Timing execution duration
 * - Error tracking and logging
 * - Custom callbacks for metrics collection
 *
 * The behavior uses a callback function to report events, allowing
 * flexible integration with different logging/metrics systems.
 *
 * @tparam I Input type
 * @tparam O Output type
 */
template <typename I, typename O>
class MonitoringBehavior : public UtilityBehavior<I, O> {
   private:
    std::function<void(const std::string&)> log_callback_;
    std::string utility_name_;
    std::chrono::steady_clock::time_point start_time_;
    bool timing_active_;

    /**
     * @brief Log a message via callback.
     */
    void log(const std::string& message) const {
        if (log_callback_) {
            log_callback_(message);
        }
    }

   public:
    /**
     * @brief Construct monitoring behavior.
     *
     * @param log_callback Callback function for log messages
     * @param utility_name Optional name of the utility being monitored
     */
    explicit MonitoringBehavior(
        std::function<void(const std::string&)> log_callback,
        std::string utility_name = "Utility")
        : log_callback_(log_callback),
          utility_name_(std::move(utility_name)),
          timing_active_(false) {}

    /**
     * @brief Log before processing and start timing.
     *
     * @param input The input to be processed
     */
    void before_process([[maybe_unused]] const I& input) override {
        start_time_ = std::chrono::steady_clock::now();
        timing_active_ = true;

        std::ostringstream oss;
        oss << "[Monitor] " << utility_name_ << " starting processing";
        log(oss.str());
    }

    /**
     * @brief Log after processing with execution time.
     *
     * @param input The input that was processed
     * @param result The processing result
     * @return Unmodified result
     */
    O after_process([[maybe_unused]] const I& input, O result) override {
        if (timing_active_) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    end_time - start_time_);

            std::ostringstream oss;
            oss << "[Monitor] " << utility_name_ << " completed in "
                << duration.count() << " microseconds";
            log(oss.str());

            timing_active_ = false;
        }

        return result;
    }

    /**
     * @brief Log error occurrence.
     *
     * Logs the error but doesn't influence retry or recovery behavior.
     * Returns nullopt to pass through to next behavior in chain.
     *
     * @param input The input that caused the error
     * @param e The exception that occurred
     * @param attempt Current retry attempt
     * @return nullopt (pass through to next behavior)
     */
    std::variant<BehaviorErrorResult, std::optional<O>> on_error(
        [[maybe_unused]] const I& input, const std::exception& e,
        std::size_t attempt) override {
        std::ostringstream oss;
        oss << "[Monitor] " << utility_name_ << " error on attempt "
            << (attempt + 1) << ": " << e.what();
        log(oss.str());

        timing_active_ = false;

        // Pass through - let other behaviors decide
        return std::optional<O>(std::nullopt);
    }

    /**
     * @brief Set new log callback.
     *
     * @param callback New callback function
     */
    void set_log_callback(std::function<void(const std::string&)> callback) {
        log_callback_ = callback;
    }

    /**
     * @brief Get current log callback.
     */
    const std::function<void(const std::string&)>& get_log_callback() const {
        return log_callback_;
    }
};

}  // namespace behaviors
}  // namespace utilities
}  // namespace utils
}  // namespace dftracer

#endif  // DFTRACER_UTILS_UTILITIES_BEHAVIORS_MONITORING_BEHAVIOR_H
