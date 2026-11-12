// SPDX-License-Identifier: MIT
// Benchmark: ns per log line. An enabled WARN line with 0, 2 and 5 arguments
// goes to /dev/null; a DEBUG and a TRACE line below the runtime level cost one
// predicted branch. Median of the rounds.
//
//   log_bench            (table)
//   log_bench --smoke    (few iterations)

#include <dftracer/utils/common/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace dftracer::utils;

#define F2 "open {} bytes={}"
#define F5 "file {} idx={} size={} ratio={:.2f} off={}"

namespace {

using Clock = std::chrono::steady_clock;

template <class F>
double median_ns(std::size_t iterations, std::size_t rounds, F&& fn) {
    double samples[15];
    for (std::size_t r = 0; r < rounds; ++r) {
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < iterations; ++i) fn(i);
        const auto t1 = Clock::now();
        samples[r] = std::chrono::duration<double, std::nano>(t1 - t0).count() /
                     static_cast<double>(iterations);
    }
    std::sort(samples, samples + rounds);
    return samples[rounds / 2];
}

}  // namespace

int main(int argc, char** argv) {
    const bool smoke = argc > 1 && std::strcmp(argv[1], "--smoke") == 0;
    const std::size_t iterations = smoke ? 2000 : 200000;
    const std::size_t rounds = smoke ? 3 : 11;

    std::FILE* sink = std::fopen("/dev/null", "w");
    if (sink == nullptr) return 1;
    common::logger::init({.level = common::logger::Level::Warn,
                          .color = common::logger::ColorMode::Never,
                          .sink = sink});

    volatile int vi = 7;
    volatile std::size_t vz = 123456;
    volatile double vd = 0.8125;
    volatile std::uint64_t vo = 987654321;
    const char* name = "/p/lustre/trace-0001.pfw.gz";

    const double w0 = median_ns(iterations, rounds, [&](std::size_t) {
        DFTRACER_UTILS_LOG_WARN("plain warning line");
    });
    const double w2 = median_ns(iterations, rounds, [&](std::size_t) {
        DFTRACER_UTILS_LOG_WARN(F2, static_cast<int>(vi),
                                static_cast<std::size_t>(vz));
    });
    const double w5 = median_ns(iterations, rounds, [&](std::size_t) {
        DFTRACER_UTILS_LOG_WARN(
            F5, name, static_cast<int>(vi), static_cast<std::size_t>(vz),
            static_cast<double>(vd), static_cast<std::uint64_t>(vo));
    });
    const double d2 = median_ns(iterations * 20, rounds, [&](std::size_t) {
        DFTRACER_UTILS_LOG_DEBUG(F2, static_cast<int>(vi),
                                 static_cast<std::size_t>(vz));
    });
    const double t2 = median_ns(iterations * 20, rounds, [&](std::size_t) {
        DFTRACER_UTILS_LOG_TRACE(F2, static_cast<int>(vi),
                                 static_cast<std::size_t>(vz));
    });

    std::printf("%-28s %10s\n", "case", "ns/line");
    std::printf("%-28s %10.1f\n", "warn, 0 args", w0);
    std::printf("%-28s %10.1f\n", "warn, 2 args", w2);
    std::printf("%-28s %10.1f\n", "warn, 5 args", w5);
    std::printf("%-28s %10.2f\n", "debug, below level", d2);
    std::printf("%-28s %10.2f\n", "trace, below level", t2);
    return 0;
}
