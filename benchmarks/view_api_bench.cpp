// Benchmark: the View builder API against TraceViewer, the LazyFrame over the
// same trace source, on the same generated trace in one process.
//
// Each case runs the same query both ways: 5 warmups, then 20 timed runs
// alternating old and new. It reports the median time of each and a bootstrap
// 95% confidence interval for new/old, and exits non-zero when the interval's
// lower bound shows the new API more than 5% slower.
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

// 95% bootstrap interval of median(new) / median(old).
std::pair<double, double> ratio_ci(const std::vector<double>& old_t,
                                   const std::vector<double>& new_t) {
    std::mt19937_64 rng(11);
    std::uniform_int_distribution<std::size_t> pick(0, old_t.size() - 1);
    std::vector<double> ratios;
    for (int b = 0; b < 2000; ++b) {
        std::vector<double> o, n;
        for (std::size_t i = 0; i < old_t.size(); ++i) {
            o.push_back(old_t[pick(rng)]);
            n.push_back(new_t[pick(rng)]);
        }
        ratios.push_back(median(n) / median(o));
    }
    std::sort(ratios.begin(), ratios.end());
    return {ratios[50], ratios[1949]};
}

struct Case {
    const char* name;
    std::function<void()> old_api;
    std::function<void()> new_api;
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
            .collect_frame());

    const View view = View::from_file(gz).metadata(false);
    const TraceViewer tv = TraceViewer::from_file(gz).metadata(false);
    const auto cols = tv.schema();
    auto at = [&](const char* name) {
        return static_cast<std::int32_t>(
            std::find(cols.begin(), cols.end(), name) - cols.begin());
    };

    std::vector<Case> cases = {
        {"agg name: count, sum dur",
         [&] {
             run(view.group_by({GroupKey::name()})
                     .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "s"}})
                     .collect()
                     .collect());
         },
         [&] {
             run(tv.group_by({GroupKey::name()})
                     .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "s"}})
                     .collect());
         }},
        {"filter + agg pid, top 5",
         [&] {
             run(view.query(R"(cat == "POSIX")")
                     .group_by({GroupKey::pid()})
                     .agg({{AggOp::Mean, "dur", "m"}})
                     .topk("m", 5)
                     .collect()
                     .collect());
         },
         [&] {
             run(tv.query(R"(cat == "POSIX")")
                     .group_by({GroupKey::pid()})
                     .agg({{AggOp::Mean, "dur", "m"}})
                     .topk("m", 5)
                     .collect());
         }},
        {"time bucket 10ms",
         [&] {
             run(view.time_bucket(10000)
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}})
                     .collect()
                     .collect());
         },
         [&] {
             run(tv.time_bucket(10000)
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}})
                     .collect());
         }},
        {"row query, select",
         [&] {
             run(view.query(R"(name == "fwrite")")
                     .select({"name", "dur", "args.size"})
                     .collect()
                     .collect());
         },
         [&] {
             run(tv.query(R"(name == "fwrite")")
                     .select({"name", "dur", "args.size"})
                     .collect());
         }},
        {"generic group_by (absorbed)",
         [&] {
             run(view.group_by({GroupKey::name()})
                     .agg({{AggOp::Sum, "dur", "s"}})
                     .collect()
                     .collect());
         },
         [&] {
             run(tv.group_by(std::vector<std::string>{"name"},
                             {{df::Agg::Sum, "dur", "s"}})
                     .collect());
         }},
        {"expression key vs engine over rows",
         [&] {
             run(view.collect()
                     .with_column("big", df::col(at("dur")) > std::int64_t{250})
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
        {"flamegraph",
         [&] { run(view.query(R"(cat == "POSIX")").flamegraph()); },
         [&] { run(tv.query(R"(cat == "POSIX")").flamegraph().collect()); }},
        {"session vs collect_all: tree + agg",
         [&] {
             ViewSession s = view.session();
             auto both = s.containment(view);
             auto by_name = s.collect(view.group_by({GroupKey::name()})
                                          .agg({{AggOp::Count, "", "n"}}));
             run(s.execute());
         },
         [&] {
             run(df::collect_all(tv.containment(),
                                 tv.group_by({GroupKey::name()})
                                     .agg({{AggOp::Count, "", "n"}})));
         }},
        {"4 plans: separate vs collect_all",
         [&] {
             run(view.group_by({GroupKey::name()})
                     .agg({{AggOp::Count, "", "n"}})
                     .collect()
                     .collect());
             run(view.query(R"(cat == "STDIO")").collect().collect());
             run(view.query("dur > 400").collect().collect());
             run(view.group_by({GroupKey::pid()})
                     .agg({{AggOp::Max, "dur", "mx"}})
                     .collect()
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
    std::printf("%-36s %10s %10s %8s %20s\n", "case", "old ms", "new ms",
                "new/old", "95% CI");
    for (const Case& c : cases) {
        for (int i = 0; i < WARMUPS; ++i) {
            c.old_api();
            c.new_api();
        }
        std::vector<double> old_t, new_t;
        for (int i = 0; i < RUNS; ++i) {
            old_t.push_back(seconds(c.old_api));
            new_t.push_back(seconds(c.new_api));
        }
        const double mo = median(old_t), mn = median(new_t);
        const auto [lo, hi] = ratio_ci(old_t, new_t);
        const bool bad = lo > 1.0 + MAX_SLOWDOWN;
        regressed = regressed || bad;
        std::printf("%-36s %10.2f %10.2f %8.3f   [%6.3f, %6.3f]%s\n", c.name,
                    mo * 1e3, mn * 1e3, mn / mo, lo, hi, bad ? "  SLOWER" : "");
    }
    return regressed ? 1 : 0;
}
