// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_TIMER_H
#define DFTRACER_UTILS_COMMON_TIMER_H

#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>

#include <chrono>
#include <cstdint>

namespace dftracer::utils::common {

class Timer {
   public:
    Timer(bool autostart = false, bool verbose = false);
    Timer(dstd::StringView name, bool autostart = false, bool verbose = false);
    Timer(const char* name, bool autostart = false, bool verbose = false);
    ~Timer();
    void start();
    void stop();
    std::int64_t elapsed() const;
    void increment(dstd::StringView key, std::uint64_t by = 1);
    void set_counter(dstd::StringView key, std::uint64_t value);
    /// The counters in no particular order.
    const dstd::HashMap<dstd::String, std::uint64_t>& counters() const;
    void print_stages(dstd::StringView indent = "  ") const;

    inline const dstd::String& name() const { return name_; }
    inline bool is_running() const { return running_; }
    inline bool is_stopped() const { return !running_; }
    inline bool is_verbose() const { return verbose_; }

    inline Timer& reset() {
        start_time_ = dstd::steady_now();
        end_time_ = Clock::time_point();
        running_ = false;
        return *this;
    }

    inline Timer& set_name(dstd::StringView name) {
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
    dstd::String name_;
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_time_;
    Clock::time_point end_time_;
    dstd::HashMap<dstd::String, std::uint64_t> counters_;
};

/// Self-contained scoped timer. Each instance captures its own start
/// timestamp and writes elapsed nanoseconds to `timer->set_counter(key)`
/// on destruction, so ScopedTimers can be nested freely.
class ScopedTimer {
   public:
    ScopedTimer(Timer& timer, dstd::String key)
        : timer_(&timer), key_(std::move(key)), start_(dstd::steady_now()) {}

    ScopedTimer(Timer* timer, dstd::String key)
        : timer_(timer), key_(std::move(key)), start_(dstd::steady_now()) {}

    ~ScopedTimer() {
        if (!timer_) return;
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      dstd::steady_now() - start_)
                      .count();
        timer_->set_counter(key_, static_cast<std::uint64_t>(ns));
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

   private:
    using Clock = std::chrono::steady_clock;
    Timer* timer_;
    dstd::String key_;
    Clock::time_point start_;
};

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_TIMER_H
