// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_agg_partials.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <cstdio>
#include <fstream>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

using namespace dftracer::utils::trace;

namespace {

// `lines` as a gzip of members of about `member_bytes`, indexed with small
// checkpoints so each member is its own chunk.
dstd::String write_gz(TestEnvironment& env, const dstd::String& name,
                      const dstd::Vector<dstd::String>& lines,
                      std::size_t member_bytes = 256) {
    const dstd::String plain = env.dir() + "/" + name + ".ndjson";
    {
        dftu_utils_test::FileOut o(plain);
        for (const auto& l : lines) o << l << "\n";
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               member_bytes));
    dstd::fs::remove(plain).discard();
    return gz;
}

dstd::String write_indexed(TestEnvironment& env, const dstd::String& name,
                           const dstd::Vector<dstd::String>& lines,
                           std::size_t member_bytes = 256,
                           dstd::StringView schema = "",
                           std::uint64_t tier_interval_us = 0) {
    const dstd::String gz = write_gz(env, name, lines, member_bytes);
    index::IndexerOptions o;
    o.checkpoint_size = member_bytes;
    o.schema = schema;
    if (tier_interval_us > 0) o.aggregation.time_interval_us = tier_interval_us;
    index::Indexer::open({dstd::String(dstd::StringView(gz))}, o).build();
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
dstd::String dft_line(int pid, int tid, const char* name, std::int64_t ts,
                      std::int64_t dur) {
    return R"({"id":1,"name":")" + dstd::String(name) +
           R"(","cat":"POSIX","pid":)" + dstd::to_string(pid) + R"(,"tid":)" +
           dstd::to_string(tid) + R"(,"ts":)" + dstd::to_string(ts) +
           R"(,"dur":)" + dstd::to_string(dur) +
           R"(,"ph":"X","args":{"size":7}})";
}

// The aggregation of `p` by a full scan, the tier bypassed.
dataframe::DataFrame scan_only(const views::detail::scan::ScanPlan& p) {
    Runtime rt;
    dataframe::DataFrame out;
    dftu_utils_test::ok(rt.block_on(coro::named(
        "scan-only",
        coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            auto st = co_await views::detail::build_engine_agg_state(*p);
            out = views::detail::finalize_engine_result(*st, *p);
        }))));
    return views::detail::apply_agg_post_ops(std::move(out), *p);
}

// Fills `st` when the chunk partials answer `p`.
bool partials_serve(const views::detail::scan::ScanPlan& p,
                    dataframe::AggStatePtr& st) {
    views::detail::ensure_schema(*p);
    Runtime rt;
    bool served = false;
    dftu_utils_test::ok(rt.block_on(coro::named(
        "partials", coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            served = co_await views::detail::partials_collect(*p, st);
        }))));
    return served;
}

// The aggregation of `p` from the chunk partials. Fails when they decline it.
dataframe::DataFrame tier_only(const views::detail::scan::ScanPlan& p) {
    dataframe::AggStatePtr st;
    REQUIRE(partials_serve(p, st));
    return views::detail::apply_agg_post_ops(
        views::detail::finalize_engine_result(*st, *p), *p);
}

// Whether the chunk partials answer the events and profiles of `p`.
bool typed_serve(const views::detail::scan::ScanPlan& p,
                 dataframe::DataFrame& regular,
                 dataframe::DataFrame& profiles) {
    views::detail::ensure_schema(*p);
    Runtime rt;
    bool served = false;
    dftu_utils_test::ok(rt.block_on(
        coro::named("typed-partials",
                    coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                        served = co_await views::detail::partials_collect_typed(
                            *p, regular, profiles);
                    }))));
    return served;
}

// Each row of `f` as its cells' text, keyed by the `keys` columns' text.
dstd::Map<dstd::String, dstd::Vector<dstd::String>> rows_by_key(
    const dataframe::DataFrame& f, const dstd::Vector<dstd::String>& keys) {
    dstd::Map<dstd::String, dstd::Vector<dstd::String>> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        dstd::String k;
        for (const auto& c : keys) k += bstr(f, r, c) + '\x1f';
        auto& cells = out[k];
        for (std::size_t c = 0; c < f.names.size(); ++c)
            cells.push_back(
                f.columns[c].type() == dataframe::TypeId::String
                    ? bstr(f, r, f.names[c])
                    : dstd::String(dstd::double_text(bnum(f, r, f.names[c]))));
    }
    return out;
}

// `a` and `b` hold the same columns and rows; with `types`, of the same types.
void same_frame(const dataframe::DataFrame& a, const dataframe::DataFrame& b,
                const dstd::Vector<dstd::String>& keys, bool types = true) {
    REQUIRE(a.names == b.names);
    if (types)
        for (std::size_t c = 0; c < a.names.size(); ++c)
            CHECK(a.columns[c].type() == b.columns[c].type());
    REQUIRE(a.num_rows() == b.num_rows());
    CHECK(rows_by_key(a, keys) == rows_by_key(b, keys));
}

std::int64_t count_of(const View& v, const dstd::String& query) {
    const dataframe::DataFrame f =
        run(v.duql(query + " | agg { n = count() }").lazy().collect());
    REQUIRE(f.num_rows() == 1);
    return static_cast<std::int64_t>(bnum(f, 0, "n"));
}

// 400 records of a path schema with every role: size i, op read or write,
// hosts h0-h1, threads t0-t3.
const index::RecordSchema& path_roles() {
    return index::register_schema(R"(
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

dstd::Vector<dstd::String> path_role_lines() {
    dstd::Vector<dstd::String> out;
    for (int i = 0; i < 400; ++i)
        out.push_back(R"({"start":)" + dstd::to_string(1000 + i * 10) +
                      R"(,"took":5,"host":"h)" + dstd::to_string(i % 2) +
                      R"(","thread":"t)" + dstd::to_string(i % 4) +
                      R"(","op":")" + (i % 3 ? "read" : "write") +
                      R"(","size":)" + dstd::to_string(i) + "}");
    return out;
}

dstd::Map<dstd::String, double> counts_by_op(const View& v) {
    const dataframe::DataFrame f =
        dftu_utils_test::ok(v.group_by({GroupKey::name()})
                                .agg({{AggOp::Count, "", "n"}})
                                .collect()
                                .get());
    dstd::Map<dstd::String, double> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out[bstr(f, r, "op")] = bnum(f, r, "n");
    return out;
}

