// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

constexpr int RECORDS = 3000;

const char* op_of(int i) {
    static const char* const OPS[] = {"read", "write", "close"};
    return OPS[i % 3];
}

// NDJSON records of no trace format: `op` cycles read/write/close, `lat` is
// i % 50, `io.off` is i * 4096 and `name` cycles x0..x4.
dstd::String write_ndjson(TestEnvironment& env, const dstd::String& name) {
    const dstd::String plain = env.dir() + "/" + name + ".ndjson";
    {
        dftu_utils_test::FileOut out(plain);
        for (int i = 0; i < RECORDS; ++i)
            out << R"({"op":")" << op_of(i) << R"(","host":"n)" << i % 4
                << R"(","t":)" << 1000 + i * 10 << R"(,"lat":)" << i % 50
                << R"(,"io":{"off":)" << static_cast<std::int64_t>(i) * 4096
                << R"(},"name":"x)" << i % 5 << R"("})" << "\n";
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    dstd::fs::remove(plain).discard();
    dftu_utils_test::ok(
        dftu_utils_test::ok(dftracer::utils::index::Indexer::open(
                                {dstd::String(dstd::StringView(gz))}))
            .build());
    return gz;
}

View view_of(const dstd::String& gz) {
    return View::from_file(
        dstd::String(gz),
        dstd::String(dstd::String(determine_index_path(gz, ""))));
}

dstd::Set<dstd::String> names_of(const dataframe::DataFrame& df) {
    return {df.names.begin(), df.names.end()};
}

}  // namespace

