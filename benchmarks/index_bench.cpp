// Benchmark: index build, index size, decode, pruning and a fixed query set
// over real trace inputs, for comparing index changes against a recorded
// baseline. Each input runs in its own child process so peak memory is per
// input. Times are medians over repeated runs; with --baseline, a metric
// regresses when the bootstrap 95% interval of new/base (base/new for
// throughput) has its lower bound above 1.05, and the exit status is 1.
//
// Not a unit test (too slow for CI): built only with
// DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   index_bench [--out F] [--baseline F] [--workdir D] [--build-runs N]
//               [--runs N] [--warmups N] [--memory-budget BYTES]
//               [--aggregation-interval-us US] INPUT...
//
// --memory-budget bounds the index build (IndexerOptions::memory_budget);
// build_peak_rss_bytes is the peak before any query runs.
// --aggregation-interval-us also builds the aggregation tier at that interval.
//
// An INPUT is a .pfw.gz file or a directory scanned recursively.

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <simdjson.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;

namespace {

constexpr double MAX_SLOWDOWN = 0.05;

struct Options {
    std::string out = "index_bench.json";
    std::string baseline;
    std::string workdir = (fs::temp_directory_path() / "index_bench").string();
    int build_runs = 3;
    int runs = 10;
    int warmups = 2;
    std::uint64_t memory_budget = 0;
    std::uint64_t aggregation_interval_us = 0;
    std::vector<std::string> inputs;
};

template <class T>
T run(coro::CoroTask<T> t) {
    return default_runtime().submit(std::move(t)).get();
}

template <class Fn>
void run_scoped(Fn fn) {
    default_runtime()
        .submit(run_coro_scope(default_runtime().executor(), std::move(fn)))
        .wait();
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

std::vector<double> timed(const std::function<void()>& f, int warmups,
                          int runs) {
    for (int i = 0; i < warmups; ++i) f();
    std::vector<double> t;
    for (int i = 0; i < runs; ++i) t.push_back(seconds(f));
    return t;
}

std::uint64_t dir_bytes(const fs::path& p) {
    std::uint64_t n = 0;
    for (const auto& e : fs::recursive_directory_iterator(p))
        if (e.is_regular_file()) n += e.file_size();
    return n;
}

std::uint64_t peak_rss_bytes() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return static_cast<std::uint64_t>(u.ru_maxrss);
#else
    return static_cast<std::uint64_t>(u.ru_maxrss) * 1024;
#endif
}

std::vector<std::string> trace_files(const std::string& input) {
    std::vector<std::string> files;
    if (fs::is_regular_file(input)) return {input};
    for (const auto& e : fs::recursive_directory_iterator(input)) {
        const std::string p = e.path().string();
        if (e.is_regular_file() && p.size() > 7 &&
            p.compare(p.size() - 7, 7, ".pfw.gz") == 0)
            files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out + "\"";
}

// Filters built from the most frequent name, cat and scalar args value in
// the first lines of the first file, so the query set adapts to any input
// without timing the discovery.
struct QuerySet {
    std::string name;
    std::string cat;
    std::string args;
    std::string ts;
};

bool is_metadata(simdjson::dom::object o) {
    simdjson::dom::element ph;
    if (o["ph"].get(ph) != simdjson::SUCCESS) return true;
    std::string_view letter;
    std::int64_t n = 0;
    if (ph.get(letter) == simdjson::SUCCESS) return letter == "M";
    return ph.get(n) == simdjson::SUCCESS && n == 4;
}

coro::CoroTask<QuerySet> sample_queries(std::string file) {
    std::map<std::string, int> names, cats, args;
    std::vector<std::int64_t> stamps;
    simdjson::dom::parser parser;
    auto gen =
        utilities::fileio::lines::sources::async_streaming_gz_lines(file);
    int seen = 0;
    while (auto line = co_await gen.next()) {
        std::string_view l = line->content;
        while (!l.empty() && (l.back() == ',' || l.back() == ' '))
            l.remove_suffix(1);
        if (l.empty() || l.front() != '{') continue;
        simdjson::dom::object o;
        if (parser.parse(l.data(), l.size()).get(o) != simdjson::SUCCESS ||
            is_metadata(o))
            continue;
        std::int64_t ts = 0;
        if (o["ts"].get(ts) == simdjson::SUCCESS && ts > 0)
            stamps.push_back(ts);
        std::string_view v;
        if (o["name"].get(v) == simdjson::SUCCESS) ++names[std::string(v)];
        if (o["cat"].get(v) == simdjson::SUCCESS) ++cats[std::string(v)];
        simdjson::dom::object a;
        if (o["args"].get(a) == simdjson::SUCCESS) {
            for (auto [k, x] : a) {
                if (k == "hhash" || k == "fhash") continue;
                if (x.is_string())
                    ++args["args." + std::string(k) +
                           " == " + quote(x.get_string().value_unsafe())];
                else if (x.is_int64())
                    ++args["args." + std::string(k) + " == " +
                           std::to_string(x.get_int64().value_unsafe())];
                else if (x.is_uint64())
                    ++args["args." + std::string(k) + " == " +
                           std::to_string(x.get_uint64().value_unsafe())];
            }
        }
        if (++seen == 2000) break;
    }
    auto top = [](const std::map<std::string, int>& m) {
        std::string best;
        int n = -1;
        for (const auto& [k, c] : m)
            if (c > n) best = k, n = c;
        return best;
    };
    QuerySet q;
    q.name = "name == " + quote(top(names));
    q.cat = "cat == " + quote(top(cats));
    q.args = top(args);
    if (stamps.size() >= 2) {
        std::sort(stamps.begin(), stamps.end());
        const std::int64_t lo = stamps[stamps.size() / 10];
        const std::int64_t hi = stamps[stamps.size() * 9 / 10];
        const std::int64_t mid = stamps[stamps.size() / 2];
        const std::int64_t width = std::max<std::int64_t>(1, (hi - lo) / 10);
        q.ts = "ts >= " + std::to_string(mid) + " and ts < " +
               std::to_string(mid + width);
    }
    co_return q;
}

using Samples = std::vector<double>;

struct Result {
    std::uint64_t files = 0;
    std::uint64_t compressed_bytes = 0;
    std::uint64_t events = 0;
    std::uint64_t index_bytes = 0;
    std::uint64_t peak_rss = 0;
    std::uint64_t build_peak_rss = 0;
    Samples build_s;
    Samples decode_s;
    std::map<std::string, Samples> prune_us_per_file;
    std::map<std::string, Samples> query_s;
    std::map<std::string, std::string> queries;
};

View open_view(const std::string& input, const std::string& index_dir) {
    View v = fs::is_regular_file(input)
                 ? View::from_file(input, index_dir)
                 : run(View::from_directory(input, index_dir));
    return v.metadata(false);
}

Result measure(const Options& opt, const std::string& input,
               const std::string& index_dir) {
    Result r;
    const auto files = trace_files(input);
    r.files = files.size();
    for (const auto& f : files) r.compressed_bytes += fs::file_size(f);

    for (int i = 0; i < opt.build_runs; ++i) {
        std::error_code ec;
        fs::remove_all(index_dir, ec);
        fs::create_directories(index_dir);
        r.build_s.push_back(seconds([&] {
            dftracer::utils::index::IndexerOptions options;
            options.index_dir = index_dir;
            options.bloom->required = false;
            options.memory_budget = opt.memory_budget;
            if (opt.aggregation_interval_us > 0) {
                dftracer::utils::index::schemas::dft::agg::AggregationConfig
                    agg;
                agg.time_interval_us = opt.aggregation_interval_us;
                options.aggregation = agg;
            }
            auto indexer =
                dftracer::utils::index::Indexer::open(files, options);
            run_scoped([&](CoroScope& scope) -> coro::CoroTask<void> {
                co_await indexer.rebuild(scope);
            });
        }));
    }
    r.build_peak_rss = peak_rss_bytes();
    std::string index_path;
    for (const auto& e : fs::recursive_directory_iterator(index_dir))
        if (e.is_directory() && e.path().filename() == ".dftindex")
            index_path = e.path().string();
    if (index_path.empty()) {
        std::fprintf(stderr, "no index built under %s\n", index_dir.c_str());
        std::exit(1);
    }
    r.index_bytes = dir_bytes(index_dir);

    const View view = open_view(input, index_dir);
    r.decode_s = timed(
        [&] {
            ExportStats s = run(view.for_each_batch(
                [](std::size_t, const std::vector<std::string_view>&) {}, 1));
            r.events = s.events_scanned;
        },
        opt.warmups, opt.runs);

    const QuerySet qs = run(sample_queries(files.front()));
    r.queries = {{"name", qs.name}, {"cat", qs.cat}};
    if (!qs.ts.empty()) r.queries["ts"] = qs.ts;
    if (!qs.args.empty()) r.queries["args"] = qs.args;

    for (const auto& [key, text] : r.queries) {
        const auto parsed = query::parse_or_throw(text);
        dftracer::utils::index::plan::ChunkPrunerBatchInput in;
        in.index_path = index_path;
        for (const auto& f : files) in.items.push_back({f, parsed});
        dftracer::utils::index::plan::ChunkPruner pruner;
        Samples per_file;
        for (double s : timed([&] { (void)pruner.process_batch(in); },
                              opt.warmups, opt.runs))
            per_file.push_back(s * 1e6 / static_cast<double>(files.size()));
        r.prune_us_per_file[key] = std::move(per_file);
        r.query_s[key] = timed(
            [&] {
                run(view.query(text).agg({{AggOp::Count, "", "n"}}).collect());
            },
            opt.warmups, opt.runs);
    }
    r.query_s["group_by"] = timed(
        [&] {
            run(view.group_by({GroupKey::name()})
                    .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "s"}})
                    .collect());
        },
        opt.warmups, opt.runs);
    r.query_s["flamegraph"] =
        timed([&] { run(view.query(qs.cat).flamegraph().collect()); },
              opt.warmups, opt.runs);
    r.peak_rss = peak_rss_bytes();
    return r;
}

