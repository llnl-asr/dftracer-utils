#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "test_view_common.h"

using namespace test_view_common;
namespace st = dftracer::utils::index::store;

namespace {

void write_events(const std::string& gz, const char* cat, int n,
                  std::size_t member_bytes, const char* odd_cat = nullptr) {
    const std::string plain = gz + ".plain";
    {
        std::ofstream ofs(plain);
        ofs << "[\n";
        for (int i = 0; i < n; ++i) {
            const char* c = (odd_cat && i >= n - 3) ? odd_cat : cat;
            ofs << R"({"id":)" << i << R"(,"name":"read","cat":")" << c
                << R"(","pid":1,"tid":1,"ts":)" << (1000 + i)
                << R"(,"dur":5,"ph":"X"})" << "\n";
        }
    }
    if (member_bytes)
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(
            plain, gz, member_bytes));
    else
        REQUIRE(dftu_utils_test::compress_file_to_gzip(plain, gz));
    fs::remove(plain);
}

void rewrite_events(const std::string& gz, const char* cat, int n,
                    std::size_t member_bytes = 0) {
    const auto before = fs::last_write_time(gz);
    write_events(gz, cat, n, member_bytes);
    fs::last_write_time(gz, before + std::chrono::seconds(10));
}

coro::CoroTask<void> build_tier(std::string gz, std::string index_dir = "") {
    dftracer::utils::index::build::ResolveAndBuildInput in;
    in.files.push_back(std::move(gz));
    in.index_dir = std::move(index_dir);
    in.require_checkpoints = true;
    in.require_aggregation = true;
    in.aggregation_config =
        dftracer::utils::index::schemas::dft::agg::AggregationConfig{};
    co_await dftracer::utils::run_coro_scope(
        [&](dftracer::utils::CoroScope& scope) -> coro::CoroTask<void> {
            co_await dftracer::utils::index::build::resolve_and_build_index(
                &scope, std::move(in));
        });
}

std::size_t rows(const View& v, const std::string& q) {
    return v.duql(q).collect().get().num_rows();
}

View dir_view(const std::string& dir, const std::string& root = "") {
    return run(View::from_directory(dir, root));
}

std::int64_t count_by(const dataframe::DataFrame& df, const std::string& cat) {
    for (std::int64_t i = 0; i < df.num_rows(); ++i)
        if (bstr(df, i, "cat") == cat)
            return static_cast<std::int64_t>(bnum(df, i, "n"));
    return 0;
}

dataframe::DataFrame by_cat(const View& v) {
    return v.group_by({GroupKey::cat()})
        .agg({{AggOp::Count, "", "n"}})
        .collect()
        .get();
}

struct NullSink : ExportSink {
    void write(std::string_view) override {}
};

}  // namespace

TEST_SUITE("ViewIndexFreshness") {
    TEST_CASE("a rewritten trace gives the rows of its new content") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const std::string gz = env.get_dir() + "/t.pfw.gz";
        write_events(gz, "A", 200000, 0);
        (void)run(
            dir_view(env.get_dir()).agg({{AggOp::Count, "", "n"}}).collect());
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "A")") == 200000);
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "B")") == 0);

        rewrite_events(gz, "B", 150000);
        (void)run(
            dir_view(env.get_dir()).agg({{AggOp::Count, "", "n"}}).collect());
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "B")") == 150000);
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "A")") == 0);
    }

    TEST_CASE("an aggregation after a rewrite counts the new content") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const std::string gz = env.get_dir() + "/t.pfw.gz";
        write_events(gz, "A", 200000, 0);
        run(build_tier(gz));
        const auto first = by_cat(dir_view(env.get_dir()));
        CHECK(count_by(first, "a") == 200000);

        rewrite_events(gz, "B", 150000);
        const auto second = by_cat(dir_view(env.get_dir()));
        CHECK(count_by(second, "b") == 150000);
        CHECK(count_by(second, "a") == 0);
    }

    TEST_CASE(
        "an identical rewrite is rebuilt and then served from the index") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const std::string gz = env.get_dir() + "/t.pfw.gz";
        write_events(gz, "A", 30000, 64 * 1024);
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "A")") == 30000);

        const auto before = fs::last_write_time(gz);
        fs::last_write_time(gz, before + std::chrono::seconds(10));
        const auto want = static_cast<std::uint64_t>(
            st::internal::get_file_modification_time(gz));

        CHECK(rows(dir_view(env.get_dir()), R"(cat == "A")") == 30000);
        st::IndexDatabase db(determine_index_path(gz, ""),
                             st::IndexOpenMode::ReadOnly);
        const auto stat = db.get_file_stat(st::internal::get_logical_path(gz));
        REQUIRE(stat);
        CHECK(stat->mtime == want);
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "B")") == 0);
    }

    TEST_CASE("a rebuild leaves the other directories of a shared root alone") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const std::string d1 = env.get_dir() + "/d1";
        const std::string d2 = env.get_dir() + "/d2";
        const std::string root = env.get_dir() + "/shared";
        fs::create_directories(d1);
        fs::create_directories(d2);
        const std::string f1 = d1 + "/a.pfw.gz";
        const std::string f2 = d2 + "/b.pfw.gz";
        write_events(f1, "A", 200000, 0);
        write_events(f2, "C", 30000, 64 * 1024);
        run(build_tier(f1, root));
        run(build_tier(f2, root));
        CHECK(rows(dir_view(d1, root), R"(cat == "A")") == 200000);
        CHECK(rows(dir_view(d2, root), R"(cat == "C")") == 30000);

        const auto idx = determine_index_path(f1, root);
        std::int64_t id2 = -1;
        {
            st::IndexDatabase db(idx, st::IndexOpenMode::ReadOnly);
            id2 = db.get_file_info_id(st::internal::get_logical_path(f2));
            REQUIRE(id2 >= 0);
        }

        rewrite_events(f1, "B", 150000);
        CHECK(rows(dir_view(d1, root), R"(cat == "B")") == 150000);

        st::IndexDatabase db(idx, st::IndexOpenMode::ReadOnly);
        CHECK(db.get_file_info_id(st::internal::get_logical_path(f2)) == id2);
        const auto stat = db.get_file_stat(st::internal::get_logical_path(f2));
        REQUIRE(stat);
        CHECK(stat->mtime == static_cast<std::uint64_t>(
                                 st::internal::get_file_modification_time(f2)));
        CHECK(rows(dir_view(d2, root), R"(cat == "C")") == 30000);
        CHECK(count_by(by_cat(dir_view(d2, root)), "c") == 30000);
    }

    TEST_CASE("from_file without an index path prunes with the default index") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const std::string gz = env.get_dir() + "/t.pfw.gz";
        write_events(gz, "A", 60000, 64 * 1024, "RARE");
        REQUIRE(dftu_utils_test::build_index(gz));

        NullSink sink;
        const auto stats =
            View::from_file(gz).duql(R"(cat == "RARE")").sink_json(sink).get();
        CHECK(stats.events_matched == 3);
        CHECK(stats.chunks_skipped > 0);
    }
}
