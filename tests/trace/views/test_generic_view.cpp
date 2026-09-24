#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace {

constexpr int RECORDS = 3000;

const char* op_of(int i) {
    static const char* const OPS[] = {"read", "write", "close"};
    return OPS[i % 3];
}

// NDJSON records of no trace format: `op` cycles read/write/close, `lat` is
// i % 50, `io.off` is i * 4096 and `name` cycles x0..x4.
std::string write_ndjson(TestEnvironment& env, const std::string& name) {
    const std::string plain = env.get_dir() + "/" + name + ".ndjson";
    {
        std::ofstream out(plain);
        for (int i = 0; i < RECORDS; ++i)
            out << R"({"op":")" << op_of(i) << R"(","host":"n)" << i % 4
                << R"(","t":)" << 1000 + i * 10 << R"(,"lat":)" << i % 50
                << R"(,"io":{"off":)" << static_cast<std::int64_t>(i) * 4096
                << R"(},"name":"x)" << i % 5 << R"("})" << "\n";
    }
    const std::string gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    fs::remove(plain);
    dftracer::utils::index::Indexer::open({gz}).build();
    return gz;
}

View view_of(const std::string& gz) {
    return View::from_file(gz, determine_index_path(gz, ""));
}

std::set<std::string> names_of(const dataframe::DataFrame& df) {
    return {df.names.begin(), df.names.end()};
}

}  // namespace

TEST_SUITE("GenericView") {
    TEST_CASE("rows are named by path and filter on paths") {
        TestEnvironment env(10);
        const auto gz = write_ndjson(env, "g");
        const std::set<std::string> paths{"host", "io.off", "lat",
                                          "name", "op",     "t"};

        auto all = view_of(gz).collect().get();
        CHECK(all.num_rows() == RECORDS);
        CHECK(names_of(all) == paths);
        auto streamed = run(view_of(gz).lazy().collect());
        CHECK(streamed.num_rows() == RECORDS);
        CHECK(names_of(streamed) == paths);

        CHECK(view_of(gz).query(R"(op == "read")").collect().get().num_rows() ==
              RECORDS / 3);
        std::int64_t deep = 0;
        for (int i = 0; i < RECORDS; ++i) deep += i * 4096 > 12000000 ? 1 : 0;
        CHECK(
            view_of(gz).query("io.off > 12000000").collect().get().num_rows() ==
            deep);

        std::set<std::string> schema;
        for (const auto& c : view_of(gz).column_info()) schema.insert(c.name);
        CHECK(schema == paths);
    }

    TEST_CASE("select, sort and topk read paths") {
        TestEnvironment env(10);
        const auto gz = write_ndjson(env, "s");
        auto sel =
            run(view_of(gz).select({"op", "lat", "io.off"}).lazy().collect());
        CHECK(sel.names == std::vector<std::string>{"op", "lat", "io.off"});
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
        std::map<std::string, double> sum;
        for (int i = 0; i < RECORDS; ++i) sum[op_of(i)] += i % 50;

        for (bool streamed : {false, true}) {
            CAPTURE(streamed);
            View v = view_of(gz)
                         .group_by({GroupKey::field("op")})
                         .agg({{AggOp::Count, "", "n"},
                               {AggOp::Sum, "lat", "s"},
                               {AggOp::Max, "io.off", "m"}});
            auto df = streamed ? run(v.lazy().collect()) : v.collect().get();
            REQUIRE(df.num_rows() == 3);
            double total = 0;
            for (std::int64_t r = 0; r < 3; ++r) {
                const std::string op = bstr(df, r, "op");
                CAPTURE(op);
                CHECK(bnum(df, r, "s") == sum[op]);
                total += bnum(df, r, "n");
            }
            CHECK(total == RECORDS);
        }

        // `name` is a path of these records, not dftracer's event name.
        auto by_name = view_of(gz)
                           .group_by({GroupKey::name()})
                           .agg({{AggOp::Count, "", "n"}})
                           .collect()
                           .get();
        CHECK(by_name.num_rows() == 5);
        CHECK(bhas(by_name, "name"));
    }

    TEST_CASE("trace operations need roles the schema binds") {
        TestEnvironment env(10);
        const auto gz = write_ndjson(env, "r");
        auto fails_with = [](auto&& fn, const std::string& text) {
            try {
                fn();
            } catch (const std::exception& e) {
                CAPTURE(e.what());
                CHECK(std::string(e.what()).find(text) != std::string::npos);
                return;
            }
            FAIL("no error");
        };
        fails_with([&] { (void)view_of(gz).time_bucket(100); },
                   "time_bucket needs a time role; schema generic");
        fails_with([&] { (void)view_of(gz).time_range(0, 100); },
                   "time_range needs a time role");
        fails_with([&] { (void)view_of(gz).call_tree(); },
                   "call_tree needs a time role");
        fails_with([&] { (void)view_of(gz).agg({{AggOp::Busy, "", "b"}}); },
                   "occupancy aggregate needs a time role");
        fails_with([&] { (void)view_of(gz).group_by({GroupKey::io_cat()}); },
                   "group key io_cat needs dftracer events");
    }

    TEST_CASE("a View reads files of one schema") {
        TestEnvironment env(10);
        const auto gz = write_ndjson(env, "m");
        const auto dft = create_mixed_trace(env, 5, 5);
        dftracer::utils::index::Indexer::open({dft}).build();
        View v = View::from_files({{gz, determine_index_path(gz, "")},
                                   {dft, determine_index_path(dft, "")}});
        try {
            (void)v.collect().get();
            FAIL("no error");
        } catch (const std::exception& e) {
            const std::string what = e.what();
            CAPTURE(what);
            CHECK(what.find("dftracer") != std::string::npos);
            CHECK(what.find("generic") != std::string::npos);
        }
    }

    TEST_CASE("from_directory finds JSON lines files") {
        TestEnvironment env(10);
        write_ndjson(env, "d");
        View v = run(View::from_directory(env.get_dir()));
        CHECK(v.collect().get().num_rows() == RECORDS);
    }
}
