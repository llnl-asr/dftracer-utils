// SPDX-License-Identifier: MIT
// The @NAME@ fold plugin, made by `dftracer_plugin new`.
#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>
#include <cstdio>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

/// One slice of the fold. Every thread folds its own slice of the events and
/// the host merges the slices, so merge combines two of them into one.
struct EventCount {
    std::uint64_t events = 0;

    explicit EventCount(const Config&) {}

    /// Called once per batch of events, on the thread that scanned them.
    void step(const Batch& batch, Host) {
        for (const Event& event : batch) {
            if (event.has_dur()) ++events;
        }
    }

    void merge(EventCount& other) { events += other.events; }

    /// Called once, after the last merge, on the thread that owns the whole.
    void finalize(Host) {
        std::fprintf(stderr, "@NAME@: %llu events with a duration\n",
                     static_cast<unsigned long long>(events));
    }
};

DFTU_PLUGIN("@NAME@", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.fold<EventCount>();
                return {};
            })