TEST_SUITE("GenericView") {
    TEST_CASE("rows are named by path and filter on paths") {
        TestEnvironment env(10);
        const auto gz = write_ndjson(env, "g");
        const dstd::Set<dstd::String> paths{"host", "io.off", "lat",
                                            "name", "op",     "t"};

        auto all = dftu_utils_test::ok(view_of(gz).collect().get());
        CHECK(all.num_rows() == RECORDS);
        CHECK(names_of(all) == paths);
        auto streamed = run(view_of(gz).lazy().collect());
        CHECK(streamed.num_rows() == RECORDS);
        CHECK(names_of(streamed) == paths);

        CHECK(dftu_utils_test::ok(
                  view_of(gz).duql(R"(op == "read")").collect().get())
                  .num_rows() == RECORDS / 3);
        std::int64_t deep = 0;
        for (int i = 0; i < RECORDS; ++i) deep += i * 4096 > 12000000 ? 1 : 0;
        CHECK(dftu_utils_test::ok(
                  view_of(gz).duql("io.off > 12000000").collect().get())
                  .num_rows() == deep);

        dstd::Set<dstd::String> schema;
        for (const auto& c : dftu_utils_test::ok(view_of(gz).column_info()))
            schema.insert(dstd::String(c.name));
        CHECK(schema == paths);
    }

    TEST_CASE("select, sort and topk read paths") {
        TestEnvironment env(10);
        const auto gz = write_ndjson(env, "s");
        auto sel =
            run(view_of(gz).select({"op", "lat", "io.off"}).lazy().collect());
        CHECK(sel.names == dstd::Vector<dstd::String>{"op", "lat", "io.off"});
        CHECK(sel.num_rows() == RECORDS);

        auto top = run(view_of(gz).topk("lat", 3).lazy().collect());
        REQUIRE(top.num_rows() == 3);
        for (std::int64_t r = 0; r < 3; ++r) CHECK(bnum(top, r, "lat") == 49);

        auto sorted =
            run(view_of(gz).sort_by("io.off", true).head(1).lazy().collect());
        REQUIRE(sorted.num_rows() == 1);
        CHECK(bnum(sorted, 0, "io.off") == (RECORDS - 1) * 4096.0);
    }

    TEST_CASE("group_by and aggregates read paths") {
        TestEnvironment env(10);
        const auto gz = write_ndjson(env, "a");
        dstd::Map<dstd::String, double> sum;
        for (int i = 0; i < RECORDS; ++i) sum[op_of(i)] += i % 50;

        for (bool streamed : {false, true}) {
            CAPTURE(streamed);
            View v = view_of(gz)
                         .group_by({GroupKey::field("op")})
                         .agg({{AggOp::Count, "", "n"},
                               {AggOp::Sum, "lat", "s"},
                               {AggOp::Max, "io.off", "m"}});
            auto df = streamed ? run(v.lazy().collect())
                               : dftu_utils_test::ok(v.collect().get());
            REQUIRE(df.num_rows() == 3);
            double total = 0;
            for (std::int64_t r = 0; r < 3; ++r) {
                const dstd::String op = bstr(df, r, "op");
                CAPTURE(op);
                CHECK(bnum(df, r, "s") == sum[op]);
                total += bnum(df, r, "n");
            }
            CHECK(total == RECORDS);
        }

        // `name` is a path of these records, not dftracer's event name.
        auto by_name = dftu_utils_test::ok(view_of(gz)
                                               .group_by({GroupKey::name()})
                                               .agg({{AggOp::Count, "", "n"}})
                                               .collect()
                                               .get());
        CHECK(by_name.num_rows() == 5);
        CHECK(bhas(by_name, "name"));
    }

    TEST_CASE("trace operations need roles the schema binds") {
        TestEnvironment env(10);
        const auto gz = write_ndjson(env, "r");
        auto fails_with = [](auto&& fn, const dstd::String& text) {
            const auto status = fn().status();
            REQUIRE_FALSE(status.has_value());
            const dstd::String what = status.error().c_str();
            CAPTURE(what);
            CHECK(what.find(text) != dstd::String::npos);
        };
        fails_with([&] { return view_of(gz).time_bucket(100); },
                   "time_bucket needs a time role; schema generic");
        fails_with([&] { return view_of(gz).time_range(0, 100); },
                   "time_range needs a time role");
        fails_with([&] { return view_of(gz).call_tree(); },
                   "call_tree needs a time role");
        fails_with([&] { return view_of(gz).agg({{AggOp::Busy, "", "b"}}); },
                   "occupancy aggregate needs a time role");
        fails_with([&] { return view_of(gz).group_by({GroupKey::io_cat()}); },
                   "group key io_cat needs dftracer events");
    }

    TEST_CASE("a View reads files of one schema") {
        TestEnvironment env(10);
        const auto gz = write_ndjson(env, "m");
        const auto dft = create_mixed_trace(env, 5, 5);
        dftu_utils_test::ok(
            dftu_utils_test::ok(dftracer::utils::index::Indexer::open(
                                    {dstd::String(dstd::StringView(dft))}))
                .build());
        View v = View::from_files(
            {{dstd::String(gz),
              dstd::String(dstd::String(determine_index_path(gz, "")))},
             {dstd::String(dft),
              dstd::String(dstd::String(determine_index_path(dft, "")))}});
        const auto collected = v.collect().get();
        REQUIRE_FALSE(collected.has_value());
        const dstd::String what = collected.error().c_str();
        CAPTURE(what);
        CHECK(what.find("dftracer") != dstd::String::npos);
        CHECK(what.find("generic") != dstd::String::npos);

        const auto status = v.schema_status();
        REQUIRE_FALSE(status.has_value());
        CHECK(
            dstd::String(status.error().message()).find("one record schema") !=
            dstd::String::npos);
        CHECK(v.lazy().output_schema().fields.empty());
        const auto names =
            dftu_utils_test::ok(v.record_schema("generic").columns());
        CHECK(v.record_schema("generic").schema_status().has_value());
        CHECK(std::find(names.begin(), names.end(), "lat") != names.end());
        CHECK(std::find(names.begin(), names.end(), "dur") == names.end());
    }

    TEST_CASE("every terminal over files of two schemas returns the error") {
        TestEnvironment env(10);
        const auto gz = write_ndjson(env, "t");
        const auto dft = create_mixed_trace(env, 5, 5);
        dftu_utils_test::ok(
            dftu_utils_test::ok(dftracer::utils::index::Indexer::open(
                                    {dstd::String(dstd::StringView(dft))}))
                .build());
        const View v = View::from_files(
            {{dstd::String(gz),
              dstd::String(dstd::String(determine_index_path(gz, "")))},
             {dstd::String(dft),
              dstd::String(dstd::String(determine_index_path(dft, "")))}});
        const View counted = v.group_by({GroupKey::field("name")})
                                 .agg({{AggOp::Count, "", "n"}});
        auto mixed = [](const auto& result) {
            REQUIRE_FALSE(result.has_value());
            const dstd::String what = dstd::String(result.error().message());
            CAPTURE(what);
            CHECK(what.find("one record schema") != dstd::String::npos);
        };
        StringSink sink;
        mixed(counted.merge_partials({}));
        mixed(counted.merge_counter_partials({}, sink));
        mixed(counted.reconstruct_if_cached());
        mixed(counted.materialize_dir());
        mixed(counted.mv_source());
        mixed(v.export_would_bootstrap());
        mixed(counted.collect_would_bootstrap());
        mixed(counted.materialize_partials({}).get());
        mixed(counted.aggregate_partial().collect().get());
        mixed(counted.collect().get());
        mixed(v.collect_typed().get());
        mixed(v.materialize().get());
        mixed(counted.sink_counters(sink).get());
    }

    TEST_CASE("from_directory finds JSON lines files") {
        TestEnvironment env(10);
        write_ndjson(env, "d");
        View v = run(View::from_directory(dstd::String(env.dir())));
        CHECK(dftu_utils_test::ok(v.collect().get()).num_rows() == RECORDS);
    }

    TEST_CASE("files without records take no vote") {
        TestEnvironment env(10);
        const auto dft = create_mixed_trace(env, 5, 5);
        const dstd::Path dir(dstd::PathView(dft).parent().str());
        dftu_utils_test::FileOut(dir / "rank_1.pfw.gz");
        {
            const dstd::String meta((dir / "rank_2.pfw").str());
            dftu_utils_test::FileOut(meta)
                << R"({"name":"HH","cat":"dftracer","pid":2,"tid":2,)"
                   R"("ph":"M","args":{"name":"host","value":"h1"}})"
                << "\n";
            REQUIRE(dftu_utils_test::compress_file_to_gzip(meta, meta + ".gz"));
            dstd::fs::remove(meta).discard();
        }
        // Unindexed, then indexed with each file's own detection.
        auto rows = [&] {
            return dftu_utils_test::ok(run(View::from_directory(dstd::String(
                                               dstd::String(dir.str()))))
                                           .collect()
                                           .get())
                .num_rows();
        };
        CHECK(rows() == 10);
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                dftracer::utils::index::Indexer::open(
                    {dstd::String(dstd::StringView(dstd::String(dir.str())))}))
                .build());
        CHECK(rows() == 10);

        TestEnvironment empty_env(10);
        dftu_utils_test::FileOut(empty_env.dir() + "/a.pfw.gz");
        dftu_utils_test::FileOut(empty_env.dir() + "/b.pfw.gz");
        View empty = run(View::from_directory(dstd::String(empty_env.dir())));
        CHECK(dftu_utils_test::ok(empty.collect().get()).num_rows() == 0);
    }

    TEST_CASE(
        "from_directory reads a non-gzip .gz file as an error, not as empty") {
        TestEnvironment env(10);
        dftu_utils_test::FileOut(env.dir() + "/plain.jsonl.gz")
            << R"({"k":1})" << "\n";
        const auto collected =
            run(View::from_directory(dstd::String(env.dir()))).collect().get();
        REQUIRE_FALSE(collected.has_value());
        CAPTURE(dstd::String(collected.error().c_str()));
        CHECK(dstd::String(collected.error().c_str())
                  .find("plain.jsonl.gz is not gzip") != dstd::String::npos);
    }
}