void put_samples(simdjson::builder::string_builder& sb, const Samples& s) {
    sb.start_array();
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i) sb.append_comma();
        sb.append(s[i]);
    }
    sb.end_array();
}

void put_map(simdjson::builder::string_builder& sb, std::string_view key,
             const std::map<std::string, Samples>& m) {
    sb.escape_and_append_with_quotes(key);
    sb.append_colon();
    sb.start_object();
    bool first = true;
    for (const auto& [k, s] : m) {
        if (!first) sb.append_comma();
        first = false;
        sb.escape_and_append_with_quotes(k);
        sb.append_colon();
        put_samples(sb, s);
    }
    sb.end_object();
}

std::string to_json(const Result& r) {
    simdjson::builder::string_builder sb;
    sb.start_object();
    sb.append_key_value("files", r.files);
    sb.append_comma();
    sb.append_key_value("compressed_bytes", r.compressed_bytes);
    sb.append_comma();
    sb.append_key_value("events", r.events);
    sb.append_comma();
    sb.append_key_value("index_bytes", r.index_bytes);
    sb.append_comma();
    sb.append_key_value("peak_rss_bytes", r.peak_rss);
    sb.append_comma();
    sb.append_key_value("build_peak_rss_bytes", r.build_peak_rss);
    sb.append_comma();
    sb.escape_and_append_with_quotes("build_s");
    sb.append_colon();
    put_samples(sb, r.build_s);
    sb.append_comma();
    sb.escape_and_append_with_quotes("decode_s");
    sb.append_colon();
    put_samples(sb, r.decode_s);
    sb.append_comma();
    put_map(sb, "prune_us_per_file", r.prune_us_per_file);
    sb.append_comma();
    put_map(sb, "query_s", r.query_s);
    sb.append_comma();
    sb.escape_and_append_with_quotes("queries");
    sb.append_colon();
    sb.start_object();
    bool first = true;
    for (const auto& [k, q] : r.queries) {
        if (!first) sb.append_comma();
        first = false;
        sb.append_key_value(std::string_view(k), std::string_view(q));
    }
    sb.end_object();
    sb.end_object();
    return std::string(sb);
}

