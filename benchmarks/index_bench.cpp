// SPDX-License-Identifier: MIT
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
//   index_bench --smoke   (one tiny generated trace, one run, for ctest)
//
// --memory-budget bounds the index build (IndexerOptions::memory_budget);
// build_peak_rss_bytes is the peak before any query runs.
// --aggregation-interval-us sets the interval of the aggregation tier.
//
// An INPUT is a .pfw.gz file or a directory scanned recursively. A genesis
// output file (long or wide format) runs three filter queries and no
// group_by or flamegraph; a query with an arrow is not pruned, which only
// reads one file's own columns.

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function_ref.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
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
#include <random>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;

namespace {

/// Prints the error and exits with 1 when a setup call failed.
template <class T>
void require_ok(const dftracer::utils::dstd::Expected<
                    T, dftracer::utils::dstd::Error>& result,
                const char* what) {
    if (result.has_value()) return;
    const auto& error = result.error();
    std::fprintf(stderr, "%s failed: %.*s %.*s\n", what,
                 static_cast<int>(error.name().size()), error.name().data(),
                 static_cast<int>(error.message().size()),
                 error.message().data());
    std::exit(1);
}

constexpr double MAX_SLOWDOWN = 0.05;

struct Options {
    dstd::String out = "index_bench.json";
    dstd::String baseline;
    dstd::String workdir =
        dstd::String((dstd::fs::temp_directory() / "index_bench").str());
    int build_runs = 3;
    int runs = 10;
    int warmups = 2;
    std::uint64_t memory_budget = 0;
    std::uint64_t aggregation_interval_us = 0;
    dstd::Vector<dstd::String> inputs;
};

template <class T>
T run(coro::CoroTask<T> t) {
    return default_runtime().submit(std::move(t)).get();
}

template <class Fn>
void run_scoped(Fn fn) {
    default_runtime().submit(coro::with_scope(std::move(fn))).wait();
}

double seconds(dstd::FunctionRef<void()> f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
}

double median(dstd::Vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

dstd::Vector<double> timed(dstd::FunctionRef<void()> f, int warmups, int runs) {
    for (int i = 0; i < warmups; ++i) f();
    dstd::Vector<double> t;
    for (int i = 0; i < runs; ++i) t.push_back(seconds(f));
    return t;
}

std::uint64_t dir_bytes(dstd::PathView p) {
    std::uint64_t n = 0;
    require_ok(
        dstd::fs::walk(p,
                       [&](dstd::PathView path, dstd::fs::FileKind kind) {
                           if (kind == dstd::fs::FileKind::File) {
                               const auto st = dstd::fs::stat(path);
                               if (st) n += st->size;
                           }
                           return dstd::fs::WalkAction::Continue;
                       }),
        "walk");
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

dstd::Vector<dstd::String> trace_files(const dstd::String& input) {
    dstd::Vector<dstd::String> files;
    if (dstd::fs::is_regular_file(input).value()) return {input};
    require_ok(dstd::fs::walk(
                   input,
                   [&](dstd::PathView path, dstd::fs::FileKind kind) {
                       const dstd::StringView p = path.str();
                       if (kind == dstd::fs::FileKind::File && p.size() > 7 &&
                           p.compare(p.size() - 7, 7, ".pfw.gz") == 0)
                           files.emplace_back(p);
                       return dstd::fs::WalkAction::Continue;
                   }),
               "walk");
    std::sort(files.begin(), files.end());
    return files;
}

dstd::String quote(dstd::StringView s) {
    dstd::String out = "\"";
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
    dstd::String name;
    dstd::String cat;
    dstd::String args;
    dstd::String ts;
};

bool is_metadata(simdjson::dom::object o) {
    simdjson::dom::element ph;
    if (o["ph"].get(ph) != simdjson::SUCCESS) return true;
    dstd::StringView letter;
    std::int64_t n = 0;
    if (ph.get(letter) == simdjson::SUCCESS) return letter == "M";
    return ph.get(n) == simdjson::SUCCESS && n == 4;
}

coro::CoroTask<QuerySet> sample_queries(dstd::String file) {
    dstd::Map<dstd::String, int> names, cats, args;
    dstd::Vector<std::int64_t> stamps;
    simdjson::dom::parser parser;
    auto gen = utilities::fileio::lines::sources::async_streaming_gz_lines(
        std::move(file));
    int seen = 0;
    while (auto line = co_await gen.next()) {
        dstd::StringView l = line->content;
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
        dstd::StringView v;
        if (o["name"].get(v) == simdjson::SUCCESS) ++names[dstd::String(v)];
        if (o["cat"].get(v) == simdjson::SUCCESS) ++cats[dstd::String(v)];
        simdjson::dom::object a;
        if (o["args"].get(a) == simdjson::SUCCESS) {
            for (auto [k, x] : a) {
                if (k == "hhash" || k == "fhash") continue;
                if (x.is_string())
                    ++args["args." + dstd::String(k) +
                           " == " + quote(x.get_string().value_unsafe())];
                else if (x.is_int64())
                    ++args["args." + dstd::String(k) + " == " +
                           dstd::to_string(x.get_int64().value_unsafe())];
                else if (x.is_uint64())
                    ++args["args." + dstd::String(k) + " == " +
                           dstd::to_string(x.get_uint64().value_unsafe())];
            }
        }
        if (++seen == 2000) break;
    }
    auto top = [](const dstd::Map<dstd::String, int>& m) {
        dstd::String best;
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
        q.ts = "ts >= " + dstd::to_string(mid) + " and ts < " +
               dstd::to_string(mid + width);
    }
    co_return q;
}

dstd::String number_text(double d) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", d);
    return buf;
}

// Genesis output is not a trace: a long-format file has one `gtype` per line
// (run, func, counter) and the older wide format has a RUN line then ph 3
// lines. The three queries are the same filters in either shape: one run,
// functions above the median duration p50, one counter above its median p50.
// A metric with a zero median is passed over while another exists, so the
// filter selects some rows. Empty when the first line is neither shape.
coro::CoroTask<dstd::Map<dstd::String, dstd::String>> sample_genesis(
    dstd::String file) {
    dstd::Map<dstd::String, dstd::String> out;
    dstd::String app, run_id;
    dstd::Vector<double> durs;
    dstd::Map<dstd::String, dstd::Vector<double>> metrics;
    int shape = 0;  // 1 long, 2 wide
    simdjson::dom::parser parser;
    auto gen = utilities::fileio::lines::sources::async_streaming_gz_lines(
        std::move(file));
    int seen = 0;
    while (auto line = co_await gen.next()) {
        dstd::StringView l = line->content;
        simdjson::dom::object o;
        if (l.empty() || l.front() != '{' ||
            parser.parse(l.data(), l.size()).get(o) != simdjson::SUCCESS)
            continue;
        dstd::StringView v;
        double d = 0;
        if (shape == 0) {
            if (o["gtype"].get(v) == simdjson::SUCCESS &&
                (v == "run" || v == "func" || v == "counter"))
                shape = 1;
            else if (o["name"].get(v) == simdjson::SUCCESS && v == "RUN")
                shape = 2;
            else
                co_return out;
        }
        if (shape == 1) {
            if (o["gtype"].get(v) != simdjson::SUCCESS) continue;
            const dstd::String type(v);
            if (type == "run" && app.empty()) {
                if (o["app"].get(v) == simdjson::SUCCESS) app = dstd::String(v);
                if (o["run"].get(v) == simdjson::SUCCESS)
                    run_id = dstd::String(v);
            } else if (type == "func") {
                if (o["dur"]["p50"].get(d) == simdjson::SUCCESS)
                    durs.push_back(d);
            } else if (type == "counter") {
                dstd::StringView m;
                if (o["metric"].get(m) == simdjson::SUCCESS &&
                    o["v"]["p50"].get(d) == simdjson::SUCCESS)
                    metrics[dstd::String(m)].push_back(d);
            }
        } else {
            simdjson::dom::object a;
            if (o["args"].get(a) != simdjson::SUCCESS) continue;
            if (run_id.empty() && a["run"].get(v) == simdjson::SUCCESS)
                run_id = dstd::String(v);
            if (a["dur"]["p50"].get(d) == simdjson::SUCCESS) durs.push_back(d);
            simdjson::dom::object c;
            if (a["counters"].get(c) == simdjson::SUCCESS)
                for (auto [k, x] : c)
                    if (x["p50"].get(d) == simdjson::SUCCESS)
                        metrics[dstd::String(k)].push_back(d);
        }
        if (++seen == 20000) break;
    }
    if (shape == 0) co_return out;
    const bool lng = shape == 1;
    if (!run_id.empty())
        out["run"] = (lng ? "run == " : "args.run == ") + quote(run_id);
    if (lng && !app.empty())
        out["run_app"] = "run -> runs.app == " + quote(app);
    if (!durs.empty())
        out["func"] = dstd::String(lng ? "gtype == \"func\" and dur.p50 > "
                                       : "ph == 3 and args.dur.p50 > ") +
                      number_text(median(durs));
    const dstd::Vector<double>* best = nullptr;
    dstd::String metric;
    for (const auto& [m, vals] : metrics) {
        if (median(vals) <= 0) continue;
        if (!best || vals.size() > best->size()) best = &vals, metric = m;
    }
    if (!best)
        for (const auto& [m, vals] : metrics)
            if (!best || vals.size() > best->size()) best = &vals, metric = m;
    if (best) {
        const dstd::String med = number_text(median(*best));
        const dstd::String key = "`" + metric + "`";
        out["counter"] =
            lng ? "gtype == \"counter\" and metric == " + quote(metric) +
                      " and v.p50 > " + med
                : "args.counters." + key + ".p50 > " + med;
    }
    co_return out;
}

using Samples = dstd::Vector<double>;

struct Result {
    std::uint64_t files = 0;
    std::uint64_t compressed_bytes = 0;
    std::uint64_t events = 0;
    std::uint64_t lines = 0;
    std::uint64_t distinct_paths = 0;
    std::uint64_t index_bytes = 0;
    std::uint64_t peak_rss = 0;
    std::uint64_t build_peak_rss = 0;
    Samples build_s;
    Samples decode_s;
    dstd::Map<dstd::String, Samples> prune_us_per_file;
    dstd::Map<dstd::String, Samples> query_s;
    dstd::Map<dstd::String, dstd::String> queries;
    dstd::Map<dstd::String, std::int64_t> rows;
};

View open_view(const dstd::String& input, const dstd::String& index_dir) {
    View v = dstd::fs::is_regular_file(input).value()
                 ? View::from_file(input, index_dir)
                 : run(View::from_directory(input, index_dir));
    return v;
}

Result measure(const Options& opt, const dstd::String& input,
               const dstd::String& index_dir) {
    Result r;
    const auto files = trace_files(input);
    r.files = files.size();
    for (const auto& f : files)
        r.compressed_bytes += dstd::fs::file_size(f).value();

    for (int i = 0; i < opt.build_runs; ++i) {
        (void)dstd::fs::remove_all(index_dir);
        require_ok(dstd::fs::create_directories(index_dir),
                   "create_directories");
        r.build_s.push_back(seconds([&] {
            dftracer::utils::index::IndexerOptions options;
            options.index_dir = index_dir;
            options.bloom->required = false;
            options.memory_budget = opt.memory_budget;
            if (opt.aggregation_interval_us > 0)
                options.aggregation.time_interval_us =
                    opt.aggregation_interval_us;
            auto indexer =
                dftracer::utils::index::Indexer::open(files, options);
            run_scoped([&](coro::Scope& scope) -> coro::CoroTask<void> {
                co_await indexer.rebuild(scope);
            });
        }));
    }
    r.build_peak_rss = peak_rss_bytes();
    dstd::String index_path;
    require_ok(
        dstd::fs::walk(index_dir,
                       [&](dstd::PathView path, dstd::fs::FileKind kind) {
                           if (kind == dstd::fs::FileKind::Directory &&
                               path.filename() == ".dftindex")
                               index_path = dstd::String(path.str());
                           return dstd::fs::WalkAction::Continue;
                       }),
        "walk");
    if (index_path.empty()) {
        std::fprintf(stderr, "no index built under %s\n", index_dir.c_str());
        std::exit(1);
    }
    r.index_bytes = dir_bytes(index_dir);

    const View view = open_view(input, index_dir);
    r.decode_s = timed(
        [&] {
            ExportStats s = run(view.for_each_batch(
                [](std::size_t, const dstd::Vector<dstd::StringView>&) {}, 1));
            r.events = s.events_scanned;
        },
        opt.warmups, opt.runs);

    {
        const auto snapshot = common::unwrap(
            dftracer::utils::index::plan::open_snapshot(index_path));
        dstd::HashSet<dstd::String> paths;
        for (const auto& f : files) {
            const auto file = snapshot.file(
                dftracer::utils::index::store::internal::get_logical_path(f));
            if (!file) continue;
            if (auto meta = common::unwrap(
                    dftracer::utils::index::file_metadata(snapshot, *file)))
                r.lines += meta->num_lines;
            for (const auto& [p, stat] : common::unwrap(
                     dftracer::utils::index::catalog(snapshot, *file)))
                paths.insert(dstd::String(p));
        }
        r.distinct_paths = paths.size();
    }

    r.queries = run(sample_genesis(files.front()));
    const bool genesis = !r.queries.empty();
    QuerySet qs;
    if (!genesis) {
        qs = run(sample_queries(files.front()));
        r.queries = {{"name", qs.name}, {"cat", qs.cat}};
        if (!qs.ts.empty()) r.queries["ts"] = qs.ts;
        if (!qs.args.empty()) r.queries["args"] = qs.args;
    }

    for (const auto& [key, text] : r.queries) {
        if (text.find("->") == dstd::String::npos) {
            const auto parsed = duql::parse_or_throw(text);
            dftracer::utils::index::plan::ChunkPrunerBatchInput in;
            in.index_path = index_path;
            for (const auto& f : files) in.items.push_back({f, parsed});
            dftracer::utils::index::plan::ChunkPruner pruner;
            Samples per_file;
            for (double s : timed([&] { pruner.process_batch(in); },
                                  opt.warmups, opt.runs))
                per_file.push_back(s * 1e6 / static_cast<double>(files.size()));
            r.prune_us_per_file[key] = std::move(per_file);
        }
        if (genesis)
            r.rows[key] =
                run(view.duql(text).agg({{AggOp::Count, "", "n"}}).collect())
                    .column("n")
                    .materialize()
                    .data<std::int64_t>()[0];
        r.query_s[key] = timed(
            [&] {
                run(view.duql(text).agg({{AggOp::Count, "", "n"}}).collect());
            },
            opt.warmups, opt.runs);
    }
    if (genesis) {
        r.query_s["collect"] =
            timed([&] { run(view.collect()); }, opt.warmups, opt.runs);
        r.query_s["group_collect"] = timed(
            [&] {
                run(view.duql("where gtype == \"counter\" | group metric "
                              "{ n = count() }")
                        .collect());
            },
            opt.warmups, opt.runs);

        const dataframe::DataFrame frame =
            run(view.duql("where gtype == \"counter\"").collect());
        const dstd::String metric(frame.column("metric").string_at(0));
        r.rows["frame"] = frame.num_rows();
        r.query_s["frame_filter"] =
            timed([&] { frame.filter(frame.column("metric").str_eq(metric)); },
                  opt.warmups, opt.runs);
        r.query_s["frame_starts_with"] = timed(
            [&] {
                frame.filter(frame.column("metric").str_starts_with("cpu"));
            },
            opt.warmups, opt.runs);
        r.query_s["frame_sort"] =
            timed([&] { frame.sort_by("metric"); }, opt.warmups, opt.runs);
        r.query_s["frame_group"] = timed(
            [&] {
                frame.group_by(dstd::Vector<dstd::String>{"metric"}).size();
            },
            opt.warmups, opt.runs);
        const dataframe::DataFrame runs =
            frame.group_by(dstd::Vector<dstd::String>{"run"}).size();
        r.query_s["frame_join"] =
            timed([&] { frame.join(runs, dstd::Vector<dstd::String>{"run"}); },
                  opt.warmups, opt.runs);
        r.peak_rss = peak_rss_bytes();
        return r;
    }
    r.query_s["group_by"] = timed(
        [&] {
            run(view.group_by({GroupKey::name()})
                    .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "s"}})
                    .collect());
        },
        opt.warmups, opt.runs);
    r.query_s["flamegraph"] =
        timed([&] { run(view.duql(qs.cat).flamegraph().collect()); },
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

void put_map(simdjson::builder::string_builder& sb, dstd::StringView key,
             const dstd::Map<dstd::String, Samples>& m) {
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

dstd::String to_json(const Result& r) {
    simdjson::builder::string_builder sb;
    sb.start_object();
    sb.append_key_value("files", r.files);
    sb.append_comma();
    sb.append_key_value("compressed_bytes", r.compressed_bytes);
    sb.append_comma();
    sb.append_key_value("events", r.events);
    sb.append_comma();
    sb.append_key_value("lines", r.lines);
    sb.append_comma();
    sb.append_key_value("distinct_paths", r.distinct_paths);
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
    sb.escape_and_append_with_quotes("rows");
    sb.append_colon();
    sb.start_object();
    bool first_row = true;
    for (const auto& [k, n] : r.rows) {
        if (!first_row) sb.append_comma();
        first_row = false;
        sb.append_key_value(dstd::StringView(k), n);
    }
    sb.end_object();
    sb.append_comma();
    sb.escape_and_append_with_quotes("queries");
    sb.append_colon();
    sb.start_object();
    bool first = true;
    for (const auto& [k, q] : r.queries) {
        if (!first) sb.append_comma();
        first = false;
        sb.append_key_value(dstd::StringView(k), dstd::StringView(q));
    }
    sb.end_object();
    sb.end_object();
    return dstd::String(static_cast<dstd::StringView>(sb));
}

// 95% bootstrap interval of median(num) / median(den).
dstd::Pair<double, double> ratio_ci(const Samples& num, const Samples& den) {
    std::mt19937_64 rng(11);
    std::uniform_int_distribution<std::size_t> pn(0, num.size() - 1);
    std::uniform_int_distribution<std::size_t> pd(0, den.size() - 1);
    dstd::Vector<double> ratios;
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
    auto check_times = [&](const dstd::String& label, simdjson::dom::element b,
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
                check_times(dstd::String(group) + "." + dstd::String(k), bv, v);
        }
    }
    check_value("index_bytes");
    check_value("peak_rss_bytes");
    return regressed;
}

dstd::String write_smoke_trace(const dstd::String& dir) {
    require_ok(dstd::fs::create_directories(dir), "create_directories");
    const dstd::String gz = dir + "/smoke.pfw.gz";
    dstd::String lines;
    const char* names[] = {"read", "write", "open", "close"};
    for (int i = 0; i < 2000; ++i) {
        lines += R"({"ph":"X","name":")";
        lines += names[i % 4];
        lines += R"(","cat":"POSIX","pid":)" + dstd::to_string(i % 4);
        lines += R"(,"tid":1,"ts":)" + dstd::to_string(1000 + i * 10);
        lines += R"(,"dur":)" + dstd::to_string(1 + i % 50);
        lines +=
            R"(,"args":{"fname":"/data/f)" + dstd::to_string(i % 8) + "\"}}\n";
    }
    dftracer::utils::utilities::fileio::compress::GzipMemberCompressor comp(6);
    auto member = comp.compress_member(
        reinterpret_cast<const std::uint8_t*>(lines.data()), lines.size());
    if (!member) {
        std::fprintf(stderr, "compress failed\n");
        std::exit(1);
    }
    require_ok(
        dstd::fs::write_file(
            gz, dstd::StringView(reinterpret_cast<const char*>(member->data()),
                                 member->size())),
        "write_file");
    return gz;
}

