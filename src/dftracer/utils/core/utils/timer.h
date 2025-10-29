#ifndef DFTRACER_UTILS_CORE_UTILS_TIMER_H
#define DFTRACER_UTILS_CORE_UTILS_TIMER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace dftracer::utils {

class Timer {
   public:
    Timer(bool autostart = false, bool verbose = false);
    Timer(const std::string& name, bool autostart = false,
          bool verbose = false);
    ~Timer();
    void start();
    void stop();
    std::int64_t elapsed() const;

    inline const std::string& name() const { return name_; }
    inline bool is_running() const { return running_; }
    inline bool is_stopped() const { return !running_; }
    inline bool is_verbose() const { return verbose_; }

    inline Timer& reset() {
        start_time = Clock::now();
        end_time = Clock::time_point();
        running_ = false;
        return *this;
    }

    inline Timer& set_name(const std::string& name) {
        name_ = name;
        return *this;
    }

    inline Timer& set_verbose(bool verbose) {
        verbose_ = verbose;
        return *this;
    }

   private:
    bool verbose_ = false;
    bool running_ = false;
    std::string name_;
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point start_time;
    Clock::time_point end_time;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_UTILS_TIMER_H
