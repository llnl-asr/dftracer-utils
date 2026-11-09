// SPDX-License-Identifier: MIT
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

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/function_ref.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/views/typed_rows.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

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

duql::Query parsed_query(dstd::StringView text) {
    auto parsed = duql::Query::from_string(text);
    if (!parsed) throw duql::DuqlParseError(parsed.error());
    return std::move(*parsed);
}

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

constexpr int WARMUPS = 5;
constexpr int RUNS = 20;
constexpr double MAX_SLOWDOWN = 0.05;

template <class T>
T run(coro::CoroTask<T> t) {
    return common::unwrap(default_runtime().submit(std::move(t)).get());
}

// A multi-member trace: POSIX/STDIO reads and writes across pids with a
// string and a numeric arg, one gzip member per ~1 MB of lines.
dstd::String write_trace(const dstd::String& dir, std::int64_t events) {
    const dstd::String gz = dir + "/bench.pfw.gz";
    require_ok(dstd::fs::write_file(gz, {}), "write_file");
    utilities::fileio::compress::GzipMemberCompressor comp(6);
    const char* names[] = {"read", "write", "open", "close", "fread", "fwrite"};
    const char* cats[] = {"POSIX", "POSIX", "POSIX", "POSIX", "STDIO", "STDIO"};
    std::mt19937_64 rng(7);
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
    return gz;
}

// Records whose `blk` changes every 10000 events, so each key sits in a
// few chunks.
dstd::String write_clustered(const dstd::String& dir, std::int64_t events) {
    const dstd::String gz = dir + "/clustered.pfw.gz";
    require_ok(dstd::fs::write_file(gz, {}), "write_file");
    utilities::fileio::compress::GzipMemberCompressor comp(6);
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
        chunk += R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1)";
        chunk += R"(,"ts":)" + dstd::to_string(1000 + i * 10);
        chunk += R"(,"dur":)" + dstd::to_string(1 + i % 500);
        chunk += R"(,"args":{"blk":)" + dstd::to_string(i / 10000) + "}}\n";
        if (chunk.size() > (1u << 20)) flush();
    }
    flush();
    return gz;
}

// The same records without a ph, read by path.
dstd::String write_generic(const dstd::String& dir, std::int64_t records) {
    const dstd::String gz = dir + "/bench.ndjson.gz";
    require_ok(dstd::fs::write_file(gz, {}), "write_file");
    utilities::fileio::compress::GzipMemberCompressor comp(6);
    const char* ops[] = {"read", "write", "open", "close"};
    std::mt19937_64 rng(7);
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
    for (std::int64_t i = 0; i < records; ++i) {
        chunk += R"({"op":")";
        chunk += ops[rng() % 4];
        chunk += R"(","lat":)" + dstd::to_string(1 + rng() % 500);
        chunk += R"(,"io":{"file":"/data/f)" + dstd::to_string(rng() % 64);
        chunk += R"(","size":)" + dstd::to_string(rng() % 65536) + "}}\n";
        if (chunk.size() > (1u << 20)) flush();
    }
    flush();
    return gz;
}

struct EventRow {
    index::Field<dstd::String, "name"> name;
    index::Field<std::int64_t, "dur"> dur;
    index::Field<std::int64_t, "args.size"> size;
};

struct GenericRow {
    index::Field<dstd::String, "op"> op;
    index::Field<std::int64_t, "lat"> lat;
    index::Field<std::int64_t, "io.size"> size;
};

std::int64_t int_at(const dataframe::DataFrame& f, std::size_t c,
                    std::int64_t r) {
    const auto& col = f.columns[c];
    if (col.type() == dataframe::TypeId::Uint64)
        return static_cast<std::int64_t>(col.data<std::uint64_t>()[r]);
    if (col.type() == dataframe::TypeId::Float64)
        return static_cast<std::int64_t>(col.data<double>()[r]);
    return col.data<std::int64_t>()[r];
}

