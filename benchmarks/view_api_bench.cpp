// Benchmark: each case runs one query two ways over the same generated trace
// in one process: a baseline (the raw scan plan, the engine over its rows, or
// one collect per plan) and the same query through View. 5 warmups, then 20
// timed runs alternating the two. It reports the median time of each and a
// bootstrap 95% confidence interval for their ratio, and exits non-zero when
// the interval's lower bound shows View more than 5% slower.
//
// Not a unit test (too slow for CI): built only with
// DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   view_api_bench [events] [workdir]

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;
namespace df = dftracer::utils::dataframe;
namespace compress = dftracer::utils::utilities::fileio::compress;
namespace scan = dftracer::utils::trace::views::detail::scan;

namespace {

constexpr int WARMUPS = 5;
constexpr int RUNS = 20;
constexpr double MAX_SLOWDOWN = 0.05;

template <class T>
T run(coro::CoroTask<T> t) {
    return default_runtime().submit(std::move(t)).get();
}

// A multi-member trace: POSIX/STDIO reads and writes across pids with a
// string and a numeric arg, one gzip member per ~1 MB of lines.
std::string write_trace(const std::string& dir, std::int64_t events) {
    const std::string gz = dir + "/bench.pfw.gz";
    std::ofstream out(gz, std::ios::binary);
    compress::GzipMemberCompressor comp(6);
    const char* names[] = {"read", "write", "open", "close", "fread", "fwrite"};
    const char* cats[] = {"POSIX", "POSIX", "POSIX", "POSIX", "STDIO", "STDIO"};
    std::mt19937_64 rng(7);
    std::string chunk;
    auto flush = [&] {
        if (chunk.empty()) return;
        auto m = comp.compress_member(
            reinterpret_cast<const std::uint8_t*>(chunk.data()), chunk.size());
        if (!m) {
            std::fprintf(stderr, "compress failed\n");
            std::exit(1);
        }
        out.write(reinterpret_cast<const char*>(m->data()),
                  static_cast<std::streamsize>(m->size()));
        chunk.clear();
    };
    for (std::int64_t i = 0; i < events; ++i) {
        const std::size_t k = rng() % 6;
        chunk += R"({"ph":"X","name":")";
        chunk += names[k];
        chunk += R"(","cat":")";
        chunk += cats[k];
        chunk += R"(","pid":)" + std::to_string(rng() % 16);
        chunk += R"(,"tid":)" + std::to_string(rng() % 4);
        chunk += R"(,"ts":)" + std::to_string(1000 + i * 10);
        chunk += R"(,"dur":)" + std::to_string(1 + rng() % 500);
        chunk += R"(,"args":{"fname":"/data/f)" + std::to_string(rng() % 64);
        chunk += R"(","size":)" + std::to_string(rng() % 65536) + "}}\n";
        if (chunk.size() > (1u << 20)) flush();
    }
    flush();
    return gz;
}

double seconds(const std::function<void()>& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// 95% bootstrap interval of median(view) / median(base).
std::pair<double, double> ratio_ci(const std::vector<double>& base_t,
                                   const std::vector<double>& view_t) {
    std::mt19937_64 rng(11);
    std::uniform_int_distribution<std::size_t> pick(0, base_t.size() - 1);
    std::vector<double> ratios;
    for (int b = 0; b < 2000; ++b) {
        std::vector<double> o, n;
        for (std::size_t i = 0; i < base_t.size(); ++i) {
            o.push_back(base_t[pick(rng)]);
            n.push_back(view_t[pick(rng)]);
        }
        ratios.push_back(median(n) / median(o));
    }
    std::sort(ratios.begin(), ratios.end());
    return {ratios[50], ratios[1949]};
}

struct Case {
    const char* name;
    std::function<void()> baseline;
    std::function<void()> view;
};

}  // namespace

