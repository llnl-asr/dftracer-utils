// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <utility>

namespace dftracer::utils::common {

Timer::Timer(bool autostart, bool verbose)
    : verbose_(verbose), running_(false) {
    if (autostart) {
        start();
    }
}

Timer::Timer(dstd::StringView name, bool autostart, bool verbose)
    : verbose_(verbose), running_(false), name_(name) {
    if (autostart) {
        start();
    }
}

Timer::Timer(const char* name, bool autostart, bool verbose)
    : Timer(dstd::StringView(name), autostart, verbose) {}

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

void Timer::increment(dstd::StringView key, std::uint64_t by) {
    counters_[key] += by;
}

void Timer::set_counter(dstd::StringView key, std::uint64_t value) {
    counters_[key] = value;
}

const dstd::HashMap<dstd::String, std::uint64_t>& Timer::counters() const {
    return counters_;
}

void Timer::print_stages(dstd::StringView prefix) const {
    if (counters_.empty()) return;

    dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>> sorted(
        counters_.begin(), counters_.end());
    std::sort(sorted.begin(), sorted.end());

    std::uint64_t total_ns = 0;
    for (const auto& [_, ns] : sorted) total_ns += ns;

    if (!name_.empty()) {
        std::printf("%.*s%s (%.2f ms)\n", static_cast<int>(prefix.size()),
                    prefix.data(), name_.c_str(),
                    static_cast<double>(total_ns) / 1e6);
    }
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const auto& [key, ns] = sorted[i];
        bool last = (i + 1 == sorted.size());
        double ms = static_cast<double>(ns) / 1e6;
        double pct = total_ns > 0 ? 100.0 * static_cast<double>(ns) /
                                        static_cast<double>(total_ns)
                                  : 0.0;
        std::printf("%.*s%s %-28s %8.2f ms  (%5.1f%%)\n",
                    static_cast<int>(prefix.size()), prefix.data(),
                    last ? "\\-- " : "|-- ", key.c_str(), ms, pct);
    }
}

}  // namespace dftracer::utils::common