template <class T>
T parse_number(dstd::StringView text, const char* flag) {
    const auto value = dstd::from_chars<T>(text);
    if (!value) {
        std::fprintf(stderr, "invalid value for %s\n", flag);
        std::exit(2);
    }
    return *value;
}

Options parse_args(int argc, char** argv) {
    Options o;
    if (argc == 2 && dstd::StringView(argv[1]) == "--smoke") {
        o.workdir = (dstd::fs::temp_directory() / "index_bench_smoke").str();
        o.out = o.workdir + "/index_bench.json";
        o.build_runs = 1;
        o.runs = 1;
        o.warmups = 0;
        o.inputs.push_back(write_smoke_trace(o.workdir));
        return o;
    }
    for (int i = 1; i < argc; ++i) {
        const dstd::String a = argv[i];
        auto next = [&]() -> dstd::String {
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
            o.build_runs = parse_number<int>(next(), "--build-runs");
        else if (a == "--runs")
            o.runs = parse_number<int>(next(), "--runs");
        else if (a == "--warmups")
            o.warmups = parse_number<int>(next(), "--warmups");
        else if (a == "--memory-budget")
            o.memory_budget =
                parse_number<std::uint64_t>(next(), "--memory-budget");
        else if (a == "--aggregation-interval-us")
            o.aggregation_interval_us = parse_number<std::uint64_t>(
                next(), "--aggregation-interval-us");
        else
            o.inputs.push_back(a);
    }
    if (o.inputs.empty() || o.build_runs < 1 || o.runs < 1 || o.warmups < 0) {
        std::fprintf(stderr,
                     "usage: index_bench [--out F] [--baseline F] "
                     "[--workdir D] [--build-runs N] [--runs N] "
                     "[--warmups N] [--memory-budget BYTES] "
                     "[--aggregation-interval-us US] INPUT...\n       "
                     "index_bench --smoke\n");
        std::exit(2);
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = parse_args(argc, argv);
    require_ok(dstd::fs::create_directories(opt.workdir), "create_directories");

    dstd::String entries;
    for (std::size_t i = 0; i < opt.inputs.size(); ++i) {
        const dstd::String input(
            dstd::fs::absolute(opt.inputs[i]).value().str());
        const dstd::String part =
            opt.workdir + "/input_" + dstd::to_string(i) + ".json";
        const dstd::String index_dir =
            opt.workdir + "/index_" + dstd::to_string(i);
        // No runtime threads exist before this fork; the child starts its own.
        const pid_t pid = fork();
        if (pid == 0) {
            require_ok(dstd::fs::write_file(
                           part, to_json(measure(opt, input, index_dir))),
                       "write_file");
            std::_Exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::fprintf(stderr, "input failed: %s\n", input.c_str());
            return 1;
        }
        const auto body = dstd::fs::read_file(part);
        require_ok(body, "read_file");
        if (!entries.empty()) entries += ",";
        entries += quote(input) + ":" + *body;
        std::printf("measured %s\n", input.c_str());
    }

    char host[256] = {};
    gethostname(host, sizeof(host) - 1);
    dstd::String doc = "{\"meta\":{\"host\":" + quote(host) +
                       ",\"compiler\":" + quote(__VERSION__) +
#ifdef NDEBUG
                       ",\"build\":\"release\"" +
#else
                       ",\"build\":\"debug\"" +
#endif
                       ",\"runs\":" + dstd::to_string(opt.runs) +
                       ",\"build_runs\":" + dstd::to_string(opt.build_runs) +
                       "},\"inputs\":{" + entries + "}}\n";
    require_ok(dstd::fs::write_file(opt.out, doc), "write_file");
    std::printf("wrote %s\n", opt.out.c_str());

    if (opt.baseline.empty()) return 0;
    simdjson::dom::parser pb, pc;
    simdjson::dom::object base_inputs, cur_inputs;
    if (pb.load(opt.baseline.c_str())["inputs"].get(base_inputs) !=
            simdjson::SUCCESS ||
        pc.parse(doc.data(), doc.size())["inputs"].get(cur_inputs) !=
            simdjson::SUCCESS) {
        std::fprintf(stderr, "cannot read baseline %s\n", opt.baseline.c_str());
        return 1;
    }
    bool regressed = false;
    for (auto [input, cur] : cur_inputs) {
        simdjson::dom::object b, c;
        if (base_inputs[input].get(b) != simdjson::SUCCESS) {
            std::printf("%s: no baseline\n", dstd::String(input).c_str());
            continue;
        }
        if (cur.get(c) != simdjson::SUCCESS) {
            std::printf("%s: no current result\n", dstd::String(input).c_str());
            continue;
        }
        std::printf("%s\n", dstd::String(input).c_str());
        regressed = compare(b, c) || regressed;
    }
    return regressed ? 1 : 0;
}