int main(int argc, char** argv) {
    const std::int64_t events = argc > 1 ? std::atoll(argv[1]) : 2'000'000;
    const std::string dir =
        argc > 2 ? argv[2] : fs::temp_directory_path().string() + "/view_bench";
    fs::create_directories(dir);
    const std::string gz = write_trace(dir, events);
    std::printf("trace: %s (%lld events)\n", gz.c_str(),
                static_cast<long long>(events));

    // First touch builds the index; keep it out of the timings.
    run(View::from_file(gz)
            .metadata(false)
            .agg({{AggOp::Count, "", "n"}})
            .collect());

    const View tv = View::from_file(gz).metadata(false);
    const scan::ScanPlan base = scan::metadata(scan::from_file(gz), false);
    const auto cols = tv.schema();
    auto at = [&](const char* name) {
        return static_cast<std::int32_t>(
            std::find(cols.begin(), cols.end(), name) - cols.begin());
    };

    std::vector<Case> cases = {
        {"agg name: count, sum dur",
         [&] {
             scan::ScanPlan p =
                 scan::agg(scan::group_by(base, {GroupKey::name()}),
                           {{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "s"}});
             run(scan::collect_frame(p));
         },
         [&] {
             run(tv.group_by({GroupKey::name()})
                     .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "s"}})
                     .collect());
         }},
        {"filter + agg pid, top 5",
         [&] {
             scan::ScanPlan p = scan::topk(
                 scan::agg(
                     scan::group_by(scan::query(base, R"(cat == "POSIX")"),
                                    {GroupKey::pid()}),
                     {{AggOp::Mean, "dur", "m"}}),
                 "m", 5);
             run(scan::collect_frame(p));
         },
         [&] {
             run(tv.query(R"(cat == "POSIX")")
                     .group_by({GroupKey::pid()})
                     .agg({{AggOp::Mean, "dur", "m"}})
                     .sort_by("m", true)
                     .head(5)
                     .collect());
         }},
        {"time bucket 10ms",
         [&] {
             scan::ScanPlan p =
                 scan::agg(scan::group_by(scan::time_bucket(base, 10000),
                                          {GroupKey::cat()}),
                           {{AggOp::Count, "", "n"}});
             run(scan::collect_frame(p));
         },
         [&] {
             run(tv.time_bucket(10000)
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}})
                     .collect());
         }},
        {"row query, select",
         [&] {
             scan::ScanPlan p =
                 scan::select(scan::query(base, R"(name == "fwrite")"),
                              {"name", "dur", "args.size"});
             run(scan::collect_frame(p));
         },
         [&] {
             run(tv.query(R"(name == "fwrite")")
                     .select({"name", "dur", "args.size"})
                     .collect());
         }},
        {"flamegraph",
         [&] {
             scan::ScanPlan p = scan::query(base, R"(cat == "POSIX")");
             run(scan::flamegraph(p, {"pid", "tid"}, "ts", "dur", "name", {}));
         },
         [&] { run(tv.query(R"(cat == "POSIX")").flamegraph().collect()); }},
        {"generic group_by (absorbed)",
         [&] {
             scan::ScanPlan p =
                 scan::agg(scan::group_by(base, {GroupKey::name()}),
                           {{AggOp::Sum, "dur", "s"}});
             run(scan::collect_frame(p));
         },
         [&] {
             run(tv.group_by(std::vector<std::string>{"name"},
                             {{df::Agg::Sum, "dur", "s"}})
                     .collect());
         }},
        {"expression key vs engine over rows",
         [&] {
             df::LazyFrame lf = scan::collect(base);
             run(lf.with_column("big", df::col(at("dur")) > std::int64_t{250})
                     .group_by(std::vector<std::string>{"big"},
                               {{df::Agg::Count, "", "n"}})
                     .collect());
         },
         [&] {
             run(tv.with_column("big", df::col(at("dur")) > std::int64_t{250})
                     .group_by(std::vector<std::string>{"big"},
                               {{df::Agg::Count, "", "n"}})
                     .collect());
         }},
        {"session vs collect_all: tree + agg",
         [&] {
             TraceSession s = tv.session();
             auto both = s.collect(tv.containment());
             auto by_name = s.collect(tv.group_by({GroupKey::name()})
                                          .agg({{AggOp::Count, "", "n"}})
                                          .lazy());
             run(s.execute());
         },
         [&] {
             run(df::collect_all(tv.containment(),
                                 tv.group_by({GroupKey::name()})
                                     .agg({{AggOp::Count, "", "n"}})));
         }},
        {"4 plans: separate vs collect_all",
         [&] {
             run(tv.group_by({GroupKey::name()})
                     .agg({{AggOp::Count, "", "n"}})
                     .collect());
             run(tv.query(R"(cat == "STDIO")").collect());
             run(tv.query("dur > 400").collect());
             run(tv.group_by({GroupKey::pid()})
                     .agg({{AggOp::Max, "dur", "mx"}})
                     .collect());
         },
         [&] {
             run(df::collect_all({tv.group_by({GroupKey::name()})
                                      .agg({{AggOp::Count, "", "n"}})
                                      .lazy(),
                                  tv.query(R"(cat == "STDIO")").lazy(),
                                  tv.query("dur > 400").lazy(),
                                  tv.group_by({GroupKey::pid()})
                                      .agg({{AggOp::Max, "dur", "mx"}})
                                      .lazy()}));
         }},
    };

    bool regressed = false;
    std::printf("%-36s %10s %10s %8s %20s\n", "case", "base ms", "view ms",
                "view/base", "95% CI");
    for (const Case& c : cases) {
        for (int i = 0; i < WARMUPS; ++i) {
            c.baseline();
            c.view();
        }
        std::vector<double> base_t, view_t;
        for (int i = 0; i < RUNS; ++i) {
            base_t.push_back(seconds(c.baseline));
            view_t.push_back(seconds(c.view));
        }
        const double mo = median(base_t), mn = median(view_t);
        const auto [lo, hi] = ratio_ci(base_t, view_t);
        const bool bad = lo > 1.0 + MAX_SLOWDOWN;
        regressed = regressed || bad;
        std::printf("%-36s %10.2f %10.2f %8.3f   [%6.3f, %6.3f]%s\n", c.name,
                    mo * 1e3, mn * 1e3, mn / mo, lo, hi, bad ? "  SLOWER" : "");
    }
    return regressed ? 1 : 0;
}
