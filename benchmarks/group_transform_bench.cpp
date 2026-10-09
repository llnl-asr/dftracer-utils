// Benchmark: a group-wise transform as the native plan step against the
// window plan it replaces (a window, a sort by row number and a projection),
// in memory, over an integer and a float column.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   group_transform_bench [rows] [groups] [reps] [op]
//
// With `op` (cumsum, cummax, cumcount, shift(1), diff, ffill) only that op
// runs.

#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/native_transform.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/series.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace df = dftracer::utils::dataframe;
using df::DataFrame;
using df::GroupwiseOp;
using df::LazyFrame;
using df::RankMethod;
using df::Series;

namespace {

constexpr std::int64_t MORSEL_ROWS = 65536;

DataFrame collect(LazyFrame plan) {
    return dftracer::utils::default_runtime()
        .submit(plan.collect(MORSEL_ROWS))
        .get();
}

double median_ms(std::vector<double> ms) {
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    const std::int64_t n = argc > 1 ? std::atoll(argv[1]) : 2000000;
    const std::int64_t groups = argc > 2 ? std::atoll(argv[2]) : 20000;
    const int reps = argc > 3 ? std::atoi(argv[3]) : 5;
    const std::string only = argc > 4 ? argv[4] : "";

    std::vector<std::int64_t> k(static_cast<std::size_t>(n));
    std::vector<std::int64_t> v(static_cast<std::size_t>(n));
    std::vector<double> f(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        const auto z = static_cast<std::size_t>(i);
        k[z] = (i * 7919) % groups;
        v[z] = i % 1000;
        f[z] = static_cast<double>(i % 977) * 0.5;
    }
    DataFrame frame;
    frame.names = {"k", "v", "f"};
    frame.columns.push_back(Series::flat_i64(k.data(), n));
    frame.columns.push_back(Series::flat_i64(v.data(), n));
    frame.columns.push_back(Series::flat_f64(f.data(), n));
    const std::vector<std::string> keys = {"k"};

    struct Case {
        const char* name;
        GroupwiseOp op;
        std::int64_t n;
    };
    const Case cases[] = {{"cumsum", GroupwiseOp::CumSum, 0},
                          {"cummax", GroupwiseOp::CumMax, 0},
                          {"cumcount", GroupwiseOp::CumCount, 0},
                          {"shift(1)", GroupwiseOp::Shift, 1},
                          {"diff", GroupwiseOp::Diff, 0},
                          {"ffill", GroupwiseOp::FFill, 0}};

    std::printf("rows=%lld groups=%lld reps=%d\n", static_cast<long long>(n),
                static_cast<long long>(groups), reps);
    std::printf("%-10s %12s %12s %8s\n", "op", "native ms", "window ms",
                "speedup");
    for (const Case& c : cases) {
        if (!only.empty() && only != c.name) continue;
        std::vector<double> native, composed;
        for (int r = 0; r < reps; ++r) {
            auto t0 = std::chrono::steady_clock::now();
            collect(frame.lazy().group_by(keys).transform(
                c.op, c.n, RankMethod::Average, true));
            auto t1 = std::chrono::steady_clock::now();
            collect(df::composed_group_transform(frame.lazy(), keys, c.op, c.n,
                                                 RankMethod::Average, true));
            auto t2 = std::chrono::steady_clock::now();
            native.push_back(
                std::chrono::duration<double, std::milli>(t1 - t0).count());
            composed.push_back(
                std::chrono::duration<double, std::milli>(t2 - t1).count());
        }
        const double a = median_ms(native);
        const double b = median_ms(composed);
        std::printf("%-10s %12.1f %12.1f %7.2fx\n", c.name, a, b, b / a);
    }
    return 0;
}