bool bloom_has(const dstd::String& idx, const dstd::String& gz,
               dstd::StringView dim, dstd::StringView value) {
    const auto snapshot = dftu_utils_test::index_snapshot(idx);
    const auto file = dftu_utils_test::require_indexed_file(snapshot, gz);
    const auto bytes = dftu_utils_test::index_take(index::path_file(
        snapshot, file, index::store::IndexExtension::Bloom, dim));
    if (!bytes) return false;
    const auto bloom = index::extensions::kinds::decode_bloom(
        dstd::StringView(bytes->data(), bytes->size()));
    return bloom && bloom->possibly_contains(value);
}

// The file's recorded schema and whether its pruning tier is built.
dstd::Pair<dstd::String, bool> index_state(const dstd::String& gz,
                                           const dstd::String& idx) {
    const auto snapshot = dftu_utils_test::index_snapshot(idx);
    const auto file = dftu_utils_test::require_indexed_file(snapshot, gz);
    const auto schema =
        dftu_utils_test::index_take(index::schema(snapshot, file));
    return {schema ? dstd::String(schema->data(), schema->size()) : "",
            dftu_utils_test::pruning_tier_current(snapshot, file)};
}

// Nested calls on two processes of two threads each: step holds read (which
// holds sys) and write. `line` writes one call.
dstd::Vector<dstd::String> nested_calls(
    const dstd::Function<dstd::String(int pid, int tid, const char* name,
                                      std::int64_t ts, std::int64_t dur)>&
        line) {
    dstd::Vector<dstd::String> out;
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
dstd::Map<dstd::String, dstd::Pair<double, std::int64_t>> flame_paths(
    const dataframe::DataFrame& f) {
    dstd::Vector<dstd::String> path(static_cast<std::size_t>(f.num_rows()));
    dstd::Map<dstd::String, dstd::Pair<double, std::int64_t>> out;
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
        index::register_schema(DFT_ROLES, "test");
        index::register_schema(R"(
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
        const dstd::String a = write_indexed(env, "dft", dft, 1024);
        const dstd::String b =
            write_indexed(env, "roles", dft, 1024, "dft_roles");
        const auto secs = nested_calls([](int pid, int tid, const char* name,
                                          std::int64_t ts, std::int64_t dur) {
            return R"({"host":"h)" + dstd::to_string(pid) + R"(","thread":"t)" +
                   dstd::to_string(tid) + R"(","op":")" + name +
                   R"(","start":)" +
                   dstd::String(
                       dstd::double_text(static_cast<double>(ts) / 1e6)) +
                   R"(,"took":)" +
                   dstd::String(
                       dstd::double_text(static_cast<double>(dur) / 1e3)) +
                   "}";
        });
        const dstd::String c =
            write_indexed(env, "secs", secs, 1024, "secs_roles");
        const View va = View::from_file(
            dstd::String(a),
            dstd::String(dstd::String(determine_index_path(a, ""))));
        const View vb = View::from_file(
            dstd::String(b),
            dstd::String(dstd::String(determine_index_path(b, ""))));
        const View vc = View::from_file(
            dstd::String(c),
            dstd::String(dstd::String(determine_index_path(c, ""))));

        const dataframe::DataFrame ta = run(va.call_tree().collect());
        const dataframe::DataFrame tb = run(vb.call_tree().collect());
        REQUIRE(ta.num_rows() == 80);
        REQUIRE(tb.num_rows() == ta.num_rows());
        for (std::int64_t r = 0; r < ta.num_rows(); ++r) {
            for (const char* col :
                 {"pid", "tid", "ts", "dur", "level", "parent_id"})
                CHECK(bnum(tb, r, col) == bnum(ta, r, col));
            CHECK(bstr(tb, r, "name") == bstr(ta, r, "name"));
        }
        const dataframe::DataFrame da =
            run(va.duql("call_tree").lazy().collect());
        const dataframe::DataFrame db =
            run(vb.duql("call_tree").lazy().collect());
        REQUIRE(db.num_rows() == da.num_rows());
        for (std::int64_t r = 0; r < da.num_rows(); ++r)
            CHECK(bnum(db, r, "depth") == bnum(da, r, "depth"));

        const auto fa = flame_paths(run(va.flamegraph().collect()));
        CHECK(flame_paths(run(vb.flamegraph().collect())) == fa);
        CHECK(fa.at("all/step/read/sys").second == 20);

        const dataframe::DataFrame ga = dftu_utils_test::ok(
            (va.group_by({GroupKey::pid(), GroupKey::tid(), GroupKey::name()})
                 .agg({{AggOp::Count, "", "n"}})
                 .collect()
                 .get()));
        const dataframe::DataFrame gc = dftu_utils_test::ok(
            (vc.group_by({GroupKey::pid(), GroupKey::tid(), GroupKey::name()})
                 .agg({{AggOp::Count, "", "n"}})
                 .collect()
                 .get()));
        CHECK(ga.num_rows() == 16);
        CHECK(gc.num_rows() == 16);
        CHECK(bhas(gc, "host"));
        CHECK(bhas(gc, "op"));

        CHECK_THROWS_WITH_AS((void)vc.time_scale(2.0),
                             doctest::Contains("gives its time units"),
                             dftracer::utils::common::DFTUtilsException);

        // Seconds and milliseconds come back in those units.
        const dataframe::DataFrame tc = run(vc.call_tree().collect());
        REQUIRE(tc.num_rows() == ta.num_rows());
        CHECK(tc.columns[tc.column_index("ts")].type() ==
              dataframe::TypeId::Float64);
        dstd::MultiSet<dstd::Pair<dstd::String, std::int64_t>> la, lc;
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
        const dstd::String gz = write_indexed(
            env, "big",
            {R"({"id":18446744073709551615,"k":1})",
             R"({"id":18446744073709551614,"k":2})",
             R"({"id":340282366920938463463374607431768211455,"k":3})",
             R"({"id":5,"k":4})"});
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("generic");
        CHECK(count_of(v, "where k > 0") == 4);
        const dataframe::DataFrame ids =
            run(v.duql("where k < 3 | select id | sort id").lazy().collect());
        REQUIRE(ids.num_rows() == 2);
        REQUIRE(ids.columns[0].type() == dataframe::TypeId::Uint64);
        const auto col = ids.columns[0].materialize();
        CHECK(col.data<std::uint64_t>()[0] == 18446744073709551614ULL);
        CHECK(col.data<std::uint64_t>()[1] == 18446744073709551615ULL);
        const dataframe::DataFrame wide =
            run(v.duql("where k == 3 | select id").lazy().collect());
        REQUIRE(wide.num_rows() == 1);
        CHECK(bstr(wide, 0, "id") == "340282366920938463463374607431768211455");
    }

    TEST_CASE("an export with select writes nested and dotted paths") {
        TestEnvironment env(10);
        const dstd::String gz = write_indexed(
            env, "nested",
            {R"({"id":"a","actor":{"login":"u1"},"x.y":7,"tags":["p","q"],"args":{"z":1}})"});
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("generic");
        StringSink sink;
        dftu_utils_test::ok(v.select({"actor.login", "x.y", "tags.1", "z"})
                                .sink_json(sink)
                                .get());
        REQUIRE(sink.lines().size() == 1);
        CHECK(sink.lines()[0] ==
              R"({"actor.login":"u1","x.y":7,"tags.1":"q"})");
    }

    TEST_CASE("a stored row set keeps declared types like the scan") {
        index::register_schema(R"(
id: typed_rows
fields:
  n: {type: int}
  meta: {type: json}
source: |
  items = where n > 0 | select n, meta
)",
                               "test");
        TestEnvironment env(10);
        const dstd::String plain = env.dir() + "/t.ndjson";
        {
            dftu_utils_test::FileOut o(plain);
            o << R"({"n":2.0,"meta":{"b":1,"a":[1,2]}})" << "\n"
              << R"({"n":3,"meta":"x"})" << "\n";
        }
        const dstd::String gz = plain + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(plain, gz));
        index::IndexerOptions o;
        o.schema = "typed_rows";
        auto indexer =
            index::Indexer::open({dstd::String(dstd::StringView(gz))}, o);
        indexer.build();
        const dataframe::DataFrame stored = indexer.rowset("items");
        REQUIRE(stored.num_rows() == 2);
        CHECK(stored.columns[0].type() == dataframe::TypeId::Int64);
        CHECK(bstr(stored, 0, "meta") == R"({"a":[1,2],"b":1})");
        CHECK(bstr(stored, 1, "meta") == R"("x")");
    }

    TEST_CASE("declared types convert values; the rest read as null") {
        index::register_schema(R"(
id: coerce
fields:
  at: {type: string, role: time}
  n: {type: int}
  label: {type: string}
)",
                               "test");
        TestEnvironment env(10);
        const dstd::String gz = write_indexed(
            env, "coerce",
            {R"({"at":"2024-01-01T00:00:00Z","n":"42","label":7})",
             R"({"at":"2024-01-01T00:30:00.5+00:00","n":2.0,"label":"x"})",
             R"({"at":"2024-01-01T02:00:00Z","n":2.5,"label":true})"});
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("coerce");
        const dataframe::DataFrame f =
            run(v.duql("select n, label").lazy().collect());
        REQUIRE(f.num_rows() == 3);
        CHECK(bnum(f, 0, "n") == 42);
        CHECK(bnum(f, 1, "n") == 2);
        CHECK(f.columns[0].is_null(2));
        CHECK(f.columns[0].type() == dataframe::TypeId::Int64);
        CHECK(bstr(f, 0, "label") == "7");
        CHECK(bstr(f, 2, "label") == "true");
        const dataframe::DataFrame b =
            run(v.duql("bucket 1h | agg { c = count() } | sort bucket")
                    .lazy()
                    .collect());
        REQUIRE(b.num_rows() == 2);
        CHECK(bnum(b, 0, "c") == 2);
        CHECK(bnum(b, 1, "c") == 1);
        CHECK(bnum(b, 0, "bucket") == 1704067200000000.0);
    }

    TEST_CASE(
        "digit text stays text in a string field and parses in a number "
        "field") {
        index::register_schema(R"(
id: digits
fields:
  at: {type: string, role: time}
  x: {type: float}
  label: {type: string}
)",
                               "test");
        TestEnvironment env(10);
        const dstd::String gz = write_indexed(
            env, "digits",
            {R"({"at":"2024-01-01T00:00:00Z","x":"1e3","label":"007"})",
             R"({"at":"2024-01-01T00:30:00Z","x":"-1.5","label":"1.50"})"});
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("digits");
        const dataframe::DataFrame f =
            run(v.duql("select x, label").lazy().collect());
        REQUIRE(f.num_rows() == 2);
        CHECK(f.columns[0].type() == dataframe::TypeId::Float64);
        CHECK(bnum(f, 0, "x") == 1000);
        CHECK(bnum(f, 1, "x") == -1.5);
        CHECK(bstr(f, 0, "label") == "007");
        CHECK(bstr(f, 1, "label") == "1.50");
    }

    TEST_CASE("bad lines and unconverted values are counted, not lost") {
        index::register_schema("id: counted\nfields:\n  n: {type: int}\n",
                               "test");
        TestEnvironment env(10);
        dstd::String deep;
        for (int i = 0; i < 1100; ++i) deep += R"({"a":)";
        deep += "1";
        for (int i = 0; i < 1100; ++i) deep += "}";
        const dstd::String gz =
            write_indexed(env, "bad",
                          {"[", R"({"n":1},)", R"({"n":"n/a"},)", "{not json",
                           deep, R"({"n":3})", "]"},
                          1 << 20);
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("counted");
        const dataframe::DataFrame f = run(v.duql("select n").lazy().collect());
        // The nested line is valid JSON that holds no n: a row of nulls.
        CHECK(f.num_rows() == 4);
        // A filter parses every line; the unfiltered export copies lines
        // through unparsed.
        StringSink sink;
        const auto stats = dftu_utils_test::ok(
            v.filter(dftracer::utils::duql::parse_or_throw("exists(n)"))
                .sink_json(sink)
                .get());
        CHECK(sink.lines().size() == 3);
        CHECK(sink.lines()[0] == R"({"n":1})");
        CHECK(stats.lines_invalid == 1);
    }

    TEST_CASE("buckets and time ranges count in the time role's unit") {
        index::register_schema(R"(
id: seconds_log
fields:
  t: {type: float, role: time, unit: s}
  d: {type: float, role: duration, unit: s}
  k: {type: string}
)",
                               "test");
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        for (int i = 0; i < 300; ++i)
            lines.push_back(R"({"t":)" + dstd::to_string(1000 + i) +
                            R"(.25,"d":0.5,"k":"a"})");
        const dstd::String gz = write_indexed(env, "secs", lines, 4096);
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("seconds_log");
        const dataframe::DataFrame trace =
            run(v.duql("bucket 60s | agg { n = count() } | sort bucket")
                    .lazy()
                    .collect());
        const dataframe::DataFrame frame = run(
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
        const dataframe::DataFrame from_min =
            dftu_utils_test::ok(v.time_bucket_min(60000000)
                                    .agg({{AggOp::Count, "", "n"}})
                                    .collect()
                                    .get());
        REQUIRE(from_min.num_rows() == 5);
        for (std::int64_t r = 0; r < from_min.num_rows(); ++r)
            CHECK(bnum(from_min, r, "n") == 60);
        const dataframe::DataFrame busy =
            run(v.duql("agg { b = busy(), s = sum(d) }").lazy().collect());
        REQUIRE(busy.num_rows() == 1);
        CHECK(bnum(busy, 0, "s") == 150);
        CHECK(bnum(busy, 0, "b") == 150);
    }

    TEST_CASE("a column a select dropped is an error, not nulls") {
        TestEnvironment env(10);
        const dstd::String gz =
            write_indexed(env, "sel", {R"({"a":1,"b":2})", R"({"a":3,"b":4})"});
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("generic");
        dstd::String what;
        try {
            (void)run(
                v.duql("select a | group b { n = count() }").lazy().collect());
        } catch (const std::exception& e) {
            what = e.what();
        }
        CHECK(what.find("has no column 'b'") != dstd::String::npos);
        CHECK(count_of(v, "select a | where a > 1") == 1);
    }

    TEST_CASE("zone pruning compares non-integral literals exactly") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        for (int i = 0; i < 40; ++i)
            lines.push_back(R"({"k":"a","x":)" +
                            dstd::to_string(i < 20 ? 1 : 5) + "}");
        const dstd::String gz = write_indexed(env, "zones", lines);
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("generic");
        CHECK(count_of(v, "where x < 1.5") == 20);
        CHECK(count_of(v, "where x > 4.5") == 20);
        CHECK(count_of(v, "where x <= 0.5") == 0);
        CHECK(count_of(v, "where x > -1.5") == 40);
    }

    TEST_CASE(
        "an index of huge arrays keeps a bounded catalog and prunes right") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        for (int r = 0; r < 20; ++r) {
            dstd::String big;
            for (int i = 0; i < 1000; ++i)
                big += (i ? "," : "") + dstd::to_string(i + r);
            lines.push_back(R"({"k":)" + dstd::to_string(r) + R"(,"big":[)" +
                            big + "]}");
        }
        const dstd::String gz = write_indexed(env, "huge", lines, 4096);
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
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
        dstd::Vector<dstd::String> lines;
        for (char c : {'a', 'b', 'c'})
            for (int i = 0; i < 1000; ++i) {
                char n[8];
                std::snprintf(n, sizeof(n), "%c%04d", c, i);
                lines.push_back(R"({"name":")" + dstd::String(n) +
                                R"(","pid":)" + dstd::to_string(i) + "}");
            }
        // Chunks of more than 256 names carry no bloom, so zones prune.
        const dstd::String gz = write_indexed(env, "named", lines, 16384);
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("generic");
        CHECK(count_of(v, "where name == \"b0150\"") == 1);
        CHECK(count_of(v, "where name in [\"a0001\", \"c0999\"]") == 2);
        const auto snapshot = index::plan::open_snapshot(
            dstd::String(determine_index_path(gz, "")));
        REQUIRE(snapshot.has_value());
        const auto plan = index::plan::explain_file_chunks(
            *snapshot, dstd::String(dstd::StringView(gz)),
            duql::parse_or_throw("name == \"b0150\""));
        REQUIRE(plan.total_chunks > 3);
        CHECK(plan.read.size() < plan.total_chunks / 2);
    }

    TEST_CASE(
        "the aggregation tier answers a path schema's roles like the scan") {
        index::register_schema(DFT_ROLES, "test");
        index::register_schema(R"(
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
        const dstd::String a = write_indexed(ea, "dft", lines, 1024, "", 1000);
        const dstd::String b =
            write_indexed(eb, "roles", lines, 1024, "dft_roles", 1000);
        const dstd::String c =
            write_indexed(ec, "loose", lines, 1024, "loose_roles", 1000);
        auto plan = [](const dstd::String& gz, dstd::Vector<GroupKey> keys,
                       dstd::Vector<AggSpec> aggs,
                       const dstd::String& where = "") {
            auto p = views::detail::scan::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))));
            if (!where.empty())
                p = views::detail::scan::filter(p, duql::parse_or_throw(where));
            return views::detail::scan::agg(
                views::detail::scan::group_by(p, std::move(keys)),
                std::move(aggs));
        };
        const dstd::Vector<GroupKey> keys{GroupKey::name(), GroupKey::pid(),
                                          GroupKey::tid()};
        const dstd::Vector<AggSpec> aggs{
            {AggOp::Count, "", "n"},     {AggOp::Sum, "dur", "total"},
            {AggOp::Mean, "dur", "avg"}, {AggOp::Min, "dur", "lo"},
            {AggOp::Max, "dur", "hi"},   {AggOp::Std, "dur", "sd"},
            {AggOp::Min, "ts", "first"}};
        const dstd::Vector<dstd::String> cols{"name", "pid", "tid"};

        const dataframe::DataFrame dft_tier = tier_only(plan(a, keys, aggs));
        same_frame(dft_tier, scan_only(plan(a, keys, aggs)), cols);
        const dataframe::DataFrame path_tier = tier_only(plan(b, keys, aggs));
        CHECK(path_tier.num_rows() == 16);
        same_frame(path_tier, scan_only(plan(b, keys, aggs)), cols);
        // dftracer reads its integers unsigned, a path record signed.
        same_frame(path_tier, dft_tier, cols, false);

        const dstd::String reads = R"(name == "read" and pid == 2)";
        const dataframe::DataFrame filtered =
            tier_only(plan(b, keys, aggs, reads));
        CHECK(filtered.num_rows() == 2);
        same_frame(filtered, scan_only(plan(b, keys, aggs, reads)), cols);

        // A one second window holds every chunk of the file, so the partials
        // answer it. A two millisecond window cuts chunks, which only a
        // re-read of the traces can split, and a path plan reads none.
        const auto bucketed =
            views::detail::scan::time_bucket(plan(b, keys, aggs), 1000000);
        dataframe::DataFrame regular, profiles;
        REQUIRE(typed_serve(bucketed, regular, profiles));
        CHECK(regular.num_rows() == 16);
        same_frame(regular, scan_only(bucketed),
                   {"time_bucket", "name", "pid", "tid"});
        dataframe::DataFrame cut_regular, cut_profiles;
        CHECK_FALSE(typed_serve(
            views::detail::scan::time_bucket(plan(b, keys, aggs), 2000),
            cut_regular, cut_profiles));

        // A field with no exact tier copy scans: cat is no role, the loose
        // schema's duration is a float and its entity optional.
        auto declines = [](const views::detail::scan::ScanPlan& p) {
            dataframe::AggStatePtr st;
            return !partials_serve(p, st);
        };
        // A record without its entity, or with a negative lane, is a null
        // or signed group in the scan, so the tier steps aside.
        TestEnvironment ed(10), ee(10);
        auto no_pid = lines;
        no_pid[5] =
            R"({"id":1,"name":"read","cat":"POSIX","tid":10,"ts":7,"dur":3})";
        auto neg_tid = lines;
        neg_tid[7] = dft_line(1, -3, "read", 9, 4);
        const dstd::String d =
            write_indexed(ed, "nopid", no_pid, 1024, "dft_roles", 1000);
        const dstd::String e =
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
        index::register_schema(R"(
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
        index::register_schema(R"(
id: unranked_io
fields:
  ts: {type: int, role: time}
  pid: {type: int, role: entity}
  op: {type: string, role: name}
)",
                               "test");
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines{
            R"({"kind":"file","fhash":"f1","path":"/data/a.bin"})",
            R"({"kind":"file","fhash":"f2","path":"/data/b.bin"})",
            R"({"kind":"rank","pid":100,"rank":0})",
            R"({"kind":"rank","pid":200,"rank":1})"};
        // Rank 0 reads a.bin three times; rank 1 a.bin once and b.bin twice.
        for (int i = 0; i < 6; ++i)
            lines.push_back(R"({"kind":"event","op":"read","ts":)" +
                            dstd::to_string(1000 + i) + R"(,"dur":10,"pid":)" +
                            (i < 3 ? "100" : "200") + R"(,"fhash":")" +
                            (i < 4 ? "f1" : "f2") + R"("})");
        using Counts = dstd::Map<dstd::String, std::int64_t>;
        auto counts = [](const View& v, GroupKey key, const char* col) {
            const dataframe::DataFrame f =
                dftu_utils_test::ok(v.group_by({key})
                                        .agg({{AggOp::Count, "", "n"}})
                                        .collect()
                                        .get());
            Counts out;
            for (std::int64_t r = 0; r < f.num_rows(); ++r)
                out[bstr(f, r, col)] =
                    static_cast<std::int64_t>(bnum(f, r, "n"));
            return out;
        };
        const dstd::String gz =
            write_indexed(env, "ranked", lines, 256, "ranked_io");
        const View v = View::from_file(
            dstd::String(gz),
            dstd::String(dstd::String(determine_index_path(gz, ""))));
        CHECK(counts(v, GroupKey::file_path(), "file_path") ==
              Counts{{"/data/a.bin", 4}, {"/data/b.bin", 2}});
        CHECK(counts(v, GroupKey::file_name(), "file_name") ==
              Counts{{"a.bin", 4}, {"b.bin", 2}});
        CHECK(counts(v, GroupKey::rank(), "rank") ==
              Counts{{"0", 3}, {"1", 3}});
        CHECK(counts(v, GroupKey::fhash(), "fhash") ==
              Counts{{"f1", 4}, {"f2", 2}});

        const dstd::String plain =
            write_indexed(env, "unranked", lines, 256, "unranked_io");
        const View u = View::from_file(
            dstd::String(plain),
            dstd::String(dstd::String(determine_index_path(plain, ""))));
        CHECK_THROWS_WITH_AS(
            (void)counts(u, GroupKey::rank(), "rank"),
            doctest::Contains(
                "needs row set ranks (pid, rank); schema unranked_io"),
            dftracer::utils::common::DFTUtilsException);
        CHECK_THROWS_WITH_AS(
            (void)counts(u, GroupKey::file_path(), "file_path"),
            doctest::Contains("needs row set files"),
            dftracer::utils::common::DFTUtilsException);
        // Without a data row set the file records are rows too.
        const Counts by_hash = counts(u, GroupKey::fhash(), "fhash");
        CHECK(by_hash.at("f1") == 5);
        CHECK(by_hash.at("f2") == 3);
    }

    TEST_CASE("an indexed export of path records reads back as their schema") {
        const index::RecordSchema& schema = path_roles();
        TestEnvironment env(10);
        const dstd::String src =
            write_indexed(env, "src", path_role_lines(), 1024, schema.id);
        const View vs = View::from_file(
            dstd::String(src),
            dstd::String(dstd::String(determine_index_path(src, ""))));
        TraceWriteOptions opts;
        opts.output_path = env.dir() + "/out.pfw.gz";
        opts.build_index = true;
        opts.member_size = 1024;
        (void)vs.sink_trace(opts).get();

        const dstd::String idx =
            dstd::String(determine_index_path(opts.output_path, ""));
        const auto [id, tier] =
            index_state(dstd::String(opts.output_path), idx);
        CHECK(id == dstd::String(schema.id));
        CHECK(tier);

        const View vo = View::from_file(opts.output_path, dstd::String(idx));
        CHECK(count_of(vo, "where size < 50") == 50);
        CHECK(counts_by_op(vo) == counts_by_op(vs));
        StringSink sink;
        const ExportStats st =
            dftu_utils_test::ok(vo.duql("size < 50").sink_json(sink).get());
        CHECK(sink.lines().size() == 50);
        CHECK(st.chunks_skipped > 0);
    }

    TEST_CASE(
        "an index built on the way keeps the values of lines a DOM parse "
        "refuses") {
        TestEnvironment env(10);
        const dstd::Vector<dstd::String> lines = {
            R"({"k":1,"inf":1e400,"s":"a"})",
            R"({"k":2,"s":"a\qb"})",
            dstd::String("{\"k\":3,\"s\":\"b\xff"
                         "d\"}"),
            R"({"k":4,"b":123456789012345678901234567890,"s":"c"})",
            R"({"k":5,"inf":-1e400,"s":"d"})",
            R"({"k":6,"s":"e"})"};
        const dstd::String gz = write_gz(env, "ride", lines, 40);
        const dstd::String idx =
            dstd::String(determine_index_path(gz, env.dir() + "/ri"));
        const View v = View::from_file(dstd::String(gz), dstd::String(idx))
                           .record_schema("generic");
        (void)v.select({"k"}).collect().get();
        REQUIRE_FALSE(index_state(gz, idx).second);
        StringSink all;
        (void)v.sink_json(all).get();
        CHECK(all.lines().size() == 6);
        REQUIRE(index_state(gz, idx).second);
        for (const char* value : {"a", R"(a\qb)", "c", "d", "e"}) {
            CAPTURE(dstd::String(value));
            CHECK(bloom_has(idx, gz, "s", value));
        }
    }

    TEST_CASE(
        "an indexed export keeps the values of lines a DOM parse refuses") {
        TestEnvironment env(10);
        const dstd::Vector<dstd::String> lines = {
            R"({"k":1,"inf":1e400,"s":"a"})",
            R"({"k":2,"s":"a\qb"})",
            dstd::String("{\"k\":3,\"s\":\"b\xff"
                         "d\"}"),
            R"({"k":4,"b":123456789012345678901234567890,"s":"c"})",
            R"({"k":5,"inf":-1e400,"s":"d"})",
            R"({"k":6,"s":"e"})"};
        const dstd::String src = write_indexed(env, "src", lines, 1 << 20);
        const View vs =
            View::from_file(dstd::String(src),
                            dstd::String(determine_index_path(src, "")))
                .record_schema("generic");
        TraceWriteOptions opts;
        opts.output_path = env.dir() + "/out.ndjson.gz";
        opts.build_index = true;
        opts.member_size = 40;
        const ExportStats st = dftu_utils_test::ok(vs.sink_trace(opts).get());
        CHECK(st.lines_invalid == 0);

        const dstd::String idx =
            dstd::String(determine_index_path(opts.output_path, ""));
        for (const char* value : {"a", R"(a\qb)", "c", "d", "e"}) {
            CAPTURE(dstd::String(value));
            CHECK(bloom_has(idx, opts.output_path, "s", value));
        }
    }

    TEST_CASE("a collect keeps lines nested far past the depth limit") {
        auto deep = [](std::size_t n, bool object) {
            dstd::String t;
            for (std::size_t i = 0; i < n; ++i) t += object ? R"({"a":)" : "[";
            t += "1";
            for (std::size_t i = 0; i < n; ++i) t += object ? "}" : "]";
            return t;
        };
        for (const std::size_t depth :
             {std::size_t{100000}, std::size_t{1000000}}) {
            CAPTURE(depth);
            // An object line of a million levels is past the 4 MiB line limit.
            const dstd::String mid =
                deep(std::min<std::size_t>(depth, 600000), true);
            const dstd::String arr = deep(depth, false);
            auto ev = [](const char* name, const dstd::String& extra) {
                return R"({"ph":"X","name":")" + dstd::String(name) +
                       R"(","cat":"c","pid":1,"tid":1,"ts":10,"dur":2)" +
                       extra + "}";
            };
            const dstd::Vector<dstd::String> lines = {
                ev("plain", R"(,"args":{"k":1})"),
                ev("object", R"(,"args":{"k":)" + mid + "}"),
                ev("array", R"(,"args":{"k":)" + arr + "}"),
                ev("top", R"(,"deep":)" + arr)};
            TestEnvironment env(10);
            const dstd::String gz = write_gz(env, "deep", lines, 1 << 24);
            const View v = View::from_file(
                dstd::String(gz),
                dstd::String(determine_index_path(gz, env.dir() + "/di")));
            const dataframe::DataFrame f =
                dftu_utils_test::ok(v.collect().get());
            REQUIRE(f.num_rows() == 4);
            dstd::Map<dstd::String, std::size_t> longest;
            for (std::int64_t r = 0; r < f.num_rows(); ++r) {
                const dstd::String name = bstr(f, r, "name");
                for (std::size_t c = 0; c < f.names.size(); ++c) {
                    if (f.columns[c].type() != dataframe::TypeId::String)
                        continue;
                    longest[name] = std::max(
                        longest[name],
                        dstd::String(f.columns[c].string_at(r)).size());
                }
            }
            CHECK(longest["object"] > 1000);
            CHECK(longest["array"] > 1000);
        }
    }

    TEST_CASE("a path collect keeps records nested far past the depth limit") {
        for (const std::size_t depth :
             {std::size_t{100000}, std::size_t{1000000}}) {
            CAPTURE(depth);
            dstd::String mid, arr;
            const std::size_t object_depth =
                std::min<std::size_t>(depth, 600000);
            for (std::size_t i = 0; i < depth; ++i) {
                if (i < object_depth) mid += R"({"a":)";
                arr += "[";
            }
            mid += "1";
            arr += "1";
            for (std::size_t i = 0; i < depth; ++i) {
                if (i < object_depth) mid += "}";
                arr += "]";
            }
            const dstd::Vector<dstd::String> lines = {
                R"({"k":1,"s":"plain"})",
                R"({"k":2,"s":"obj","v":)" + mid + "}",
                R"({"k":3,"s":"arr","v":)" + arr + "}"};
            TestEnvironment env(10);
            const dstd::String gz = write_gz(env, "pdeep", lines, 1 << 24);
            const View v =
                View::from_file(
                    dstd::String(gz),
                    dstd::String(determine_index_path(gz, env.dir() + "/pi")))
                    .record_schema("generic");
            const dataframe::DataFrame f =
                dftu_utils_test::ok(v.collect().get());
            REQUIRE(f.num_rows() == 3);
            std::size_t long_texts = 0;
            for (std::int64_t r = 0; r < f.num_rows(); ++r)
                for (std::size_t c = 0; c < f.names.size(); ++c)
                    if (f.columns[c].type() == dataframe::TypeId::String &&
                        dstd::String(f.columns[c].string_at(r)).size() > 1000)
                        ++long_texts;
            CHECK(long_texts == 2);
        }
    }

    TEST_CASE("a path-schema file with no index is indexed on first touch") {
        const index::RecordSchema& schema = path_roles();
        TestEnvironment env(10);
        const auto lines = path_role_lines();
        const dstd::String ref =
            write_indexed(env, "ref", lines, 1024, schema.id);
        const View vr = View::from_file(
            dstd::String(ref),
            dstd::String(dstd::String(determine_index_path(ref, ""))));

        const dstd::String agg = write_gz(env, "agg", lines, 1024);
        const dstd::String agg_idx =
            dstd::String(determine_index_path(agg, env.dir() + "/agg_index"));
        REQUIRE_FALSE(dstd::fs::exists(agg_idx).value());
        const View va =
            View::from_file(dstd::String(agg), dstd::String(agg_idx));
        CHECK(counts_by_op(va) == counts_by_op(vr));
        const auto [agg_id, agg_tier] = index_state(agg, agg_idx);
        CHECK(agg_id == dstd::String(schema.id));
        CHECK(agg_tier);
        CHECK(count_of(va, "where size < 50") == 50);

        const dstd::String dump = write_gz(env, "dump", lines, 1024);
        const dstd::String dump_idx =
            dstd::String(determine_index_path(dump, env.dir() + "/dump_index"));
        REQUIRE_FALSE(dstd::fs::exists(dump_idx).value());
        StringSink sink;
        (void)View::from_file(dstd::String(dump), dstd::String(dump_idx))
            .sink_json(sink)
            .get();
        CHECK(sink.lines().size() == lines.size());
        const auto [dump_id, dump_tier] = index_state(dump, dump_idx);
        CHECK(dump_id == dstd::String(schema.id));
        CHECK(dump_tier);
        const View vd =
            View::from_file(dstd::String(dump), dstd::String(dump_idx));
        CHECK(counts_by_op(vd) == counts_by_op(vr));
        StringSink pruned;
        const ExportStats st =
            dftu_utils_test::ok(vd.duql("size < 50").sink_json(pruned).get());
        CHECK(pruned.lines().size() == 50);
        CHECK(st.chunks_skipped > 0);

        // A time window takes no bootstrap; the next whole read builds the
        // tier along the way.
        const dstd::String late = write_gz(env, "late", lines, 1024);
        const dstd::String late_idx =
            dstd::String(determine_index_path(late, env.dir() + "/late_index"));
        const View vl =
            View::from_file(dstd::String(late), dstd::String(late_idx));
        CHECK(count_of(vl, "time_range 1000 .. 2000") == 100);
        CHECK_FALSE(index_state(late, late_idx).second);
        StringSink whole;
        (void)vl.sink_json(whole).get();
        CHECK(whole.lines().size() == lines.size());
        CHECK(index_state(late, late_idx).second);
        CHECK(count_of(vl, "where size < 50") == 50);
    }

    TEST_CASE("counters of path records take the lane from its role") {
        const index::RecordSchema& schema = path_roles();
        TestEnvironment env(10);
        const dstd::String gz =
            write_indexed(env, "ctr", path_role_lines(), 1024, schema.id);
        StringSink sink;
        (void)View::from_file(
            dstd::String(gz),
            dstd::String(dstd::String(determine_index_path(gz, ""))))
            .group_by({GroupKey::pid(), GroupKey::tid()})
            .agg({{AggOp::Count, "", "n"}})
            .sink_counters(sink)
            .get();
        const auto lines = sink.lines();
        REQUIRE(lines.size() == 4);
        for (int t = 0; t < 4; ++t) {
            const dstd::String pid =
                dstd::to_string(index::entity_id("h" + dstd::to_string(t % 2)));
            const dstd::String tid =
                dstd::to_string(index::entity_id("t" + dstd::to_string(t)));
            CHECK(count_containing(
                      lines, "\"pid\":" + pid + ",\"tid\":" + tid + ",") == 1);
        }
    }

    TEST_CASE("an aggregate of an expression keeps its keys and lists") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        for (int i = 0; i < 40; ++i)
            lines.push_back(R"({"k":")" + dstd::String(i % 2 ? "a" : "b") +
                            R"(","v":)" + dstd::to_string(i) + R"(,"tags":[)" +
                            dstd::String(i % 4 ? "1,2" : "3") +
                            R"(],"objs":[{"x":1},{"x":2}]})");
        const dstd::String gz = write_indexed(env, "exprs", lines, 1024);
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("generic");
        const dataframe::DataFrame f = run(
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
        const dataframe::DataFrame o =
            run(v.duql("agg { n = sum(len(objs)) }").lazy().collect());
        CHECK(bnum(o, 0, "n") == 80);
    }

    TEST_CASE("a field of mixed JSON types is a JSON column") {
        TestEnvironment env(10);
        dstd::Vector<dstd::String> lines;
        const char* retry[] = {"0", "1", "2.5", R"("3")", "3"};
        for (int i = 0; i < 50; ++i)
            lines.push_back(R"({"k":)" + dstd::to_string(i) + R"(,"retry":)" +
                            retry[i % 5] + "}");
        const dstd::String gz = write_indexed(env, "mixed", lines, 256);
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("generic");
        const dataframe::DataFrame rows =
            run(v.duql("sort k | take 5 | select retry").lazy().collect());
        const dataframe::Series& col = rows.columns[rows.column_index("retry")];
        REQUIRE(col.is_json());
        const dstd::Vector<dstd::String> want{"0", "1", "2.5", R"("3")", "3"};
        for (std::int64_t r = 0; r < 5; ++r)
            CHECK(col.string_at(r) == want[static_cast<std::size_t>(r)]);

        const dataframe::DataFrame keys =
            run(v.duql("group retry { n = count() } | sort retry")
                    .lazy()
                    .collect());
        const dataframe::Series& key = keys.columns[keys.column_index("retry")];
        CHECK(key.is_json());
        dstd::Map<dstd::String, std::int64_t> by_key;
        for (std::int64_t r = 0; r < keys.num_rows(); ++r)
            by_key[dstd::String(key.string_at(r))] =
                static_cast<std::int64_t>(bnum(keys, r, "n"));
        CHECK(
            by_key ==
            dstd::Map<dstd::String, std::int64_t>{
                {"0", 10}, {"1", 10}, {"2.5", 10}, {R"("3")", 10}, {"3", 10}});
        // A field of one type keeps it.
        CHECK_FALSE(
            run(v.duql("select k").lazy().collect()).columns[0].is_json());
    }

    TEST_CASE(
        "batched path rows keep lines with a huge number, a bad escape or "
        "bad bytes") {
        TestEnvironment env(10);
        const dstd::Vector<dstd::String> lines = {
            R"({"k":1,"inf":1e400,"s":"a"})",
            R"({"k":2,"s":"a\qb"})",
            dstd::String("{\"k\":3,\"s\":\"b\xff"
                         "d\"}"),
            R"({"k":4,"b":123456789012345678901234567890,"s":"c"})",
            R"({"k":5,"inf":-1e400,"s":"d"})",
            R"({"k":6,"s":"e"})"};
        const dstd::String gz = write_indexed(env, "kept", lines, 1 << 20);
        const View v =
            View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))))
                .record_schema("generic");
        const auto frames = run(dataframe::collect_all(
            {v.duql("select k, s, inf, b | sort k").lazy(),
             v.select({"k"}).lazy()}));
        const dataframe::DataFrame& f = frames[0];
        REQUIRE(f.num_rows() == 6);
        CHECK(frames[1].num_rows() == 6);
        dstd::Map<std::int64_t, std::int64_t> row_of;
        for (std::int64_t r = 0; r < f.num_rows(); ++r)
            row_of[static_cast<std::int64_t>(bnum(f, r, "k"))] = r;
        REQUIRE(row_of.size() == 6);
        CHECK(bstr(f, row_of[1], "s") == "a");
        CHECK(bstr(f, row_of[2], "s") == R"(a\qb)");
        CHECK(bstr(f, row_of[3], "s") == "b?d");
        CHECK(bnum(f, row_of[1], "inf") ==
              std::numeric_limits<double>::infinity());
        CHECK(bnum(f, row_of[5], "inf") ==
              -std::numeric_limits<double>::infinity());
        CHECK(bstr(f, row_of[4], "b") == "123456789012345678901234567890");
    }

    TEST_CASE(
        "a batched path call tree keeps lines with a huge number or a bad "
        "escape") {
        index::register_schema(DFT_ROLES, "test");
        TestEnvironment env(10);
        const dstd::Vector<dstd::String> lines = {
            R"({"name":"a","cat":"P","pid":1,"tid":1,"ts":100,"dur":50,"x":1e400,"y":123456789012345678901234567890})",
            R"({"name":"b\q","cat":"P","pid":1,"tid":1,"ts":110,"dur":10})",
            dstd::String("{\"name\":\"c\",\"cat\":\"P\",\"pid\":1,"
                         "\"tid\":1,\"ts\":120,\"dur\":5,\"note\":\"p\xff"
                         "q\"}")};
        const dstd::String gz =
            write_indexed(env, "tree", lines, 1 << 20, "dft_roles");
        const View v = View::from_file(
            dstd::String(gz),
            dstd::String(dstd::String(determine_index_path(gz, ""))));
        const auto [tree, flame] =
            run(dataframe::collect_all(v.call_tree(), v.flamegraph()));
        REQUIRE(tree.num_rows() == 3);
        dstd::Set<dstd::String> names;
        for (std::int64_t r = 0; r < tree.num_rows(); ++r)
            names.insert(bstr(tree, r, "name"));
        CHECK(names == dstd::Set<dstd::String>{"a", R"(b\q)", "c"});
        CHECK(flame.num_rows() > 0);
    }

    TEST_CASE("sessions of a path schema equal the dftracer sessions") {
        index::register_schema(R"(
id: secs_sessions
fields:
  start: {type: float, role: time, unit: s}
  took: {type: float, role: duration, unit: ms}
  host: {type: string, role: entity}
  thread: {type: string, role: lane}
)",
                               "test");
        TestEnvironment env(10);
        dstd::Vector<dstd::String> dft;
        dstd::Vector<dstd::String> secs;
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
                R"({"host":"h)" + dstd::to_string(pid) + R"(","thread":"t)" +
                dstd::to_string(tid) + R"(","start":)" +
                dstd::String(dstd::double_text(static_cast<double>(ts) / 1e6)) +
                R"(,"took":)" +
                dstd::String(
                    dstd::double_text(static_cast<double>(dur) / 1e3)) +
                "}");
        }
        secs.push_back(R"({"host":"h1","thread":"t1","took":1})");
        const dstd::String a = write_indexed(env, "sdft", dft, 4096);
        const dstd::String b =
            write_indexed(env, "ssecs", secs, 4096, "secs_sessions");
        const View va = View::from_file(
            dstd::String(a),
            dstd::String(dstd::String(determine_index_path(a, ""))));
        const View vb = View::from_file(
            dstd::String(b),
            dstd::String(dstd::String(determine_index_path(b, ""))));
        const dataframe::DataFrame fa = run(
            va.duql("session pid, tid gap 105us max 300ms | group pid, tid, "
                    "session { n = count() } | sort pid, tid, session")
                .lazy()
                .collect());
        const dataframe::DataFrame fb = run(
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
                  "h" + dstd::to_string(static_cast<int>(bnum(fa, r, "pid"))));
            CHECK(bnum(fb, i, "session") == bnum(fa, r, "session"));
            CHECK(bnum(fb, i, "n") == bnum(fa, r, "n"));
            ++r;
        }
        CHECK(r == fa.num_rows());
        CHECK(fa.num_rows() > 8);
    }
}
