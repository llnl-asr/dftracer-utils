// SPDX-License-Identifier: MIT
// Benchmark: several row-heavy plans over one trace set, collected three
// ways. Each way runs in its own child process, so the peak resident set of
// one way never carries over to the next.
//
//   one-by-one   plan.collect() for each plan in turn
//   collect_all  dataframe::collect_all over every plan
//   session      TraceSession::collect for every plan, then execute()
//
// The parent writes the files and builds the index once, then spawns the
// children, rotating the order of the three ways each round. It prints the
// median wall time and the peak resident set of each way, and the rows and an
// order-free checksum of each plan, which must agree across the ways.
//
// Not a unit test (too slow for CI): built only with
// DFTRACER_UTILS_BUILD_BENCHMARKS=ON and run manually.
//
//   view_batch_bench [--smoke] [--files N] [--events N] [--rounds N]
//                    [--shape base|ordered|leading_select|derive|mixed] [--dir
//                    D]

#include <bench_counters.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;

namespace {

constexpr int ROUTE_COUNT = 3;
const char* const ROUTES[ROUTE_COUNT] = {"one_by_one", "collect_all",
                                         "session"};

template <class T>
void require_ok(const dstd::Expected<T, dstd::Error>& result,
                const char* what) {
    if (result.has_value()) return;
    const auto& error = result.error();
    std::fprintf(stderr, "%s failed: %.*s %.*s\n", what,
                 static_cast<int>(error.name().size()), error.name().data(),
                 static_cast<int>(error.message().size()),
                 error.message().data());
    std::exit(1);
}

template <class T>
T run(coro::CoroTask<T> t) {
    auto result = default_runtime().submit(std::move(t)).get();
    require_ok(result, "run");
    if constexpr (!std::is_void_v<T>) return std::move(*result);
}

template <class T>
T run(coro::CoroTask<dstd::Result<T>> t) {
    auto result = default_runtime().submit(std::move(t)).get();
    require_ok(result, "run");
    if constexpr (!std::is_void_v<T>) return std::move(*result);
}

// POSIX/STDIO reads and writes across pids with a string and a numeric arg,
// one gzip member per ~1 MB of lines.
void write_trace(const dstd::String& gz, std::int64_t events,
                 std::uint64_t seed) {
    require_ok(dstd::fs::write_file(gz, {}), "write_file");
    utilities::fileio::compress::GzipMemberCompressor comp(6);
    const char* names[] = {"read", "write", "open", "close", "fread", "fwrite"};
    const char* cats[] = {"POSIX", "POSIX", "POSIX", "POSIX", "STDIO", "STDIO"};
    std::mt19937_64 rng(seed);
    dstd::String chunk;
    auto flush = [&] {
        if (chunk.empty()) return;
        auto m = comp.compress_member(
            reinterpret_cast<const std::uint8_t*>(chunk.data()), chunk.size());
        if (!m) {
            std::fprintf(stderr, "compress failed\n");
            std::exit(1);
        }
        require_ok(
            dstd::fs::append_file(
                gz, dstd::StringView(reinterpret_cast<const char*>(m->data()),
                                     m->size())),
            "append_file");
        chunk.clear();
    };
    for (std::int64_t i = 0; i < events; ++i) {
        const std::size_t k = rng() % 6;
        chunk += R"({"ph":"X","name":")";
        chunk += names[k];
        chunk += R"(","cat":")";
        chunk += cats[k];
        chunk += R"(","pid":)" + dstd::to_string(rng() % 16);
        chunk += R"(,"tid":)" + dstd::to_string(rng() % 4);
        chunk += R"(,"ts":)" + dstd::to_string(1000 + i * 10);
        chunk += R"(,"dur":)" + dstd::to_string(1 + rng() % 500);
        chunk += R"(,"args":{"fname":"/data/f)" + dstd::to_string(rng() % 64);
        chunk += R"(","size":)" + dstd::to_string(rng() % 65536) + "}}\n";
        if (chunk.size() > (1u << 20)) flush();
    }
    flush();
}

