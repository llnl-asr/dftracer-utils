#ifndef DFTRACER_UTILS_CORE_UTILS_TIMER_H
#define DFTRACER_UTILS_CORE_UTILS_TIMER_H

#include <chrono>
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
    void increment(const std::string& key, std::uint64_t by = 1);
    void set_counter(const std::string& key, std::uint64_t value);
    const std::unordered_map<std::string, std::uint64_t>& counters() const;
    void print_stages(const std::string& indent = "  ") const;

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
    std::unordered_map<std::string, std::uint64_t> counters_;
};

/// Self-contained scoped timer. Each instance captures its own start
/// timestamp and writes elapsed nanoseconds to `timer->set_counter(key)`
/// on destruction, so ScopedTimers can be nested freely.
class ScopedTimer {
   public:
    ScopedTimer(Timer& timer, std::string key)
        : timer_(&timer), key_(std::move(key)), start_(Clock::now()) {}

    ScopedTimer(Timer* timer, std::string key)
        : timer_(timer), key_(std::move(key)), start_(Clock::now()) {}

    ~ScopedTimer() {
        if (!timer_) return;
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      Clock::now() - start_)
                      .count();
        timer_->set_counter(key_, static_cast<std::uint64_t>(ns));
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

   private:
    using Clock = std::chrono::high_resolution_clock;
    Timer* timer_;
    std::string key_;
    Clock::time_point start_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_UTILS_TIMER_H
