#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schemas/dft/agg/view_agg_tier.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace df = dftracer::utils::dataframe;
namespace ix = dftracer::utils::index;

namespace {

// `lines` as a gzip of members of about `member_bytes`, indexed with small
// checkpoints so each member is its own chunk.
std::string write_gz(TestEnvironment& env, const std::string& name,
                     const std::vector<std::string>& lines,
                     std::size_t member_bytes = 256) {
    const std::string plain = env.get_dir() + "/" + name + ".ndjson";
    {
        std::ofstream o(plain);
        for (const auto& l : lines) o << l << "\n";
    }
    const std::string gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               member_bytes));
    fs::remove(plain);
    return gz;
}

std::string write_indexed(TestEnvironment& env, const std::string& name,
                          const std::vector<std::string>& lines,
                          std::size_t member_bytes = 256,
                          const std::string& schema = "",
                          std::uint64_t tier_interval_us = 0) {
    const std::string gz = write_gz(env, name, lines, member_bytes);
    ix::IndexerOptions o;
    o.checkpoint_size = member_bytes;
    o.schema = schema;
    if (tier_interval_us > 0)
        o.aggregation.emplace().time_interval_us = tier_interval_us;
    ix::Indexer::open({gz}, o).build();
    return gz;
}

constexpr const char* DFT_ROLES = R"(
id: dft_roles
fields:
  ts: {type: int, role: time}
  dur: {type: int, role: duration}
  pid: {type: int, role: entity}
  tid: {type: int, role: lane}
  name: {type: string, role: name}
  cat: {type: string}
)";

// A dftracer event line, as the records of the dft_roles schema too.
std::string dft_line(int pid, int tid, const char* name, std::int64_t ts,
                     std::int64_t dur) {
    return R"({"id":1,"name":")" + std::string(name) +
           R"(","cat":"POSIX","pid":)" + std::to_string(pid) + R"(,"tid":)" +
           std::to_string(tid) + R"(,"ts":)" + std::to_string(ts) +
           R"(,"dur":)" + std::to_string(dur) +
           R"(,"ph":"X","args":{"size":7}})";
}

namespace scan = dftracer::utils::trace::views::detail::scan;

// The aggregation of `p` by a full scan, the tier bypassed.
df::DataFrame scan_only(const scan::ScanPlan& p) {
    namespace detail = dftracer::utils::trace::views::detail;
    Runtime rt;
    df::DataFrame out;
    rt.run_blocking("scan-only", [&](CoroScope&) -> coro::CoroTask<void> {
        auto st = co_await detail::build_engine_agg_state(*p);
        out = detail::finalize_engine_result(*st, *p);
    });
    return detail::apply_agg_post_ops(std::move(out), *p);
}

// The aggregation of `p` from the aggregation tier; fails when the tier
// declines it.
df::DataFrame tier_only(const scan::ScanPlan& p) {
    namespace detail = dftracer::utils::trace::views::detail;
    detail::ensure_schema(*p);
    df::AggStatePtr st;
    REQUIRE(
        dftracer::utils::index::schemas::dft::agg::agg_tier_collect(*p, st));
    return detail::apply_agg_post_ops(detail::finalize_engine_result(*st, *p),
                                      *p);
}

// Each row of `f` as its cells' text, keyed by the `keys` columns' text.
std::map<std::string, std::vector<std::string>> rows_by_key(
    const df::DataFrame& f, const std::vector<std::string>& keys) {
    std::map<std::string, std::vector<std::string>> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        std::string k;
        for (const auto& c : keys) k += bstr(f, r, c) + '\x1f';
        auto& cells = out[k];
        for (std::size_t c = 0; c < f.names.size(); ++c)
            cells.push_back(
                f.columns[c].type() == df::TypeId::String
                    ? bstr(f, r, f.names[c])
                    : dftracer::utils::double_text(bnum(f, r, f.names[c])));
    }
    return out;
}

// `a` and `b` hold the same columns and rows; with `types`, of the same types.
void same_frame(const df::DataFrame& a, const df::DataFrame& b,
                const std::vector<std::string>& keys, bool types = true) {
    REQUIRE(a.names == b.names);
    if (types)
        for (std::size_t c = 0; c < a.names.size(); ++c)
            CHECK(a.columns[c].type() == b.columns[c].type());
    REQUIRE(a.num_rows() == b.num_rows());
    CHECK(rows_by_key(a, keys) == rows_by_key(b, keys));
}

std::int64_t count_of(const View& v, const std::string& query) {
    const df::DataFrame f =
        run(v.duql(query + " | agg { n = count() }").lazy().collect());
    REQUIRE(f.num_rows() == 1);
    return static_cast<std::int64_t>(bnum(f, 0, "n"));
}

// 400 records of a path schema with every role: size i, op read or write,
// hosts h0-h1, threads t0-t3.
const ix::RecordSchema& path_roles() {
    return ix::register_schema(R"(
id: path_roles
fields:
  start: {type: int, role: time}
  took: {type: int, role: duration}
  host: {type: string, role: entity}
  thread: {type: string, role: lane}
  op: {type: string, role: name}
  size: {type: int}
)",
                               "test");
}

std::vector<std::string> path_role_lines() {
    std::vector<std::string> out;
    for (int i = 0; i < 400; ++i)
        out.push_back(R"({"start":)" + std::to_string(1000 + i * 10) +
                      R"(,"took":5,"host":"h)" + std::to_string(i % 2) +
                      R"(","thread":"t)" + std::to_string(i % 4) +
                      R"(","op":")" + (i % 3 ? "read" : "write") +
                      R"(","size":)" + std::to_string(i) + "}");
    return out;
}