struct Setup {
    int files = 32;
    std::int64_t events = 45'000;
    int rounds = 5;
    dstd::String shape = "base";
    dstd::String dir;
};

View open_view(const Setup& s) {
    dstd::Vector<ViewFile> files;
    for (int i = 0; i < s.files; ++i) {
        ViewFile f;
        f.file_path =
            dstd::String(s.dir.c_str()) + "/t" + dstd::to_string(i) + ".pfw.gz";
        f.index_path = dstd::String(s.dir.c_str()) + "/index/t" +
                       dstd::to_string(i) + ".idx";
        files.push_back(std::move(f));
    }
    return View::from_files(std::move(files));
}

// Five row-heavy plans of different shapes. `shape` varies the same five.
dstd::Vector<dataframe::LazyFrame> make_plans(const View& tv,
                                              const dstd::String& shape) {
    dstd::Vector<dataframe::LazyFrame> p;
    if (shape == "leading_select") {
        p.push_back(tv.duql("select ts, dur, name, cat").lazy());
        p.push_back(
            tv.duql(R"(select name, cat, dur | where cat == "POSIX")").lazy());
        p.push_back(tv.duql("select name, dur | where dur > 400").lazy());
        p.push_back(
            tv.duql("select ts, dur, name, pid | where dur < 100").lazy());
        p.push_back(
            tv.duql(R"(select name, ts | where name == "fwrite")").lazy());
        return p;
    }
    if (shape == "mixed") {
        p.push_back(tv.lazy());
        p.push_back(tv.duql(R"(cat == "POSIX")").lazy());
        p.push_back(tv.duql("dur > 400").lazy());
        p.push_back(tv.duql("select name, dur, ts | where dur > 100").lazy());
        p.push_back(tv.duql("derive ms = dur / 1000 | where ms < 0.4").lazy());
        return p;
    }
    if (shape == "derive") {
        p.push_back(tv.duql("derive ms = dur / 1000").lazy());
        p.push_back(
            tv.duql(R"(where cat == "POSIX" | derive ms = dur / 1000)").lazy());
        p.push_back(tv.duql("where dur > 400 | derive ms = dur / 1000").lazy());
        p.push_back(
            tv.duql("derive ms = dur / 1000 | where ms < 0.1 | select ts, ms")
                .lazy());
        p.push_back(
            tv.duql(R"(where name == "fwrite" | derive ms = dur / 1000)")
                .lazy());
        return p;
    }
    p.push_back(tv.lazy());
    p.push_back(tv.duql(R"(cat == "POSIX")").lazy());
    p.push_back(tv.duql("dur > 400").lazy());
    p.push_back(tv.duql("dur < 100").select({"ts", "dur", "name", "pid"}));
    p.push_back(tv.duql(R"(name == "fwrite")").lazy());
    if (shape == "ordered") {
        for (auto& plan : p) plan = plan.sort_by("dur");
    }
    return p;
}

