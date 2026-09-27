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
//   view_api_bench [events] [workdir] [case-substring]

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/trace/views/typed_rows.h>
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
#include <string_view>
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

// Records whose `blk` changes every 10000 events, so each key sits in a
// few chunks.
std::string write_clustered(const std::string& dir, std::int64_t events) {
    const std::string gz = dir + "/clustered.pfw.gz";
    std::ofstream out(gz, std::ios::binary);
    compress::GzipMemberCompressor comp(6);
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
        chunk += R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1)";
        chunk += R"(,"ts":)" + std::to_string(1000 + i * 10);
        chunk += R"(,"dur":)" + std::to_string(1 + i % 500);
        chunk += R"(,"args":{"blk":)" + std::to_string(i / 10000) + "}}\n";
        if (chunk.size() > (1u << 20)) flush();
    }
    flush();
    return gz;
}

// The same records without a ph, read by path.
std::string write_generic(const std::string& dir, std::int64_t records) {
    const std::string gz = dir + "/bench.ndjson.gz";
    std::ofstream out(gz, std::ios::binary);
    compress::GzipMemberCompressor comp(6);
    const char* ops[] = {"read", "write", "open", "close"};
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
    for (std::int64_t i = 0; i < records; ++i) {
        chunk += R"({"op":")";
        chunk += ops[rng() % 4];
        chunk += R"(","lat":)" + std::to_string(1 + rng() % 500);
        chunk += R"(,"io":{"file":"/data/f)" + std::to_string(rng() % 64);
        chunk += R"(","size":)" + std::to_string(rng() % 65536) + "}}\n";
        if (chunk.size() > (1u << 20)) flush();
    }
    flush();
    return gz;
}

struct EventRow {
    index::Field<std::string, "name"> name;
    index::Field<std::int64_t, "dur"> dur;
    index::Field<std::int64_t, "args.size"> size;
};

struct GenericRow {
    index::Field<std::string, "op"> op;
    index::Field<std::int64_t, "lat"> lat;
    index::Field<std::int64_t, "io.size"> size;
};

std::int64_t int_at(const df::DataFrame& f, std::size_t c, std::int64_t r) {
    const auto& col = f.columns[c];
    if (col.type() == df::TypeId::Uint64)
        return static_cast<std::int64_t>(col.data<std::uint64_t>()[r]);
    if (col.type() == df::TypeId::Float64)
        return static_cast<std::int64_t>(col.data<double>()[r]);
    return col.data<std::int64_t>()[r];
}

