// SPDX-License-Identifier: MIT
// Example dftracer-utils plugin: event times relative to the start of the whole
// input, in one scan. The start is the earliest event time of all the files,
// which a plugin cannot know from the first batch it sees. The plugin declares
// a phase result "origin" and asks the host to seed it with that value
// (PhaseSeed::TsMin), so the sealed value is there from the first batch: the
// host takes it from the index statistics, from a cache of an earlier pass, or
// from a pass of its own, and never from a stale source.
// The plugin emits one line: the first and last relative time it saw, and the
// number of events it counted.
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include -o
// two_phase_normalize.so two_phase_normalize.cpp
// Run:   dftracer_run ./traces --plugin ./two_phase_normalize.so

#include <dftracer/utils/plugins/plugin.h>

#include <algorithm>
#include <cstdint>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

constexpr const char* ORIGIN = "origin";
constexpr std::uint64_t GENERATION = 1;

struct TwoPhaseNormalize {
    std::uint64_t origin = 0;
    bool has_origin = false;
    std::uint64_t first = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t last = 0;
    std::uint64_t events = 0;

    explicit TwoPhaseNormalize(const Config&) {}

    dstd::Result<void> step(const Batch& b, Host h) {
        if (!has_origin) {
            std::uint64_t value = 0;
            DFTRACER_UTILS_TRY(
                auto size,
                h.ports().phase_result(
                    ORIGIN, GENERATION,
                    dstd::MutableByteView(reinterpret_cast<std::byte*>(&value),
                                          sizeof value)));
            if (size != sizeof value) {
                return make_error(dstd::Condition::InvalidArgument,
                                  "the seeded origin is not 8 bytes");
            }
            origin = value;
            has_origin = true;
        }
        for (const Event& e : b) {
            // An event without a clock is at ts 0, before the origin.
            if (e.ts() < origin) continue;
            const std::uint64_t relative = e.ts() - origin;
            first = std::min(first, relative);
            last = std::max(last, relative);
            ++events;
        }
        return {};
    }

    void merge(TwoPhaseNormalize& other) {
        if (!has_origin) {
            origin = other.origin;
            has_origin = other.has_origin;
        }
        first = std::min(first, other.first);
        last = std::max(last, other.last);
        events += other.events;
    }

    dstd::Result<void> finalize(Host h) {
        const dstd::String line =
            dstd::fmt<"origin={} first={} last={} events={}">(
                origin, events == 0 ? 0 : first, last, events);
        return h.emit_result("two_phase_normalize.span", line);
    }
};

}  // namespace

DFTU_PLUGIN("two_phase_normalize", "1.0",
            [](PluginBuilder& p, const Config&) -> dstd::Result<void> {
                Ports ports = p.ports();
                DFTRACER_UTILS_TRY(
                    std::uint64_t generation,
                    ports.phase_declare(ORIGIN, MergeOp::Min, PhaseType::U64));
                DFTRACER_UTILS_TRY_VOID(
                    ports.seed(ORIGIN, generation, PhaseSeed::TsMin));
                p.fold<TwoPhaseNormalize>();
                return {};
            })