std::uint64_t mix(std::uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

// Sum over rows of a hash of the row's values in column-name order, so the
// result does not depend on row order.
std::uint64_t checksum(const dataframe::DataFrame& f,
                       std::uint64_t* ordered_sum = nullptr) {
    dstd::Vector<std::size_t> order(f.columns.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return dstd::String(f.names[a].c_str()) <
               dstd::String(f.names[b].c_str());
    });
    dstd::Vector<dataframe::Series> cols;
    std::uint64_t names_hash = 0;
    for (std::size_t i : order) {
        cols.push_back(f.columns[i].materialize());
        for (char c : dstd::String(f.names[i].c_str()))
            names_hash = mix(names_hash * 131 + static_cast<unsigned char>(c));
    }
    std::uint64_t sum = names_hash;
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        std::uint64_t h = 0;
        for (const auto& c : cols) {
            std::uint64_t v = 0;
            switch (c.type()) {
                case dataframe::TypeId::Int64:
                    v = static_cast<std::uint64_t>(c.data<std::int64_t>()[r]);
                    break;
                case dataframe::TypeId::Uint64:
                    v = c.data<std::uint64_t>()[r];
                    break;
                case dataframe::TypeId::Float64: {
                    const double d = c.data<double>()[r];
                    std::memcpy(&v, &d, sizeof v);
                    break;
                }
                case dataframe::TypeId::String:
                    for (char ch : dstd::String(c.string_at(r)))
                        v = v * 131 + static_cast<unsigned char>(ch);
                    break;
                default:
                    v = static_cast<std::uint64_t>(c.type());
            }
            h = mix(h * 1000003 + v);
        }
        sum += h;
        if (ordered_sum) *ordered_sum = mix(*ordered_sum * 1000003 + h);
    }
    return sum;
}

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Child: runs one way and prints
// "RESULT <ms> <rss_before> <peak> <rows0> <sum0> ...".
int child(const Setup& s, const dstd::String& route) {
    const View tv = open_view(s);
    dstd::Vector<dataframe::LazyFrame> plans = make_plans(tv, s.shape);
    const std::uint64_t before = dftu::bench::peak_heap_bytes();
    dstd::Vector<dataframe::DataFrame> out;
    const double t0 = now_ms();
    if (route == "one_by_one") {
        for (const auto& plan : plans) out.push_back(run(plan.collect()));
    } else if (route == "collect_all") {
        out = run(dataframe::collect_all(plans));
    } else {
        TraceSession sess = tv.session();
        dstd::Vector<Deferred<dataframe::DataFrame>> handles;
        for (const auto& plan : plans) handles.push_back(sess.collect(plan));
        run(sess.execute());
        for (auto& h : handles) out.push_back(std::move(h.get()));
    }
    const double ms = now_ms() - t0;
    const std::uint64_t peak = dftu::bench::peak_heap_bytes();
    std::printf("RESULT %.3f %llu %llu", ms,
                static_cast<unsigned long long>(before),
                static_cast<unsigned long long>(peak));
    for (const auto& f : out) {
        std::uint64_t seq = 0;
        const std::uint64_t sum = checksum(f, &seq);
        std::printf(" %lld %llu %llu", static_cast<long long>(f.num_rows()),
                    static_cast<unsigned long long>(sum),
                    static_cast<unsigned long long>(seq));
    }
    std::printf("\n");
    return 0;
}

struct Sample {
    double ms = 0;
    double before_mb = 0;
    double peak_mb = 0;
    dstd::Vector<dstd::String> rows_sum;
    dstd::Vector<dstd::String> seq;
};

Sample spawn(const dstd::String& self, const Setup& s,
             const dstd::String& route) {
    const dstd::String cmd = "'" + self + "' --child " + route + " --files " +
                             dstd::to_string(s.files) + " --shape " + s.shape +
                             " --dir '" + s.dir + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        std::fprintf(stderr, "popen failed\n");
        std::exit(1);
    }
    char line[4096];
    Sample out;
    bool got = false;
    while (std::fgets(line, sizeof line, pipe)) {
        if (std::strncmp(line, "RESULT ", 7) != 0) continue;
        got = true;
        dstd::Vector<dstd::String> tok;
        dstd::String cur;
        for (const char* c = line + 7; *c; ++c) {
            if (*c == ' ' || *c == '\n') {
                if (!cur.empty()) tok.push_back(cur);
                cur.clear();
            } else {
                cur += *c;
            }
        }
        out.ms = std::atof(tok[0].c_str());
        out.before_mb = std::atof(tok[1].c_str()) / 1048576.0;
        out.peak_mb = std::atof(tok[2].c_str()) / 1048576.0;
        for (std::size_t i = 3; i + 2 < tok.size(); i += 3) {
            out.rows_sum.push_back(tok[i] + ":" + tok[i + 1]);
            out.seq.push_back(tok[i + 2]);
        }
    }
    if (pclose(pipe) != 0 || !got) {
        std::fprintf(stderr, "child failed: %s\n", cmd.c_str());
        std::exit(1);
    }
    return out;
}

