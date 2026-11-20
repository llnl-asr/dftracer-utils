// SPDX-License-Identifier: MIT
// Benchmark: the cost of the row-index rules on the frame ops. filter, sort,
// join and group_by over an unindexed frame, which must be no slower than
// before the index existed, and over the same frame indexed on two columns.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   frame_index_bench [rows] [reps]
//
// Compile with -DDFTU_BENCH_NO_INDEX against a tree without the row index to
// time the baseline: only the unindexed rows run.

#include <bench_ok.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;

using dataframe::DataFrame;
using dataframe::GroupAgg;
using dataframe::Series;

namespace {

double median_ms(dstd::Vector<double> ms) {
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

template <class Fn>
double time_ms(int reps, Fn&& fn) {
    dstd::Vector<double> ms;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = dstd::steady_now();
        fn();
        const auto t1 = dstd::steady_now();
        ms.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return median_ms(std::move(ms));
}

DataFrame make_frame(std::int64_t n, std::int64_t groups) {
    dstd::Vector<std::int64_t> k1(static_cast<std::size_t>(n));
    dstd::Vector<std::int64_t> k2(static_cast<std::size_t>(n));
    dstd::Vector<std::int64_t> v(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        const auto z = static_cast<std::size_t>(i);
        k1[z] = (i * 7919) % groups;
        k2[z] = i % 7;
        v[z] = (i * 31) % 1000;
    }
    DataFrame frame;
    frame.names = dstd::make_shared_strings({"k1", "k2", "v"});
    frame.columns.push_back(Series::flat_i64(k1.data(), n));
    frame.columns.push_back(Series::flat_i64(k2.data(), n));
    frame.columns.push_back(Series::flat_i64(v.data(), n));
    return frame;
}

void run(dstd::CStringView label, const DataFrame& frame, const DataFrame& dim,
         int reps) {
    const Series mask = frame.column("v").gt(500);
    const dstd::Vector<dstd::String> k1 = {"k1"};
    const dstd::Vector<dstd::String> k12 = {"k1", "k2"};
    const dstd::Vector<GroupAgg> sum = {
        GroupAgg{dataframe::Agg::Sum, "v", "s"}};
    const double filter =
        time_ms(reps, [&] { (void)frame.filter(mask).num_rows(); });
    const double sort = time_ms(
        reps, [&] { (void)dftu::bench::ok(frame.sort_by("v")).num_rows(); });
    const double join = time_ms(
        reps, [&] { (void)dftu::bench::ok(frame.join(dim, k1)).num_rows(); });
    const double group = time_ms(reps, [&] {
        (void)dftu::bench::ok(frame.group_by(k12, sum)).num_rows();
    });
    std::printf("%-10s %10.2f %10.2f %10.2f %10.2f\n", label.c_str(), filter,
                sort, join, group);
}

}  // namespace

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && dstd::StringView(argv[1]) == "--smoke";
    const std::int64_t n =
        smoke ? 20000 : (argc > 1 ? std::atoll(argv[1]) : 1000000);
    const int reps = smoke ? 1 : (argc > 2 ? std::atoi(argv[2]) : 7);
    const std::int64_t groups = std::max<std::int64_t>(n / 10, 1);

    const DataFrame frame = make_frame(n, groups);
    DataFrame dim = make_frame(groups, groups);
    dim.names = dstd::make_shared_strings({"k1", "d2", "d3"});

    std::printf("rows=%lld reps=%d (median ms)\n", static_cast<long long>(n),
                reps);
    std::printf("%-10s %10s %10s %10s %10s\n", "frame", "filter", "sort",
                "join", "group_by");
    run("no index", frame, dim, reps);
#ifndef DFTU_BENCH_NO_INDEX
    const dstd::StringView two[] = {"k1", "k2"};
    const dstd::StringView one[] = {"k1"};
    const DataFrame indexed = dftu::bench::ok(frame.with_index(two));
    const DataFrame dim_indexed = dftu::bench::ok(dim.with_index(one));
    run("2-level", indexed, dim_indexed, reps);
#endif
    return 0;
}
