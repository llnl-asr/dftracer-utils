// Benchmark: window frame functions over a partitioned, ordered frame, rows
// and range frames of width 100 and 10000, with FRAME_SUM as the baseline.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   window_bench [rows] [reps]

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace df = dftracer::utils::dataframe;
using df::DataFrame;
using df::Series;
using df::WindowColumn;
using df::WindowFrameMode;
using df::WindowFunc;

namespace {

constexpr std::int64_t PARTITIONS = 100;
constexpr std::int64_t DISTINCT_STRINGS = 1000;

struct Case {
    const char* name;
    WindowFunc func;
    const char* value;
};

constexpr Case CASES[] = {
    {"FRAME_SUM", WindowFunc::FrameSum, "v"},
    {"FRAME_VAR", WindowFunc::FrameVar, "v"},
    {"FRAME_STD", WindowFunc::FrameStd, "v"},
    {"FRAME_QUANTILE", WindowFunc::FrameQuantile, "v"},
    {"FRAME_COUNT_DISTINCT", WindowFunc::FrameCountDistinct, "s"},
    {"FRAME_ARG_MAX", WindowFunc::FrameArgMax, "s"},
    {"FRAME_ARG_MIN", WindowFunc::FrameArgMin, "s"},
    {"FRAME_COLLECT", WindowFunc::FrameCollect, "s"},
};

DataFrame make_frame(std::int64_t n) {
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> dist(0.0, 1000.0);
    std::uniform_int_distribution<std::int64_t> by_dist(0, 1000000);
    std::vector<std::int64_t> key(n), ts(n), by(n);
    std::vector<double> v(n);
    std::vector<std::string> s(n);
    for (std::int64_t i = 0; i < n; ++i) {
        key[i] = i % PARTITIONS;
        ts[i] = i / PARTITIONS;
        by[i] = by_dist(rng);
        v[i] = dist(rng);
        s[i] = "str" + std::to_string(rng() % DISTINCT_STRINGS);
    }
    DataFrame d;
    d.names = {"key", "ts", "v", "by", "s"};
    d.columns.push_back(Series::flat_i64(key.data(), n));
    d.columns.push_back(Series::flat_i64(ts.data(), n));
    d.columns.push_back(Series::flat_f64(v.data(), n));
    d.columns.push_back(Series::flat_i64(by.data(), n));
    d.columns.push_back(Series::strings(s));
    return d;
}

WindowColumn make_spec(const Case& c, WindowFrameMode mode,
                       std::int64_t width) {
    WindowColumn w{};
    w.func = c.func;
    w.out = "out";
    w.set_value(c.value);
    w.set_by("by");
    w.params.frame = {0, width, 0, mode, 0.5};
    return w;
}

}  // namespace

int main(int argc, char** argv) {
    const std::int64_t n = argc > 1 ? std::atoll(argv[1]) : 1000000;
    const int reps = argc > 2 ? std::atoi(argv[2]) : 3;
    const DataFrame frame = make_frame(n);
    const std::vector<std::string> part = {"key"};
    const std::vector<std::string> order = {"ts"};

    std::printf("rows=%lld reps=%d partitions=%lld\n",
                static_cast<long long>(n), reps,
                static_cast<long long>(PARTITIONS));
    for (const Case& c : CASES) {
        for (const WindowFrameMode mode :
             {WindowFrameMode::Rows, WindowFrameMode::Range}) {
            for (const std::int64_t width : {100, 10000}) {
                const char* mode_name =
                    mode == WindowFrameMode::Rows ? "rows" : "range";
                if (c.func == WindowFunc::FrameCollect && width == 10000) {
                    std::printf(
                        "%-22s %-5s width=%-6lld skipped (collect exceeds the "
                        "2^27 cap at this width)\n",
                        c.name, mode_name, static_cast<long long>(width));
                    continue;
                }
                const std::vector<WindowColumn> specs = {
                    make_spec(c, mode, width)};
                std::vector<double> ms;
                for (int r = 0; r < reps; ++r) {
                    const auto t0 = std::chrono::steady_clock::now();
                    DataFrame out = df::window(frame, part, order, specs);
                    const auto t1 = std::chrono::steady_clock::now();
                    ms.push_back(
                        std::chrono::duration<double, std::milli>(t1 - t0)
                            .count());
                }
                std::sort(ms.begin(), ms.end());
                const double med = ms[ms.size() / 2];
                std::printf("%-22s %-5s width=%-6lld %10.2f ms %10.1f ns/row\n",
                            c.name, mode_name, static_cast<long long>(width),
                            med, med * 1e6 / static_cast<double>(n));
            }
        }
    }
    return 0;
}