double median(dstd::Vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    Setup s;
    dstd::String child_route;
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        const dstd::String a = argv[i];
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--smoke") {
            smoke = true;
        } else if (a == "--child") {
            child_route = next();
        } else if (a == "--files") {
            s.files = std::atoi(next());
        } else if (a == "--events") {
            s.events = std::atoll(next());
        } else if (a == "--rounds") {
            s.rounds = std::atoi(next());
        } else if (a == "--shape") {
            s.shape = next();
        } else if (a == "--dir") {
            s.dir = next();
        }
    }
    if (smoke) {
        s.files = 4;
        s.events = 2'000;
        s.rounds = 1;
    }
    if (s.dir.empty())
        s.dir = dstd::String(
                    dstd::String(dstd::fs::temp_directory().str()).c_str()) +
                (smoke ? "/view_batch_smoke" : "/view_batch");
    if (!child_route.empty()) return child(s, child_route);

    require_ok(
        dstd::fs::create_directories(dstd::String(s.dir.c_str()) + "/index"),
        "create_directories");
    for (int i = 0; i < s.files; ++i)
        write_trace(
            dstd::String(s.dir.c_str()) + "/t" + dstd::to_string(i) + ".pfw.gz",
            s.events, 7 + static_cast<std::uint64_t>(i));
    std::printf("files: %d x %lld events in %s, shape %s\n", s.files,
                static_cast<long long>(s.events), s.dir.c_str(),
                s.shape.c_str());

    // First touch builds the indexes; keep it out of the timings.
    run(open_view(s).agg({{AggOp::Count, "", "n"}}).collect());

    dstd::Vector<dstd::Vector<double>> ms(ROUTE_COUNT), peak(ROUTE_COUNT),
        before(ROUTE_COUNT);
    dstd::Vector<dstd::String> reference, ref_seq;
    bool equal = true;
    for (int round = 0; round < s.rounds; ++round) {
        for (int k = 0; k < ROUTE_COUNT; ++k) {
            const int route = (round + k) % ROUTE_COUNT;
            const Sample r = spawn(argv[0], s, ROUTES[route]);
            ms[route].push_back(r.ms);
            peak[route].push_back(r.peak_mb);
            before[route].push_back(r.before_mb);
            if (reference.empty()) {
                reference = r.rows_sum;
                ref_seq = r.seq;
            }
            // Plans that run a stage after the scan promise file order.
            const std::size_t first_ordered =
                s.shape == "leading_select" ? 1 : (s.shape == "mixed" ? 3 : 0);
            if (s.shape == "leading_select" || s.shape == "derive" ||
                s.shape == "mixed")
                for (std::size_t i = first_ordered; i < r.seq.size(); ++i)
                    if (r.seq[i] != ref_seq[i]) {
                        equal = false;
                        std::printf("ORDER differs in %s plan %zu round %d\n",
                                    ROUTES[route], i, round);
                    }
            if (r.rows_sum != reference) {
                equal = false;
                std::printf("MISMATCH in %s round %d\n", ROUTES[route], round);
            }
        }
    }
    std::printf("%-12s %10s %12s %12s\n", "route", "median ms", "peak MB",
                "before MB");
    for (int route = 0; route < ROUTE_COUNT; ++route)
        std::printf("%-12s %10.1f %12.1f %12.1f\n", ROUTES[route],
                    median(ms[route]), median(peak[route]),
                    median(before[route]));
    for (std::size_t i = 0; i < reference.size(); ++i)
        std::printf("plan %zu rows:checksum %s\n", i, reference[i].c_str());
    std::printf("routes %s\n", equal ? "equal" : "DIFFER");
    return equal ? 0 : 1;
}
