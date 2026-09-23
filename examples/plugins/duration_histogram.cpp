// Example dftracer-utils plugin: a log2 histogram of event durations. Each
// slice accumulates a local histogram over the events it folds, the host merges
// the per-worker slices, and finalize prints one line per non-empty bucket.
// This is the self-contained shape - a plugin that computes and emits its own
// summary; for the host-owned mergeable shape see the process_* accumulator
// examples.
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include \
//            -o duration_histogram.so duration_histogram.cpp
// Run:   dftracer_run -d ./traces --plugin ./duration_histogram.so

#include <dftracer/utils/plugins/plugin.h>

#include <array>
#include <cstdint>
#include <cstdio>

using namespace dftracer::utils::plugins;

struct DurationHistogram {
    std::array<std::uint64_t, 64> bins{};

    explicit DurationHistogram(const Config&) {}

    void step(const Batch& b, Host) {
        for (const Event& e : b) {
            if (!e.has_dur()) continue;
            int bucket = e.dur() ? 63 - __builtin_clzll(e.dur()) : 0;
            ++bins[bucket];
        }
    }

    void merge(DurationHistogram& other) {
        for (std::size_t i = 0; i < bins.size(); ++i) bins[i] += other.bins[i];
    }

    void finalize(Host) {
        for (int i = 0; i < static_cast<int>(bins.size()); ++i)
            if (bins[i])
                std::fprintf(stderr, "2^%d us: %llu\n", i,
                             static_cast<unsigned long long>(bins[i]));
    }
};

extern "C" DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(
    dftu_plugin_host* h, const dftu_value* config) {
    return plugin(h, config).fold<DurationHistogram>().build();
}