std::map<std::string, double> counts_by_op(const View& v) {
    const df::DataFrame f = v.group_by({GroupKey::name()})
                                .agg({{AggOp::Count, "", "n"}})
                                .collect()
                                .get();
    std::map<std::string, double> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out[bstr(f, r, "op")] = bnum(f, r, "n");
    return out;
}

// The file's recorded schema and whether its pruning tier is built.
std::pair<std::string, bool> index_state(const std::string& gz,
                                         const std::string& idx) {
    ix::store::IndexDatabase db(idx, ix::store::IndexOpenMode::ReadOnly);
    const int fid =
        db.get_file_info_id(ix::store::internal::get_logical_path(gz));
    REQUIRE(fid >= 0);
    return {db.file_schema(fid).value_or(""), db.pruning_tier_current(fid)};
}

// Nested calls on two processes of two threads each: step holds read (which
// holds sys) and write. `line` writes one call.
std::vector<std::string> nested_calls(
    const std::function<std::string(int pid, int tid, const char* name,
                                    std::int64_t ts, std::int64_t dur)>& line) {
    std::vector<std::string> out;
    for (int pid : {1, 2})
        for (int tid : {10, 11})
            for (int k = 0; k < 5; ++k) {
                const std::int64_t base = 1000000 + k * 1000 + tid * 100000;
                out.push_back(line(pid, tid, "step", base, 400));
                out.push_back(line(pid, tid, "read", base + 10, 30 + k));
                out.push_back(line(pid, tid, "sys", base + 15, 5));
                out.push_back(line(pid, tid, "write", base + 50, 40));
            }
    return out;
}

// Each flamegraph node's name path ("all/step/read") -> (total, count).
std::map<std::string, std::pair<double, std::int64_t>> flame_paths(
    const df::DataFrame& f) {
    std::vector<std::string> path(static_cast<std::size_t>(f.num_rows()));
    std::map<std::string, std::pair<double, std::int64_t>> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        const auto parent = static_cast<std::int64_t>(bnum(f, r, "parent"));
        path[static_cast<std::size_t>(r)] =
            (parent < 0 ? "" : path[static_cast<std::size_t>(parent)] + "/") +
            bstr(f, r, "name");
        out[path[static_cast<std::size_t>(r)]] = {
            bnum(f, r, "total"),
            static_cast<std::int64_t>(bnum(f, r, "count"))};
    }
    return out;
}

}  // namespace