// collect() of three columns, then each row read into a struct, as a caller
// of collect() gets its own rows.
template <class Row, class Fill>
std::vector<Row> rows_via_collect(const View& v,
                                  const std::vector<std::string>& names,
                                  Fill fill) {
    const df::DataFrame f = run(v.select(names).collect());
    std::vector<std::size_t> at;
    for (const auto& n : names)
        at.push_back(static_cast<std::size_t>(
            std::find(f.names.begin(), f.names.end(), n) - f.names.begin()));
    std::vector<Row> out(static_cast<std::size_t>(f.num_rows()));
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        fill(f, at, r, out[static_cast<std::size_t>(r)]);
    return out;
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
    run(View::from_file(gz).agg({{AggOp::Count, "", "n"}}).collect());

    const View tv = View::from_file(gz);
    const scan::ScanPlan base = scan::from_file(gz);
    const auto cols = tv.schema();
    auto at = [&](const char* name) {
        return static_cast<std::int32_t>(
            std::find(cols.begin(), cols.end(), name) - cols.begin());
    };

    const std::string generic = write_generic(dir, events);
    run(View::from_file(generic).agg({{AggOp::Count, "", "n"}}).collect());
    const View gv = View::from_file(generic);

    const std::string clustered = write_clustered(dir, events);
    run(View::from_file(clustered).agg({{AggOp::Count, "", "n"}}).collect());
    const View cv = View::from_file(clustered);

    std::vector<Case> cases = {
        {"typed rows vs collect + read",
         [&] {
             rows_via_collect<EventRow>(
                 tv, {"name", "dur", "args.size"},
                 [](const df::DataFrame& f, const std::vector<std::size_t>& at,
                    std::int64_t r, EventRow& row) {
                     row.name.value =
                         std::string(f.columns[at[0]].string_at(r));
                     row.dur.value = int_at(f, at[1], r);
                     row.size.value = int_at(f, at[2], r);
                 });
         },
         [&] { run(rows<EventRow>(tv)); }},
        {"generic typed rows vs collect + read",
         [&] {
             rows_via_collect<GenericRow>(
                 gv, {"op", "lat", "io.size"},
                 [](const df::DataFrame& f, const std::vector<std::size_t>& at,
                    std::int64_t r, GenericRow& row) {
                     row.op.value = std::string(f.columns[at[0]].string_at(r));
                     row.lat.value = int_at(f, at[1], r);
                     row.size.value = int_at(f, at[2], r);
                 });
         },
         [&] { run(rows<GenericRow>(gv)); }},
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
                 scan::agg(scan::group_by(
                               scan::filter(base, duql::parse_or_throw(
                                                      R"(cat == "POSIX")")),
                               {GroupKey::pid()}),
                           {{AggOp::Mean, "dur", "m"}}),
                 "m", 5);
             run(scan::collect_frame(p));
         },
         [&] {
             run(tv.duql(R"(cat == "POSIX")")
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
             scan::ScanPlan p = scan::select(
                 scan::filter(base,
                              duql::parse_or_throw(R"(name == "fwrite")")),
                 {"name", "dur", "args.size"});
             run(scan::collect_frame(p));
         },
         [&] {
             run(tv.duql(R"(name == "fwrite")")
                     .select({"name", "dur", "args.size"})
                     .collect());
         }},
        {"flamegraph",
         [&] {
             scan::ScanPlan p =
                 scan::filter(base, duql::parse_or_throw(R"(cat == "POSIX")"));
             run(scan::flamegraph(p, {"pid", "tid"}, "ts", "dur", "name", {}));
         },
         [&] { run(tv.duql(R"(cat == "POSIX")").flamegraph().collect()); }},
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
        {"duql pipeline vs View calls",
         [&] {
             run(tv.duql(R"(cat == "POSIX")")
                     .with_column(
                         "ms",
                         df::expr_arith(df::ArithOp::Div, df::col(at("dur")),
                                        df::lit(std::int64_t{1000})))
                     .sort_by_multi({"ms"}, std::vector<bool>{true})
                     .head(5)
                     .collect());
         },
         [&] {
             run(tv.duql(R"(where cat == "POSIX" | derive ms = dur / 1000)"
                         " | sort -ms | take 5")
                     .collect());
         }},
        {"duql take vs View calls",
         [&] {
             run(tv.duql(R"(cat == "POSIX")")
                     .with_column("x", df::expr_arith(df::ArithOp::Mul,
                                                      df::col(at("dur")),
                                                      df::lit(std::int64_t{2})))
                     .head(1000)
                     .select({"ts", "dur"})
                     .collect());
         },
         [&] {
             run(tv.duql(R"(where cat == "POSIX" | derive x = dur * 2)"
                         " | take 1000 | select ts, dur")
                     .collect());
         }},
        {"duql group vs View group_by + agg",
         [&] {
             run(tv.group_by({GroupKey::field("name")})
                     .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "t"}})
                     .collect());
         },
         [&] {
             run(tv.duql("group name { n = count(), t = sum(dur) }").collect());
         }},
        {"duql frame group vs LazyFrame calls",
         [&] {
             const View o = tv.duql("derive q = 1");
             run(o.with_column(
                      "k", df::expr_arith(df::ArithOp::Mod, df::col(at("dur")),
                                          df::lit(std::int64_t{7})))
                     .group_by(std::vector<std::string>{"k"},
                               {{df::Agg::Count, "", "n"},
                                {df::Agg::Last, "ts", "l"}})
                     .sort_by_multi({"k"}, std::vector<bool>{false})
                     .collect());
         },
         [&] {
             run(tv.duql("group k = dur % 7 { n = count(), l = last(ts) }")
                     .collect());
         }},
        {"duql window vs frame_op",
         [&] {
             const df::LazyFrame lf =
                 tv.duql("derive q = 1").lazy().with_row_index("__pos");
             const char* part = "name";
             const char* order = "__pos";
             std::vector<dftu_window_spec> specs(2);
             for (dftu_window_spec& s : specs) {
                 s.preceding = DFTU_WINDOW_UNBOUNDED;
                 s.following = DFTU_WINDOW_UNBOUNDED;
             }
             specs[0].func = DFTU_WINDOW_ROW_NUMBER;
             specs[0].out = "r";
             specs[1].func = DFTU_WINDOW_LAG;
             specs[1].value = "dur";
             specs[1].out = "p";
             specs[1].offset = 1;
             df::OpArgs a;
             a.strlist(1, &part, 1).strlist(2, &order, 1).winlist(3, specs);
             std::vector<std::string> names = lf.schema();
             names.emplace_back("r");
             names.emplace_back("p");
             run(lf.frame_op("dftu.frame.window", a, {}, std::move(names))
                     .collect());
         },
         [&] {
             run(tv.duql("window name { r = row_number(), p = lag(dur) }")
                     .collect());
         }},
        {"duql semi-join vs two-step",
         [&] {
             const df::DataFrame keys = run(
                 tv.duql(
                       R"(where name == "fwrite" and dur > 495 | select fname)")
                     .collect());
             std::string list;
             for (std::int64_t r = 0; r < keys.num_rows(); ++r) {
                 const df::Series c = keys.columns[0].materialize();
                 list += (list.empty() ? "\"" : ", \"") +
                         std::string(c.string_at(r)) + "\"";
             }
             run(tv.duql("where fname in [" + list + "]").collect());
         },
         [&] {
             run(tv.duql(R"(where fname in (from data | where name == "fwrite")"
                         R"( and dur > 495 | select fname))")
                     .collect());
         }},
        {"duql pruned semi-join vs full scan",
         [&] {
             run(cv.duql("where (blk in (from data | where blk == 7 | select "
                         "blk)) or false")
                     .collect());
         },
         [&] {
             run(cv.duql("where blk in (from data | where blk == 7 | select "
                         "blk)")
                     .collect());
         }},
        {"duql lookup vs LazyFrame join",
         [&] {
             const df::LazyFrame side =
                 tv.duql("group fname { mx = max(size) }").lazy();
             run(tv.duql(R"(where cat == "STDIO" | select name, f = fname)")
                     .lazy()
                     .join(side, {"f"}, {"fname"}, df::JoinHow::Left)
                     .collect());
         },
         [&] {
             run(tv.duql(
                       R"(let s = from data | group fname { mx = max(size) }; )"
                       R"(where cat == "STDIO" | select name, f = fname | )"
                       R"(lookup s on f == fname)")
                     .collect());
         }},
        {"duql count_distinct vs two group_bys",
         [&] {
             run(tv.duql("select name, f = fname")
                     .lazy()
                     .group_by(std::vector<std::string>{"name", "f"},
                               {{df::Agg::Count, "", "n"}})
                     .group_by(std::vector<std::string>{"name"},
                               {{df::Agg::Count, "", "d"}})
                     .sort_by("name")
                     .collect());
         },
         [&] {
             run(tv.duql("group name { d = count_distinct(fname) }").collect());
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
             run(tv.duql(R"(cat == "STDIO")").collect());
             run(tv.duql("dur > 400").collect());
             run(tv.group_by({GroupKey::pid()})
                     .agg({{AggOp::Max, "dur", "mx"}})
                     .collect());
         },
         [&] {
             run(df::collect_all({tv.group_by({GroupKey::name()})
                                      .agg({{AggOp::Count, "", "n"}})
                                      .lazy(),
                                  tv.duql(R"(cat == "STDIO")").lazy(),
                                  tv.duql("dur > 400").lazy(),
                                  tv.group_by({GroupKey::pid()})
                                      .agg({{AggOp::Max, "dur", "mx"}})
                                      .lazy()}));
         }},
    };

    bool regressed = false;
    std::printf("%-36s %10s %10s %8s %20s\n", "case", "base ms", "view ms",
                "view/base", "95% CI");
    for (const Case& c : cases) {
        if (argc > 3 &&
            std::string_view(c.name).find(argv[3]) == std::string_view::npos)
            continue;
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
