#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/utils/timer.h>

#include <cinttypes>
#include <cstdio>

namespace dftracer::utils {

Timer::Timer(bool autostart, bool verbose)
    : verbose_(verbose), running_(false) {
    if (autostart) {
        start();
    }
}

Timer::Timer(const std::string& name, bool autostart, bool verbose)
    : verbose_(verbose), running_(false), name_(name) {
    if (autostart) {
        start();
    }
}

Timer::~Timer() {
    stop();
    if (verbose_) {
        if (name_.empty()) {
            std::printf("Elapsed time: %" PRId64 " ns\n", elapsed());
        } else {
            std::printf("[%s] Elapsed time: %" PRId64 " ns\n", name_.c_str(),
                        elapsed());
        }
    }
}

void Timer::start() {
    start_time = Clock::now();
    running_ = true;
}

void Timer::stop() {
    if (running_) {
        end_time = Clock::now();
        running_ = false;
    }
}

std::int64_t Timer::elapsed() const {
    if (running_) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   Clock::now() - start_time)
            .count();
    } else {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end_time -
                                                                    start_time)
            .count();
    }
}

}  // namespace dftracer::utils