// 95% bootstrap interval of median(num) / median(den).
std::pair<double, double> ratio_ci(const Samples& num, const Samples& den) {
    std::mt19937_64 rng(11);
    std::uniform_int_distribution<std::size_t> pn(0, num.size() - 1);
    std::uniform_int_distribution<std::size_t> pd(0, den.size() - 1);
    std::vector<double> ratios;
    for (int b = 0; b < 2000; ++b) {
        Samples n, d;
        for (std::size_t i = 0; i < num.size(); ++i) n.push_back(num[pn(rng)]);
        for (std::size_t i = 0; i < den.size(); ++i) d.push_back(den[pd(rng)]);
        ratios.push_back(median(n) / median(d));
    }
    std::sort(ratios.begin(), ratios.end());
    return {ratios[50], ratios[1949]};
}

Samples samples_of(simdjson::dom::element e) {
    Samples s;
    simdjson::dom::array a;
    if (e.get(a) != simdjson::SUCCESS) return s;
    for (auto v : a) {
        double d = 0;
        if (v.get(d) == simdjson::SUCCESS) s.push_back(d);
    }
    return s;
}

// Prints one line per metric; returns true when any regressed.
bool compare(simdjson::dom::object base, simdjson::dom::object cur) {
    bool regressed = false;
    auto check_times = [&](const std::string& label, simdjson::dom::element b,
                           simdjson::dom::element c) {
        const Samples bs = samples_of(b);
        const Samples cs = samples_of(c);
        if (bs.empty() || cs.empty()) return;
        const auto [lo, hi] = ratio_ci(cs, bs);
        const bool bad = lo > 1.0 + MAX_SLOWDOWN;
        regressed = regressed || bad;
        std::printf("  %-28s new/base %6.3f  [%6.3f, %6.3f]%s\n", label.c_str(),
                    median(cs) / median(bs), lo, hi, bad ? "  SLOWER" : "");
    };
    auto check_value = [&](const char* key) {
        std::uint64_t b = 0, c = 0;
        if (base[key].get(b) != simdjson::SUCCESS ||
            cur[key].get(c) != simdjson::SUCCESS || b == 0)
            return;
        const double ratio = static_cast<double>(c) / static_cast<double>(b);
        const bool bad = ratio > 1.0 + MAX_SLOWDOWN;
        regressed = regressed || bad;
        std::printf("  %-28s new/base %6.3f%s\n", key, ratio,
                    bad ? "  LARGER" : "");
    };
    check_times("build_s", base["build_s"], cur["build_s"]);
    check_times("decode_s", base["decode_s"], cur["decode_s"]);
    for (const char* group : {"prune_us_per_file", "query_s"}) {
        simdjson::dom::object bg, cg;
        if (base[group].get(bg) != simdjson::SUCCESS ||
            cur[group].get(cg) != simdjson::SUCCESS)
            continue;
        for (auto [k, v] : cg) {
            simdjson::dom::element bv;
            if (bg[k].get(bv) == simdjson::SUCCESS)
                check_times(std::string(group) + "." + std::string(k), bv, v);
        }
    }
    check_value("index_bytes");
    check_value("peak_rss_bytes");
    return regressed;
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--out")
            o.out = next();
        else if (a == "--baseline")
            o.baseline = next();
        else if (a == "--workdir")
            o.workdir = next();
        else if (a == "--build-runs")
            o.build_runs = std::stoi(next());
        else if (a == "--runs")
            o.runs = std::stoi(next());
        else if (a == "--warmups")
            o.warmups = std::stoi(next());
        else if (a == "--memory-budget")
            o.memory_budget = std::stoull(next());
        else if (a == "--aggregation-interval-us")
            o.aggregation_interval_us = std::stoull(next());
        else
            o.inputs.push_back(a);
    }
    if (o.inputs.empty() || o.build_runs < 1 || o.runs < 1 || o.warmups < 0) {
        std::fprintf(stderr,
                     "usage: index_bench [--out F] [--baseline F] "
                     "[--workdir D] [--build-runs N] [--runs N] "
                     "[--warmups N] [--memory-budget BYTES] "
                     "[--aggregation-interval-us US] INPUT...\n");
        std::exit(2);
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = parse_args(argc, argv);
    fs::create_directories(opt.workdir);

    std::string entries;
    for (std::size_t i = 0; i < opt.inputs.size(); ++i) {
        const std::string input = fs::absolute(opt.inputs[i]).string();
        const std::string part =
            opt.workdir + "/input_" + std::to_string(i) + ".json";
        const std::string index_dir =
            opt.workdir + "/index_" + std::to_string(i);
        // No runtime threads exist before this fork; the child starts its own.
        const pid_t pid = fork();
        if (pid == 0) {
            std::ofstream(part) << to_json(measure(opt, input, index_dir));
            std::_Exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::fprintf(stderr, "input failed: %s\n", input.c_str());
            return 1;
        }
        std::stringstream body;
        body << std::ifstream(part).rdbuf();
        if (!entries.empty()) entries += ",";
        entries += quote(input) + ":" + body.str();
        std::printf("measured %s\n", input.c_str());
    }

    char host[256] = {};
    gethostname(host, sizeof(host) - 1);
    std::string doc = "{\"meta\":{\"host\":" + quote(host) +
                      ",\"compiler\":" + quote(__VERSION__) +
#ifdef NDEBUG
                      ",\"build\":\"release\"" +
#else
                      ",\"build\":\"debug\"" +
#endif
                      ",\"runs\":" + std::to_string(opt.runs) +
                      ",\"build_runs\":" + std::to_string(opt.build_runs) +
                      "},\"inputs\":{" + entries + "}}\n";
    std::ofstream(opt.out) << doc;
    std::printf("wrote %s\n", opt.out.c_str());

    if (opt.baseline.empty()) return 0;
    simdjson::dom::parser pb, pc;
    simdjson::dom::object base_inputs, cur_inputs;
    if (pb.load(opt.baseline)["inputs"].get(base_inputs) != simdjson::SUCCESS ||
        pc.parse(doc)["inputs"].get(cur_inputs) != simdjson::SUCCESS) {
        std::fprintf(stderr, "cannot read baseline %s\n", opt.baseline.c_str());
        return 1;
    }
    bool regressed = false;
    for (auto [input, cur] : cur_inputs) {
        simdjson::dom::object b, c;
        if (base_inputs[input].get(b) != simdjson::SUCCESS) {
            std::printf("%s: no baseline\n", std::string(input).c_str());
            continue;
        }
        (void)cur.get(c);
        std::printf("%s\n", std::string(input).c_str());
        regressed = compare(b, c) || regressed;
    }
    return regressed ? 1 : 0;
}