// collect() of three columns, then each row read into a struct, as a caller
// of collect() gets its own rows.
template <class Row, class Fill>
dstd::Vector<Row> rows_via_collect(const View& v,
                                   const dstd::Vector<dstd::String>& names,
                                   Fill fill) {
    dataframe::DataFrame f = run(v.select(names).collect());
    for (auto& c : f.columns)
        if (c.type() != dataframe::TypeId::String) c = c.materialize();
    dstd::Vector<std::size_t> at;
    for (const auto& n : names)
        at.push_back(static_cast<std::size_t>(
            std::find(f.names.begin(), f.names.end(), n) - f.names.begin()));
    dstd::Vector<Row> out(static_cast<std::size_t>(f.num_rows()));
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        fill(f, at, r, out[static_cast<std::size_t>(r)]);
    return out;
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

// 95% bootstrap interval of median(view) / median(base).
dstd::Pair<double, double> ratio_ci(const dstd::Vector<double>& base_t,
                                    const dstd::Vector<double>& view_t) {
    std::mt19937_64 rng(11);
    std::uniform_int_distribution<std::size_t> pick(0, base_t.size() - 1);
    dstd::Vector<double> ratios;
    for (int b = 0; b < 2000; ++b) {
        dstd::Vector<double> o, n;
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
    dstd::Function<void()> baseline;
    dstd::Function<void()> view;
};

}  // namespace

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && dstd::StringView(argv[1]) == "--smoke";
    const std::int64_t events =
        smoke ? 20'000 : (argc > 1 ? std::atoll(argv[1]) : 2'000'000);
    const dstd::String dir =
        !smoke && argc > 2 ? argv[2]
                           : dstd::String(dstd::fs::temp_directory().str()) +
                                 (smoke ? "/view_bench_smoke" : "/view_bench");
    require_ok(dstd::fs::create_directories(dir), "create_directories");
    const dstd::String gz = write_trace(dir, events);
    std::printf("trace: %s (%lld events)\n", gz.c_str(),
                static_cast<long long>(events));

    // First touch builds the index; keep it out of the timings.
    run(View::from_file(gz).agg({{AggOp::Count, "", "n"}}).collect());

    const View tv = View::from_file(gz);
    const trace::views::detail::scan::ScanPlan base =
        trace::views::detail::scan::from_file(gz);
    const auto cols = tv.schema();
    auto at = [&](const char* name) {
        return static_cast<std::int32_t>(
            std::find(cols.begin(), cols.end(), name) - cols.begin());
    };

    const dstd::String generic = write_generic(dir, events);
    run(View::from_file(generic).agg({{AggOp::Count, "", "n"}}).collect());
    const View gv = View::from_file(generic);

    const dstd::String clustered = write_clustered(dir, events);
    run(View::from_file(clustered).agg({{AggOp::Count, "", "n"}}).collect());
    const View cv = View::from_file(clustered);

    dstd::Vector<Case> cases;
    auto add = [&cases](const char* name, dstd::Function<void()> baseline,
                        dstd::Function<void()> view) {
        cases.push_back(Case{name, std::move(baseline), std::move(view)});
    };
    add(
        "typed rows vs collect + read",
        [&] {
            rows_via_collect<EventRow>(
                tv, {"name", "dur", "args.size"},
                [](const dataframe::DataFrame& f,
                   const dstd::Vector<std::size_t>& col_idx, std::int64_t r,
                   EventRow& row) {
                    row.name.value =
                        dstd::String(f.columns[col_idx[0]].string_at(r));
                    row.dur.value = int_at(f, col_idx[1], r);
                    row.size.value = int_at(f, col_idx[2], r);
                });
        },
        [&] { run(rows<EventRow>(tv)); });
    add(
        "generic typed rows vs collect + read",
        [&] {
            rows_via_collect<GenericRow>(
                gv, {"op", "lat", "io.size"},
                [](const dataframe::DataFrame& f,
                   const dstd::Vector<std::size_t>& col_idx, std::int64_t r,
                   GenericRow& row) {
                    row.op.value =
                        dstd::String(f.columns[col_idx[0]].string_at(r));
                    row.lat.value = int_at(f, col_idx[1], r);
                    row.size.value = int_at(f, col_idx[2], r);
                });
        },
        [&] { run(rows<GenericRow>(gv)); });
    add(
        "agg name: count, sum dur",
        [&] {
            trace::views::detail::scan::ScanPlan p =
                trace::views::detail::scan::agg(
                    trace::views::detail::scan::group_by(base,
                                                         {GroupKey::name()}),
                    {{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "s"}});
            run(trace::views::detail::scan::collect_frame(p));
        },
        [&] {
            run(tv.group_by({GroupKey::name()})
                    .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "s"}})
                    .collect());
        });
    add(
        "filter + agg pid, top 5",
        [&] {
            trace::views::detail::scan::ScanPlan p =
                trace::views::detail::scan::topk(
                    trace::views::detail::scan::agg(
                        trace::views::detail::scan::group_by(
                            trace::views::detail::scan::filter(
                                base, parsed_query(R"(cat == "POSIX")")),
                            {GroupKey::pid()}),
                        {{AggOp::Mean, "dur", "m"}}),
                    "m", 5);
            run(trace::views::detail::scan::collect_frame(p));
        },
        [&] {
            run(tv.duql(R"(cat == "POSIX")")
                    .group_by({GroupKey::pid()})
                    .agg({{AggOp::Mean, "dur", "m"}})
                    .sort_by("m", true)
                    .head(5)
                    .collect());
        });
    add(
        "time bucket 10ms",
        [&] {
            trace::views::detail::scan::ScanPlan p =
                trace::views::detail::scan::agg(
                    trace::views::detail::scan::group_by(
                        trace::views::detail::scan::time_bucket(base, 10000),
                        {GroupKey::cat()}),
                    {{AggOp::Count, "", "n"}});
            run(trace::views::detail::scan::collect_frame(p));
        },
        [&] {
            run(tv.time_bucket(10000)
                    .group_by({GroupKey::cat()})
                    .agg({{AggOp::Count, "", "n"}})
                    .collect());
        });
    add(
        "row query, select",
        [&] {
            trace::views::detail::scan::ScanPlan p =
                trace::views::detail::scan::select(
                    trace::views::detail::scan::filter(
                        base, parsed_query(R"(name == "fwrite")")),
                    {"name", "dur", "args.size"});
            run(trace::views::detail::scan::collect_frame(p));
        },
        [&] {
            run(tv.duql(R"(name == "fwrite")")
                    .select({"name", "dur", "args.size"})
                    .collect());
        });
    add(
        "flamegraph",
        [&] {
            trace::views::detail::scan::ScanPlan p =
                trace::views::detail::scan::filter(
                    base, parsed_query(R"(cat == "POSIX")"));
            run(trace::views::detail::scan::flamegraph(p, {"pid", "tid"}, "ts",
                                                       "dur", "name", {}));
        },
        [&] { run(tv.duql(R"(cat == "POSIX")").flamegraph().collect()); });
    add(
        "generic group_by (absorbed)",
        [&] {
            trace::views::detail::scan::ScanPlan p =
                trace::views::detail::scan::agg(
                    trace::views::detail::scan::group_by(base,
                                                         {GroupKey::name()}),
                    {{AggOp::Sum, "dur", "s"}});
            run(trace::views::detail::scan::collect_frame(p));
        },
        [&] {
            run(tv.group_by(dstd::Vector<dstd::String>{"name"},
                            {{dataframe::Agg::Sum, "dur", "s"}})
                    .collect());
        });
    add(
        "expression key vs engine over rows",
        [&] {
            dataframe::LazyFrame lf = trace::views::detail::scan::collect(base);
            run(lf.with_column("big",
                               dataframe::col(at("dur")) > std::int64_t{250})
                    .group_by(dstd::Vector<dstd::String>{"big"},
                              {{dataframe::Agg::Count, "", "n"}})
                    .collect());
        },
        [&] {
            run(tv.with_column("big",
                               dataframe::col(at("dur")) > std::int64_t{250})
                    .group_by(dstd::Vector<dstd::String>{"big"},
                              {{dataframe::Agg::Count, "", "n"}})
                    .collect());
        });
    add(
        "duql pipeline vs View calls",
        [&] {
            run(tv.duql(R"(cat == "POSIX")")
                    .with_column("ms", dataframe::expr_arith(
                                           dataframe::ArithOp::Div,
                                           dataframe::col(at("dur")),
                                           dataframe::lit(std::int64_t{1000})))
                    .sort_by_multi({"ms"}, dstd::Vector<bool>{true})
                    .head(5)
                    .collect());
        },
        [&] {
            run(tv.duql(R"(where cat == "POSIX" | derive ms = dur / 1000)"
                        " | sort -ms | take 5")
                    .collect());
        });
    add(
        "duql take vs View calls",
        [&] {
            run(tv.duql(R"(cat == "POSIX")")
                    .with_column("x", dataframe::expr_arith(
                                          dataframe::ArithOp::Mul,
                                          dataframe::col(at("dur")),
                                          dataframe::lit(std::int64_t{2})))
                    .head(1000)
                    .select({"ts", "dur"})
                    .collect());
        },
        [&] {
            run(tv.duql(R"(where cat == "POSIX" | derive x = dur * 2)"
                        " | take 1000 | select ts, dur")
                    .collect());
        });
    add(
        "duql group vs View group_by + agg",
        [&] {
            run(tv.group_by({GroupKey::field("name")})
                    .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "t"}})
                    .collect());
        },
        [&] {
            run(tv.duql("group name { n = count(), t = sum(dur) }").collect());
        });
    add(
        "duql frame group vs LazyFrame calls",
        [&] {
            const View o = tv.duql("derive q = 1");
            run(o.with_column("k", dataframe::expr_arith(
                                       dataframe::ArithOp::Mod,
                                       dataframe::col(at("dur")),
                                       dataframe::lit(std::int64_t{7})))
                    .group_by(dstd::Vector<dstd::String>{"k"},
                              {{dataframe::Agg::Count, "", "n"},
                               {dataframe::Agg::Last, "ts", "l"}})
                    .sort_by_multi({"k"}, dstd::Vector<bool>{false})
                    .collect());
        },
        [&] {
            run(tv.duql("group k = dur % 7 { n = count(), l = last(ts) }")
                    .collect());
        });
    add(
        "duql window vs frame_op",
        [&] {
            const dataframe::LazyFrame lf =
                tv.duql("derive q = 1").lazy().with_row_index("__pos");
            const char* part = "name";
            const char* order = "__pos";
            dstd::Vector<dftu_window_spec> specs(2);
            for (dftu_window_spec& s : specs) {
                s.param.frame.preceding = DFTU_WINDOW_UNBOUNDED;
                s.param.frame.following = DFTU_WINDOW_UNBOUNDED;
            }
            specs[0].func = DFTU_WINDOW_ROW_NUMBER;
            specs[0].out = "r";
            specs[1].func = DFTU_WINDOW_LAG;
            specs[1].value = "dur";
            specs[1].out = "p";
            specs[1].param.offset = 1;
            dataframe::OpArgs a;
            a.strlist(1, &part, 1).strlist(2, &order, 1).winlist(3, specs);
            dstd::Vector<dstd::String> names = lf.schema();
            names.emplace_back("r");
            names.emplace_back("p");
            run(lf.frame_op("dftu.frame.window", a, {}, std::move(names))
                    .collect());
        },
        [&] {
            run(tv.duql("window name { r = row_number(), p = lag(dur) }")
                    .collect());
        });
    add(
        "duql semi-join vs two-step",
        [&] {
            const dataframe::DataFrame keys = run(
                tv.duql(
                      R"(where name == "fwrite" and dur > 495 | select fname)")
                    .collect());
            dstd::String list;
            const dataframe::Series c = keys.columns[0].materialize();
            for (std::int64_t r = 0; r < keys.num_rows(); ++r) {
                list += (list.empty() ? "\"" : ", \"") +
                        dstd::String(c.string_at(r)) + "\"";
            }
            run(tv.duql("where fname in [" + list + "]").collect());
        },
        [&] {
            run(tv.duql(R"(where fname in (from data | where name == "fwrite")"
                        R"( and dur > 495 | select fname))")
                    .collect());
        });
    add(
        "duql pruned semi-join vs full scan",
        [&] {
            run(cv.duql("where (blk in (from data | where blk == 7 | select "
                        "blk)) or false")
                    .collect());
        },
        [&] {
            run(cv.duql("where blk in (from data | where blk == 7 | select "
                        "blk)")
                    .collect());
        });
    add(
        "duql lookup vs LazyFrame join",
        [&] {
            const dataframe::LazyFrame side =
                tv.duql("group fname { mx = max(size) }").lazy();
            run(tv.duql(R"(where cat == "STDIO" | select name, f = fname)")
                    .lazy()
                    .join(side, {"f"}, {"fname"}, dataframe::JoinHow::Left)
                    .collect());
        },
        [&] {
            run(tv.duql(
                      R"(let s = from data | group fname { mx = max(size) }; )"
                      R"(where cat == "STDIO" | select name, f = fname | )"
                      R"(lookup s on f == fname)")
                    .collect());
        });
    add(
        "duql count_distinct vs two group_bys",
        [&] {
            run(tv.duql("select name, f = fname")
                    .lazy()
                    .group_by(dstd::Vector<dstd::String>{"name", "f"},
                              {{dataframe::Agg::Count, "", "n"}})
                    .group_by(dstd::Vector<dstd::String>{"name"},
                              {{dataframe::Agg::Count, "", "d"}})
                    .sort_by("name")
                    .collect());
        },
        [&] {
            run(tv.duql("group name { d = count_distinct(fname) }").collect());
        });
    add(
        "session vs collect_all: tree + agg",
        [&] {
            TraceSession s = tv.session();
            auto both = s.collect(tv.containment());
            auto by_name = s.collect(tv.group_by({GroupKey::name()})
                                         .agg({{AggOp::Count, "", "n"}})
                                         .lazy());
            run(s.execute());
        },
        [&] {
            run(dataframe::collect_all(tv.containment(),
                                       tv.group_by({GroupKey::name()})
                                           .agg({{AggOp::Count, "", "n"}})));
        });
    add(
        "4 plans: separate vs collect_all",
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
            run(dataframe::collect_all({tv.group_by({GroupKey::name()})
                                            .agg({{AggOp::Count, "", "n"}})
                                            .lazy(),
                                        tv.duql(R"(cat == "STDIO")").lazy(),
                                        tv.duql("dur > 400").lazy(),
                                        tv.group_by({GroupKey::pid()})
                                            .agg({{AggOp::Max, "dur", "mx"}})
                                            .lazy()}));
        });

    bool regressed = false;
    std::printf("%-36s %10s %10s %8s %20s\n", "case", "base ms", "view ms",
                "view/base", "95% CI");
    for (const Case& c : cases) {
        if (!smoke && argc > 3 &&
            dstd::StringView(c.name).find(argv[3]) == dstd::StringView::npos)
            continue;
        for (int i = 0; i < (smoke ? 0 : WARMUPS); ++i) {
            c.baseline();
            c.view();
        }
        dstd::Vector<double> base_t, view_t;
        for (int i = 0; i < (smoke ? 3 : RUNS); ++i) {
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
    return regressed && !smoke ? 1 : 0;
}