TEST_SUITE("Schemaless") {
    TEST_CASE("a user schema with dftracer's roles gives the same call tree") {
        ix::register_schema(DFT_ROLES, "test");
        ix::register_schema(R"(
id: secs_roles
fields:
  start: {type: float, role: time, unit: s}
  took: {type: float, role: duration, unit: ms}
  host: {type: string, role: entity}
  thread: {type: string, role: lane}
  op: {type: string, role: name}
)",
                            "test");
        TestEnvironment env(10);
        const auto dft = nested_calls(dft_line);
        const std::string a = write_indexed(env, "dft", dft, 1024);
        const std::string b =
            write_indexed(env, "roles", dft, 1024, "dft_roles");
        const auto secs = nested_calls([](int pid, int tid, const char* name,
                                          std::int64_t ts, std::int64_t dur) {
            return R"({"host":"h)" + std::to_string(pid) + R"(","thread":"t)" +
                   std::to_string(tid) + R"(","op":")" + name +
                   R"(","start":)" +
                   dftracer::utils::double_text(static_cast<double>(ts) / 1e6) +
                   R"(,"took":)" +
                   dftracer::utils::double_text(static_cast<double>(dur) /
                                                1e3) +
                   "}";
        });
        const std::string c =
            write_indexed(env, "secs", secs, 1024, "secs_roles");
        const View va = View::from_file(a, determine_index_path(a, ""));
        const View vb = View::from_file(b, determine_index_path(b, ""));
        const View vc = View::from_file(c, determine_index_path(c, ""));

        const df::DataFrame ta = run(va.call_tree().collect());
        const df::DataFrame tb = run(vb.call_tree().collect());
        REQUIRE(ta.num_rows() == 80);
        REQUIRE(tb.num_rows() == ta.num_rows());
        for (std::int64_t r = 0; r < ta.num_rows(); ++r) {
            for (const char* col :
                 {"pid", "tid", "ts", "dur", "level", "parent_id"})
                CHECK(bnum(tb, r, col) == bnum(ta, r, col));
            CHECK(bstr(tb, r, "name") == bstr(ta, r, "name"));
        }
        const df::DataFrame da = run(va.duql("call_tree").lazy().collect());
        const df::DataFrame db = run(vb.duql("call_tree").lazy().collect());
        REQUIRE(db.num_rows() == da.num_rows());
        for (std::int64_t r = 0; r < da.num_rows(); ++r)
            CHECK(bnum(db, r, "depth") == bnum(da, r, "depth"));

        const auto fa = flame_paths(run(va.flamegraph().collect()));
        CHECK(flame_paths(run(vb.flamegraph().collect())) == fa);
        CHECK(fa.at("all/step/read/sys").second == 20);

        const df::DataFrame ga =
            (va.group_by({GroupKey::pid(), GroupKey::tid(), GroupKey::name()})
                 .agg({{AggOp::Count, "", "n"}})
                 .collect()
                 .get());
        const df::DataFrame gc =
            (vc.group_by({GroupKey::pid(), GroupKey::tid(), GroupKey::name()})
                 .agg({{AggOp::Count, "", "n"}})
                 .collect()
                 .get());
        CHECK(ga.num_rows() == 16);
        CHECK(gc.num_rows() == 16);
        CHECK(bhas(gc, "host"));
        CHECK(bhas(gc, "op"));

        CHECK_THROWS_WITH_AS((void)vc.time_scale(2.0),
                             doctest::Contains("gives its time units"),
                             DFTUtilsException);

        // Seconds and milliseconds come back in those units.
        const df::DataFrame tc = run(vc.call_tree().collect());
        REQUIRE(tc.num_rows() == ta.num_rows());
        CHECK(tc.columns[tc.column_index("ts")].type() == df::TypeId::Float64);
        std::multiset<std::pair<std::string, std::int64_t>> la, lc;
        for (std::int64_t r = 0; r < ta.num_rows(); ++r) {
            la.emplace(bstr(ta, r, "name"),
                       static_cast<std::int64_t>(bnum(ta, r, "level")));
            lc.emplace(bstr(tc, r, "name"),
                       static_cast<std::int64_t>(bnum(tc, r, "level")));
            CHECK(bnum(tc, r, "ts") >= 1.0);
            CHECK(bnum(tc, r, "ts") < 3.0);
        }
        CHECK(lc == la);
        const auto fc = flame_paths(run(vc.flamegraph().collect()));
        REQUIRE(fc.size() == fa.size());
        for (const auto& [path, tc_] : fa) {
            REQUIRE(fc.count(path));
            CHECK(fc.at(path).first == doctest::Approx(tc_.first / 1e3));
            CHECK(fc.at(path).second == tc_.second);
        }
    }

    TEST_CASE("integers past int64 stay exact and wider ones keep the line") {
        TestEnvironment env(10);
        const std::string gz = write_indexed(
            env, "big",
            {R"({"id":18446744073709551615,"k":1})",
             R"({"id":18446744073709551614,"k":2})",
             R"({"id":340282366920938463463374607431768211455,"k":3})",
             R"({"id":5,"k":4})"});
        const View v = View::from_file(gz, determine_index_path(gz, ""))
                           .record_schema("generic");
        CHECK(count_of(v, "where k > 0") == 4);
        const df::DataFrame ids =
            run(v.duql("where k < 3 | select id | sort id").lazy().collect());
        REQUIRE(ids.num_rows() == 2);
        REQUIRE(ids.columns[0].type() == df::TypeId::Uint64);
        const auto col = ids.columns[0].materialize();
        CHECK(col.data<std::uint64_t>()[0] == 18446744073709551614ULL);
        CHECK(col.data<std::uint64_t>()[1] == 18446744073709551615ULL);
        const df::DataFrame wide =
            run(v.duql("where k == 3 | select id").lazy().collect());
        REQUIRE(wide.num_rows() == 1);
        CHECK(bstr(wide, 0, "id") == "340282366920938463463374607431768211455");
    }

    TEST_CASE("an export with select writes nested and dotted paths") {
        TestEnvironment env(10);
        const std::string gz = write_indexed(
            env, "nested",
            {R"({"id":"a","actor":{"login":"u1"},"x.y":7,"tags":["p","q"],"args":{"z":1}})"});
        const View v = View::from_file(gz, determine_index_path(gz, ""))
                           .record_schema("generic");
        StringSink sink;
        v.select({"actor.login", "x.y", "tags.1", "z"}).sink_json(sink).get();
        REQUIRE(sink.lines().size() == 1);
        CHECK(sink.lines()[0] ==
              R"({"actor.login":"u1","x.y":7,"tags.1":"q"})");
    }

    TEST_CASE("a stored row set keeps declared types like the scan") {
        ix::register_schema(R"(
id: typed_rows
fields:
  n: {type: int}
  meta: {type: json}
source: |
  items = where n > 0 | select n, meta
)",
                            "test");
        TestEnvironment env(10);
        const std::string plain = env.get_dir() + "/t.ndjson";
        {
            std::ofstream o(plain);
            o << R"({"n":2.0,"meta":{"b":1,"a":[1,2]}})" << "\n"
              << R"({"n":3,"meta":"x"})" << "\n";
        }
        const std::string gz = plain + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(plain, gz));
        ix::IndexerOptions o;
        o.schema = "typed_rows";
        auto indexer = ix::Indexer::open({gz}, o);
        indexer.build();
        const df::DataFrame stored = indexer.rowset("items");
        REQUIRE(stored.num_rows() == 2);
        CHECK(stored.columns[0].type() == df::TypeId::Int64);
        CHECK(bstr(stored, 0, "meta") == R"({"a":[1,2],"b":1})");
        CHECK(bstr(stored, 1, "meta") == R"("x")");
    }

    TEST_CASE("declared types convert values; the rest read as null") {
        ix::register_schema(R"(
id: coerce
fields:
  at: {type: string, role: time}
  n: {type: int}
  label: {type: string}
)",
                            "test");
        TestEnvironment env(10);
        const std::string gz = write_indexed(
            env, "coerce",
            {R"({"at":"2024-01-01T00:00:00Z","n":"42","label":7})",
             R"({"at":"2024-01-01T00:30:00.5+00:00","n":2.0,"label":"x"})",
             R"({"at":"2024-01-01T02:00:00Z","n":2.5,"label":true})"});
        const View v = View::from_file(gz, determine_index_path(gz, ""))
                           .record_schema("coerce");
        const df::DataFrame f = run(v.duql("select n, label").lazy().collect());
        REQUIRE(f.num_rows() == 3);
        CHECK(bnum(f, 0, "n") == 42);
        CHECK(bnum(f, 1, "n") == 2);
        CHECK(f.columns[0].is_null(2));
        CHECK(f.columns[0].type() == df::TypeId::Int64);
        CHECK(bstr(f, 0, "label") == "7");
        CHECK(bstr(f, 2, "label") == "true");
        const df::DataFrame b =
            run(v.duql("bucket 1h | agg { c = count() } | sort bucket")
                    .lazy()
                    .collect());
        REQUIRE(b.num_rows() == 2);
        CHECK(bnum(b, 0, "c") == 2);
        CHECK(bnum(b, 1, "c") == 1);
        CHECK(bnum(b, 0, "bucket") == 1704067200000000.0);
    }

    TEST_CASE("bad lines and unconverted values are counted, not lost") {
        ix::register_schema("id: counted\nfields:\n  n: {type: int}\n", "test");
        TestEnvironment env(10);
        std::string deep;
        for (int i = 0; i < 1100; ++i) deep += R"({"a":)";
        deep += "1";
        for (int i = 0; i < 1100; ++i) deep += "}";
        const std::string gz =
            write_indexed(env, "bad",
                          {"[", R"({"n":1},)", R"({"n":"n/a"},)", "{not json",
                           deep, R"({"n":3})", "]"},
                          1 << 20);
        const View v = View::from_file(gz, determine_index_path(gz, ""))
                           .record_schema("counted");
        const df::DataFrame f = run(v.duql("select n").lazy().collect());
        CHECK(f.num_rows() == 3);
        // A filter parses every line; the unfiltered export copies lines
        // through unparsed.
        StringSink sink;
        const auto stats =
            v.filter(dftracer::utils::duql::parse_or_throw("exists(n)"))
                .sink_json(sink)
                .get();
        CHECK(sink.lines().size() == 3);
        CHECK(sink.lines()[0] == R"({"n":1})");
        CHECK(stats.lines_invalid == 2);
    }

    TEST_CASE("buckets and time ranges count in the time role's unit") {
        ix::register_schema(R"(
id: seconds_log
fields:
  t: {type: float, role: time, unit: s}
  d: {type: float, role: duration, unit: s}
  k: {type: string}
)",
                            "test");
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int i = 0; i < 300; ++i)
            lines.push_back(R"({"t":)" + std::to_string(1000 + i) +
                            R"(.25,"d":0.5,"k":"a"})");
        const std::string gz = write_indexed(env, "secs", lines, 4096);
        const View v = View::from_file(gz, determine_index_path(gz, ""))
                           .record_schema("seconds_log");
        const df::DataFrame trace =
            run(v.duql("bucket 60s | agg { n = count() } | sort bucket")
                    .lazy()
                    .collect());
        const df::DataFrame frame = run(
            v.duql("bucket 60s | group one = 1 { n = count() } | sort bucket")
                .lazy()
                .collect());
        REQUIRE(trace.num_rows() == 6);
        CHECK(bnum(trace, 0, "bucket") == 960);
        CHECK(bnum(trace, 0, "n") == 20);
        CHECK(bnum(trace, 1, "bucket") == 1020);
        CHECK(bnum(trace, 1, "n") == 60);
        REQUIRE(frame.num_rows() == trace.num_rows());
        for (std::int64_t r = 0; r < trace.num_rows(); ++r) {
            CHECK(bnum(frame, r, "bucket") == bnum(trace, r, "bucket"));
            CHECK(bnum(frame, r, "n") == bnum(trace, r, "n"));
        }
        CHECK(count_of(v, "time_range 1100 .. 1200") == 100);
        const df::DataFrame from_min = v.time_bucket_min(60000000)
                                           .agg({{AggOp::Count, "", "n"}})
                                           .collect()
                                           .get();
        REQUIRE(from_min.num_rows() == 5);
        for (std::int64_t r = 0; r < from_min.num_rows(); ++r)
            CHECK(bnum(from_min, r, "n") == 60);
        const df::DataFrame busy =
            run(v.duql("agg { b = busy(), s = sum(d) }").lazy().collect());
        REQUIRE(busy.num_rows() == 1);
        CHECK(bnum(busy, 0, "s") == 150);
        CHECK(bnum(busy, 0, "b") == 150);
    }

    TEST_CASE("a column a select dropped is an error, not nulls") {
        TestEnvironment env(10);
        const std::string gz =
            write_indexed(env, "sel", {R"({"a":1,"b":2})", R"({"a":3,"b":4})"});
        const View v = View::from_file(gz, determine_index_path(gz, ""))
                           .record_schema("generic");
        std::string what;
        try {
            (void)run(
                v.duql("select a | group b { n = count() }").lazy().collect());
        } catch (const std::exception& e) {
            what = e.what();
        }
        CHECK(what.find("has no column 'b'") != std::string::npos);
        CHECK(count_of(v, "select a | where a > 1") == 1);
    }

    TEST_CASE("zone pruning compares non-integral literals exactly") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int i = 0; i < 40; ++i)
            lines.push_back(R"({"k":"a","x":)" +
                            std::to_string(i < 20 ? 1 : 5) + "}");
        const std::string gz = write_indexed(env, "zones", lines);
        const View v = View::from_file(gz, determine_index_path(gz, ""))
                           .record_schema("generic");
        CHECK(count_of(v, "where x < 1.5") == 20);
        CHECK(count_of(v, "where x > 4.5") == 20);
        CHECK(count_of(v, "where x <= 0.5") == 0);
        CHECK(count_of(v, "where x > -1.5") == 40);
    }

    TEST_CASE(
        "an index of huge arrays keeps a bounded catalog and prunes right") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int r = 0; r < 20; ++r) {
            std::string big;
            for (int i = 0; i < 1000; ++i)
                big += (i ? "," : "") + std::to_string(i + r);
            lines.push_back(R"({"k":)" + std::to_string(r) + R"(,"big":[)" +
                            big + "]}");
        }
        const std::string gz = write_indexed(env, "huge", lines, 4096);
        const View v = View::from_file(gz, determine_index_path(gz, ""))
                           .record_schema("generic");
        CHECK(v.schema_tree().size() < 300);
        CHECK(count_of(v, "where big[500] == 505") == 1);
        CHECK(count_of(v, "where big[999] >= 999") == 20);
        CHECK(count_of(v, "where exists(big[999])") == 20);
        CHECK(count_of(v, "where any(big, . == 1018)") == 1);
        CHECK(count_of(v, "where len(big) == 1000") == 20);
    }

    TEST_CASE("a path field named like a dftracer field prunes by its zones") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (char c : {'a', 'b', 'c'})
            for (int i = 0; i < 1000; ++i) {
                char n[8];
                std::snprintf(n, sizeof(n), "%c%04d", c, i);
                lines.push_back(R"({"name":")" + std::string(n) +
                                R"(","pid":)" + std::to_string(i) + "}");
            }
        // Chunks of more than 256 names carry no bloom, so zones prune.
        const std::string gz = write_indexed(env, "named", lines, 16384);
        const View v = View::from_file(gz, determine_index_path(gz, ""))
                           .record_schema("generic");
        CHECK(count_of(v, "where name == \"b0150\"") == 1);
        CHECK(count_of(v, "where name in [\"a0001\", \"c0999\"]") == 2);
        ix::store::IndexDatabase db(determine_index_path(gz, ""),
                                    ix::store::IndexOpenMode::ReadOnly);
        const auto plan = ix::plan::explain_file_chunks(
            db, gz, duql::parse_or_throw("name == \"b0150\""));
        REQUIRE(plan.total_chunks > 3);
        CHECK(plan.read.size() < plan.total_chunks / 2);
    }

    TEST_CASE(
        "the aggregation tier answers a path schema's roles like the scan") {
        namespace detail = dftracer::utils::trace::views::detail;
        namespace tier = dftracer::utils::index::schemas::dft::agg;
        ix::register_schema(DFT_ROLES, "test");
        ix::register_schema(R"(
id: loose_roles
fields:
  ts: {type: int, role: time}
  dur: {type: float, role: duration}
  pid: {type: int, role: entity, optional: true}
  tid: {type: int, role: lane}
  name: {type: string, role: name}
)",
                            "test");
        // The tier answers a plan over every file of its index, so each file
        // gets its own directory.
        TestEnvironment ea(10), eb(10), ec(10);
        const auto lines = nested_calls(dft_line);
        const std::string a = write_indexed(ea, "dft", lines, 1024, "", 1000);
        const std::string b =
            write_indexed(eb, "roles", lines, 1024, "dft_roles", 1000);
        const std::string c =
            write_indexed(ec, "loose", lines, 1024, "loose_roles", 1000);
        auto plan = [](const std::string& gz, std::vector<GroupKey> keys,
                       std::vector<AggSpec> aggs,
                       const std::string& where = "") {
            auto p = scan::from_file(gz, determine_index_path(gz, ""));
            if (!where.empty())
                p = scan::filter(p, duql::parse_or_throw(where));
            return scan::agg(scan::group_by(p, std::move(keys)),
                             std::move(aggs));
        };
        const std::vector<GroupKey> keys{GroupKey::name(), GroupKey::pid(),
                                         GroupKey::tid()};
        const std::vector<AggSpec> aggs{
            {AggOp::Count, "", "n"},     {AggOp::Sum, "dur", "total"},
            {AggOp::Mean, "dur", "avg"}, {AggOp::Min, "dur", "lo"},
            {AggOp::Max, "dur", "hi"},   {AggOp::Std, "dur", "sd"},
            {AggOp::Min, "ts", "first"}};
        const std::vector<std::string> cols{"name", "pid", "tid"};

        const df::DataFrame dft_tier = tier_only(plan(a, keys, aggs));
        same_frame(dft_tier, scan_only(plan(a, keys, aggs)), cols);
        const df::DataFrame path_tier = tier_only(plan(b, keys, aggs));
        CHECK(path_tier.num_rows() == 16);
        same_frame(path_tier, scan_only(plan(b, keys, aggs)), cols);
        // dftracer reads its integers unsigned, a path record signed.
        same_frame(path_tier, dft_tier, cols, false);

        const std::string reads = R"(name == "read" and pid == 2)";
        const df::DataFrame filtered = tier_only(plan(b, keys, aggs, reads));
        CHECK(filtered.num_rows() == 2);
        same_frame(filtered, scan_only(plan(b, keys, aggs, reads)), cols);

        // Five calls a millisecond apart fall in three 2 ms buckets.
        const auto bucketed = scan::time_bucket(plan(b, keys, aggs), 2000);
        detail::ensure_schema(*bucketed);
        df::DataFrame regular, profiles;
        REQUIRE(tier::events_profiles_collect(*bucketed, regular, profiles));
        CHECK(regular.num_rows() == 48);
        same_frame(regular, scan_only(bucketed),
                   {"time_bucket", "name", "pid", "tid"});

        // A field with no exact tier copy scans: cat is no role, the loose
        // schema's duration is a float and its entity optional.
        auto declines = [](const scan::ScanPlan& p) {
            detail::ensure_schema(*p);
            df::AggStatePtr st;
            return !tier::agg_tier_collect(*p, st);
        };
        // A record without its entity, or with a negative lane, is a null
        // or signed group in the scan, so the tier steps aside.
        TestEnvironment ed(10), ee(10);
        auto no_pid = lines;
        no_pid[5] =
            R"({"id":1,"name":"read","cat":"POSIX","tid":10,"ts":7,"dur":3})";
        auto neg_tid = lines;
        neg_tid[7] = dft_line(1, -3, "read", 9, 4);
        const std::string d =
            write_indexed(ed, "nopid", no_pid, 1024, "dft_roles", 1000);
        const std::string e =
            write_indexed(ee, "negtid", neg_tid, 1024, "dft_roles", 1000);
        CHECK(declines(plan(d, keys, aggs)));
        CHECK(declines(plan(e, keys, aggs)));
        CHECK_FALSE(declines(plan(b, keys, aggs)));

        CHECK(declines(plan(b, {GroupKey::cat()}, {{AggOp::Count, "", "n"}})));
        CHECK(
            declines(plan(c, {GroupKey::name()}, {{AggOp::Sum, "dur", "t"}})));
        CHECK(declines(plan(c, {GroupKey::pid()}, {{AggOp::Count, "", "n"}})));
        const auto by_name = plan(c, {GroupKey::name(), GroupKey::tid()},
                                  {{AggOp::Count, "", "n"}});
        same_frame(tier_only(by_name), scan_only(by_name), {"name", "tid"});
    }

    TEST_CASE("file and rank keys relabel through a path schema's row sets") {
        ix::register_schema(R"(
id: ranked_io
fields:
  ts: {type: int, role: time}
  dur: {type: int, role: duration}
  pid: {type: int, role: entity}
  op: {type: string, role: name}
source: |
  data = where kind == "event";
  files = where kind == "file" | select fhash, path | distinct;
  ranks = where kind == "rank" | select pid, rank | distinct
)",
                            "test");
        ix::register_schema(R"(
id: unranked_io
fields:
  ts: {type: int, role: time}
  pid: {type: int, role: entity}
  op: {type: string, role: name}
)",
                            "test");
        TestEnvironment env(10);
        std::vector<std::string> lines{
            R"({"kind":"file","fhash":"f1","path":"/data/a.bin"})",
            R"({"kind":"file","fhash":"f2","path":"/data/b.bin"})",
            R"({"kind":"rank","pid":100,"rank":0})",
            R"({"kind":"rank","pid":200,"rank":1})"};
        // Rank 0 reads a.bin three times; rank 1 a.bin once and b.bin twice.
        for (int i = 0; i < 6; ++i)
            lines.push_back(R"({"kind":"event","op":"read","ts":)" +
                            std::to_string(1000 + i) + R"(,"dur":10,"pid":)" +
                            (i < 3 ? "100" : "200") + R"(,"fhash":")" +
                            (i < 4 ? "f1" : "f2") + R"("})");
        using Counts = std::map<std::string, std::int64_t>;
        auto counts = [](const View& v, GroupKey key, const char* col) {
            const df::DataFrame f = v.group_by({key})
                                        .agg({{AggOp::Count, "", "n"}})
                                        .collect()
                                        .get();
            Counts out;
            for (std::int64_t r = 0; r < f.num_rows(); ++r)
                out[bstr(f, r, col)] =
                    static_cast<std::int64_t>(bnum(f, r, "n"));
            return out;
        };
        const std::string gz =
            write_indexed(env, "ranked", lines, 256, "ranked_io");
        const View v = View::from_file(gz, determine_index_path(gz, ""));
        CHECK(counts(v, GroupKey::file_path(), "file_path") ==
              Counts{{"/data/a.bin", 4}, {"/data/b.bin", 2}});
        CHECK(counts(v, GroupKey::file_name(), "file_name") ==
              Counts{{"a.bin", 4}, {"b.bin", 2}});
        CHECK(counts(v, GroupKey::rank(), "rank") ==
              Counts{{"0", 3}, {"1", 3}});
        CHECK(counts(v, GroupKey::fhash(), "fhash") ==
              Counts{{"f1", 4}, {"f2", 2}});

        const std::string plain =
            write_indexed(env, "unranked", lines, 256, "unranked_io");
        const View u = View::from_file(plain, determine_index_path(plain, ""));
        CHECK_THROWS_WITH_AS(
            (void)counts(u, GroupKey::rank(), "rank"),
            doctest::Contains(
                "needs row set ranks (pid, rank); schema unranked_io"),
            DFTUtilsException);
        CHECK_THROWS_WITH_AS(
            (void)counts(u, GroupKey::file_path(), "file_path"),
            doctest::Contains("needs row set files"), DFTUtilsException);
        // Without a data row set the file records are rows too.
        const Counts by_hash = counts(u, GroupKey::fhash(), "fhash");
        CHECK(by_hash.at("f1") == 5);
        CHECK(by_hash.at("f2") == 3);
    }

    TEST_CASE("an indexed export of path records reads back as their schema") {
        const ix::RecordSchema& schema = path_roles();
        TestEnvironment env(10);
        const std::string src =
            write_indexed(env, "src", path_role_lines(), 1024, schema.id);
        const View vs = View::from_file(src, determine_index_path(src, ""));
        TraceWriteOptions opts;
        opts.output_path = env.get_dir() + "/out.pfw.gz";
        opts.build_index = true;
        opts.member_size = 1024;
        (void)vs.sink_trace(opts).get();

        const std::string idx = determine_index_path(opts.output_path, "");
        const auto [id, tier] = index_state(opts.output_path, idx);
        CHECK(id == schema.id);
        CHECK(tier);

        const View vo = View::from_file(opts.output_path, idx);
        CHECK(count_of(vo, "where size < 50") == 50);
        CHECK(counts_by_op(vo) == counts_by_op(vs));
        StringSink sink;
        const ExportStats st = vo.duql("size < 50").sink_json(sink).get();
        CHECK(sink.lines().size() == 50);
        CHECK(st.chunks_skipped > 0);
    }

    TEST_CASE("a path-schema file with no index is indexed on first touch") {
        const ix::RecordSchema& schema = path_roles();
        TestEnvironment env(10);
        const auto lines = path_role_lines();
        const std::string ref =
            write_indexed(env, "ref", lines, 1024, schema.id);
        const View vr = View::from_file(ref, determine_index_path(ref, ""));

        const std::string agg = write_gz(env, "agg", lines, 1024);
        const std::string agg_idx =
            determine_index_path(agg, env.get_dir() + "/agg_index");
        REQUIRE_FALSE(fs::exists(agg_idx));
        const View va = View::from_file(agg, agg_idx);
        CHECK(counts_by_op(va) == counts_by_op(vr));
        const auto [agg_id, agg_tier] = index_state(agg, agg_idx);
        CHECK(agg_id == schema.id);
        CHECK(agg_tier);
        CHECK(count_of(va, "where size < 50") == 50);

        const std::string dump = write_gz(env, "dump", lines, 1024);
        const std::string dump_idx =
            determine_index_path(dump, env.get_dir() + "/dump_index");
        REQUIRE_FALSE(fs::exists(dump_idx));
        StringSink sink;
        (void)View::from_file(dump, dump_idx).sink_json(sink).get();
        CHECK(sink.lines().size() == lines.size());
        const auto [dump_id, dump_tier] = index_state(dump, dump_idx);
        CHECK(dump_id == schema.id);
        CHECK(dump_tier);
        const View vd = View::from_file(dump, dump_idx);
        CHECK(counts_by_op(vd) == counts_by_op(vr));
        StringSink pruned;
        const ExportStats st = vd.duql("size < 50").sink_json(pruned).get();
        CHECK(pruned.lines().size() == 50);
        CHECK(st.chunks_skipped > 0);

        // A time window takes no bootstrap; the next whole read builds the
        // tier along the way.
        const std::string late = write_gz(env, "late", lines, 1024);
        const std::string late_idx =
            determine_index_path(late, env.get_dir() + "/late_index");
        const View vl = View::from_file(late, late_idx);
        CHECK(count_of(vl, "time_range 1000 .. 2000") == 100);
        CHECK_FALSE(index_state(late, late_idx).second);
        StringSink whole;
        (void)vl.sink_json(whole).get();
        CHECK(whole.lines().size() == lines.size());
        CHECK(index_state(late, late_idx).second);
        CHECK(count_of(vl, "where size < 50") == 50);
    }

    TEST_CASE("counters of path records take the lane from its role") {
        const ix::RecordSchema& schema = path_roles();
        TestEnvironment env(10);
        const std::string gz =
            write_indexed(env, "ctr", path_role_lines(), 1024, schema.id);
        StringSink sink;
        (void)View::from_file(gz, determine_index_path(gz, ""))
            .group_by({GroupKey::pid(), GroupKey::tid()})
            .agg({{AggOp::Count, "", "n"}})
            .sink_counters(sink)
            .get();
        const auto lines = sink.lines();
        REQUIRE(lines.size() == 4);
        for (int t = 0; t < 4; ++t) {
            const std::string pid =
                std::to_string(ix::entity_id("h" + std::to_string(t % 2)));
            const std::string tid =
                std::to_string(ix::entity_id("t" + std::to_string(t)));
            CHECK(count_containing(
                      lines, "\"pid\":" + pid + ",\"tid\":" + tid + ",") == 1);
        }
    }

    TEST_CASE("an aggregate of an expression keeps its keys and lists") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        for (int i = 0; i < 40; ++i)
            lines.push_back(R"({"k":")" + std::string(i % 2 ? "a" : "b") +
                            R"(","v":)" + std::to_string(i) + R"(,"tags":[)" +
                            std::string(i % 4 ? "1,2" : "3") +
                            R"(],"objs":[{"x":1},{"x":2}]})");
        const std::string gz = write_indexed(env, "exprs", lines, 1024);
        const View v = View::from_file(gz, determine_index_path(gz, ""))
                           .record_schema("generic");
        const df::DataFrame f = run(
            v.duql("group k { s = sum(v + 1), n = sum(len(tags)) } | sort k")
                .lazy()
                .collect());
        REQUIRE(f.num_rows() == 2);
        CHECK(bstr(f, 0, "k") == "a");
        CHECK(bnum(f, 0, "s") == 420);
        CHECK(bnum(f, 0, "n") == 40);
        CHECK(bstr(f, 1, "k") == "b");
        CHECK(bnum(f, 1, "s") == 400);
        CHECK(bnum(f, 1, "n") == 30);
        const df::DataFrame o =
            run(v.duql("agg { n = sum(len(objs)) }").lazy().collect());
        CHECK(bnum(o, 0, "n") == 80);
    }

    TEST_CASE("a field of mixed JSON types is a JSON column") {
        TestEnvironment env(10);
        std::vector<std::string> lines;
        const char* retry[] = {"0", "1", "2.5", R"("3")", "3"};
        for (int i = 0; i < 50; ++i)
            lines.push_back(R"({"k":)" + std::to_string(i) + R"(,"retry":)" +
                            retry[i % 5] + "}");
        const std::string gz = write_indexed(env, "mixed", lines, 256);
        const View v = View::from_file(gz, determine_index_path(gz, ""))
                           .record_schema("generic");
        const df::DataFrame rows =
            run(v.duql("sort k | take 5 | select retry").lazy().collect());
        const df::Series& col = rows.columns[rows.column_index("retry")];
        REQUIRE(col.is_json());
        const std::vector<std::string> want{"0", "1", "2.5", R"("3")", "3"};
        for (std::int64_t r = 0; r < 5; ++r)
            CHECK(col.string_at(r) == want[static_cast<std::size_t>(r)]);

        const df::DataFrame keys =
            run(v.duql("group retry { n = count() } | sort retry")
                    .lazy()
                    .collect());
        const df::Series& key = keys.columns[keys.column_index("retry")];
        CHECK(key.is_json());
        std::map<std::string, std::int64_t> by_key;
        for (std::int64_t r = 0; r < keys.num_rows(); ++r)
            by_key[std::string(key.string_at(r))] =
                static_cast<std::int64_t>(bnum(keys, r, "n"));
        CHECK(
            by_key ==
            std::map<std::string, std::int64_t>{
                {"0", 10}, {"1", 10}, {"2.5", 10}, {R"("3")", 10}, {"3", 10}});
        // A field of one type keeps it.
        CHECK_FALSE(
            run(v.duql("select k").lazy().collect()).columns[0].is_json());
    }

    TEST_CASE("sessions of a path schema equal the dftracer sessions") {
        ix::register_schema(R"(
id: secs_sessions
fields:
  start: {type: float, role: time, unit: s}
  took: {type: float, role: duration, unit: ms}
  host: {type: string, role: entity}
  thread: {type: string, role: lane}
)",
                            "test");
        TestEnvironment env(10);
        std::vector<std::string> dft;
        std::vector<std::string> secs;
        std::uint64_t x = 2463534242ULL;
        for (int i = 0; i < 600; ++i) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            const int pid = 1 + i % 2;
            const int tid = 1 + (i / 2) % 2;
            const std::int64_t ts = static_cast<std::int64_t>(x % 3000) * 1000;
            const std::int64_t dur = static_cast<std::int64_t>(x % 50) * 10;
            dft.push_back(dft_line(pid, tid, "read", ts, dur));
            secs.push_back(
                R"({"host":"h)" + std::to_string(pid) + R"(","thread":"t)" +
                std::to_string(tid) + R"(","start":)" +
                dftracer::utils::double_text(static_cast<double>(ts) / 1e6) +
                R"(,"took":)" +
                dftracer::utils::double_text(static_cast<double>(dur) / 1e3) +
                "}");
        }
        secs.push_back(R"({"host":"h1","thread":"t1","took":1})");
        const std::string a = write_indexed(env, "sdft", dft, 4096);
        const std::string b =
            write_indexed(env, "ssecs", secs, 4096, "secs_sessions");
        const View va = View::from_file(a, determine_index_path(a, ""));
        const View vb = View::from_file(b, determine_index_path(b, ""));
        const df::DataFrame fa = run(
            va.duql("session pid, tid gap 105us max 300ms | group pid, tid, "
                    "session { n = count() } | sort pid, tid, session")
                .lazy()
                .collect());
        const df::DataFrame fb = run(
            vb.duql("session host, thread gap 105us max 300ms | group host, "
                    "thread, session { n = count() } | sort host, thread, "
                    "session")
                .lazy()
                .collect());
        // The untimed record is one null session of h1/t1.
        REQUIRE(fb.num_rows() == fa.num_rows() + 1);
        std::int64_t r = 0;
        for (std::int64_t i = 0; i < fb.num_rows(); ++i) {
            if (fb.columns[fb.column_index("session")].is_null(i)) {
                CHECK(bnum(fb, i, "n") == 1);
                continue;
            }
            CHECK(bstr(fb, i, "host") ==
                  "h" + std::to_string(static_cast<int>(bnum(fa, r, "pid"))));
            CHECK(bnum(fb, i, "session") == bnum(fa, r, "session"));
            CHECK(bnum(fb, i, "n") == bnum(fa, r, "n"));
            ++r;
        }
        CHECK(r == fa.num_rows());
        CHECK(fa.num_rows() > 8);
    }
}
