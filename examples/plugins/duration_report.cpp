// SPDX-License-Identifier: MIT
// Example dftracer-utils plugin: the log2 duration histogram of
// duration_histogram.cpp, written as gzip JSON lines through the host's fileio
// service. The output path comes from the `out` plugin argument.
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include -o duration_report.so
// duration_report.cpp
// Run:   dftracer_run ./traces --plugin ./duration_report.so --parg
// out=report.jsonl.gz

#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/plugins/plugin.h>

#include <array>
#include <cstdint>
#include <cstdio>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

struct DurationReport {
    std::array<std::uint64_t, 64> bins{};
    dstd::String out;

    explicit DurationReport(const Config& config)
        : out(config.get("out", "duration_report.jsonl.gz")) {}

    void step(const Batch& b, Host) {
        for (const Event& e : b) {
            if (!e.has_dur()) continue;
            ++bins[dstd::bits::ilog2_u64(e.dur())];
        }
    }

    void merge(DurationReport& other) {
        for (std::size_t i = 0; i < bins.size(); ++i) bins[i] += other.bins[i];
    }

    Task on_finalize(Host h) {
        GzWriter writer = co_await h.fileio().gz(out.c_str());
        co_await writer.open();
        char line[96];
        for (int i = 0; i < static_cast<int>(bins.size()); ++i) {
            if (!bins[i]) continue;
            int n = std::snprintf(line, sizeof(line),
                                  "{\"log2_us\":%d,\"count\":%llu}\n", i,
                                  static_cast<unsigned long long>(bins[i]));
            co_await writer.append(
                dstd::StringView{line, static_cast<std::size_t>(n)});
        }
        GzSummary summary = co_await writer.close();
        std::fprintf(stderr, "duration_report: lines=%llu path=%s\n",
                     static_cast<unsigned long long>(summary.lines()),
                     out.c_str());
    }
};

DFTU_PLUGIN("duration_report", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.fold<DurationReport>();
                return {};
            })
