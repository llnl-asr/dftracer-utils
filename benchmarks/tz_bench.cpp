// SPDX-License-Identifier: MIT
// Benchmark: the batch offset lookup of a time zone, in ns per row, for
// ticks that rise (the forward cursor), ticks in random order over 76 years
// (a lookup at nearly every row) and ticks in random order within one year
// (a few segments), over a zone with a table (America/New_York,
// when the host has the zone files), a zone that is only a POSIX rule, and a
// fixed offset.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   tz_bench [rows] [reps]

#include <dftracer/utils/dataframe/internal/tz.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

constexpr std::int64_t NS = 1'000'000'000;

std::uint64_t next_random(std::uint64_t& state) {
    state += dstd::hash::GOLDEN_RATIO;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * dstd::hash::SPLITMIX64_MUL1;
    z = (z ^ (z >> 27)) * dstd::hash::SPLITMIX64_MUL2;
    return z ^ (z >> 31);
}

// `span` seconds of ticks in nanoseconds, from 2024-01-01.
dstd::Vector<std::int64_t> make_ticks(std::size_t n, std::uint64_t span,
                                      bool sorted) {
    std::uint64_t rng = 12345;
    dstd::Vector<std::int64_t> ticks(n);
    constexpr std::int64_t START = 1'704'067'200;
    for (std::size_t i = 0; i < n; ++i)
        ticks[i] =
            (START + static_cast<std::int64_t>(next_random(rng) % span)) * NS +
            static_cast<std::int64_t>(next_random(rng) % NS);
    if (sorted) std::sort(ticks.begin(), ticks.end());
    return ticks;
}

double median(dstd::Vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// Time the batch lookup and check it against the single lookup on a sample.
double run(const tz::Zone& zone, dstd::Span<const std::int64_t> ticks,
           int reps) {
    dstd::Vector<std::int32_t> out(ticks.size());
    const dstd::Span<const std::int64_t> in = ticks;
    const dstd::Span<std::int32_t> res(out.data(), out.size());
    dstd::Vector<double> ns_per_row;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = dstd::steady_now();
        zone.offsets_at(in, TimeUnit::Nano, res);
        const auto t1 = dstd::steady_now();
        ns_per_row.push_back(
            std::chrono::duration<double, std::nano>(t1 - t0).count() /
            static_cast<double>(ticks.size()));
    }
    for (std::size_t i = 0; i < ticks.size(); i += 97) {
        const std::int64_t seconds =
            ticks[i] / NS - (ticks[i] % NS < 0 ? 1 : 0);
        if (out[i] != zone.offset_at(seconds))
            dstd::fatal("tz_bench: batch offset differs from offset_at");
    }
    return median(std::move(ns_per_row));
}

struct Inputs {
    dstd::Vector<std::int64_t> sorted;
    dstd::Vector<std::int64_t> random;
    dstd::Vector<std::int64_t> random_in_year;
};

void report(dstd::StringView name, const tz::Zone& zone, const Inputs& in,
            int reps) {
    std::printf(
        "%-26.*s sorted %7.2f   random %7.2f   random in a year %7.2f  "
        "ns/row\n",
        static_cast<int>(name.size()), name.data(), run(zone, in.sorted, reps),
        run(zone, in.random, reps), run(zone, in.random_in_year, reps));
}

}  // namespace

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && dstd::StringView(argv[1]) == "--smoke";
    const std::size_t rows =
        smoke ? 20000
              : (argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1]))
                          : 1'000'000);
    const int reps = smoke ? 1 : (argc > 2 ? std::atoi(argv[2]) : 7);

    // 2024 to 2100 for the full span, and one year for the local one.
    constexpr std::uint64_t FULL_SPAN = 76ULL * 365 * 86400;
    constexpr std::uint64_t YEAR = 365ULL * 86400;
    const Inputs in{make_ticks(rows, FULL_SPAN, true),
                    make_ticks(rows, FULL_SPAN, false),
                    make_ticks(rows, YEAR, false)};

    std::printf("rows %zu, reps %d\n", rows, reps);
    auto rule = tz::zone_from_posix("EST5EDT", "EST5EDT,M3.2.0,M11.1.0");
    if (!rule) dstd::fatal("tz_bench: bad rule");
    report("POSIX rule EST5EDT", *rule, in, reps);

    const tz::Zone fixed = tz::fixed_zone("+05:30", 19800, "UTC+05:30");
    report("fixed offset +05:30", fixed, in, reps);

    auto table = tz::zone("America/New_York");
    if (table)
        report("America/New_York (table)", **table, in, reps);
    else
        std::printf("America/New_York: skipped, no zone files on this host\n");
    return 0;
}
