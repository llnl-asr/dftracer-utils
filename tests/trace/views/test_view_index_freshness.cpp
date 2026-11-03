// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <trace/views/test_view_common.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;

using namespace test_view_common;
namespace st = dftracer::utils::index::store;

namespace {

// One gzip member: the trace is a single chunk.
constexpr int OLD_EVENTS = 2000;
constexpr int NEW_EVENTS = 1500;
// A line is about 80 bytes, so these events fill about 7 members.
constexpr std::size_t MEMBER_BYTES = 64 * 1024;
constexpr int MEMBER_EVENTS = 6000;

void write_events(const std::string& gz, const char* cat, int n,
                  std::size_t member_bytes, const char* odd_cat = nullptr) {
    const std::string plain = gz + ".plain";
    {
        std::ofstream ofs(plain.c_str());
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
    (void)dstd::fs::remove(plain);
}

void rewrite_events(const std::string& gz, const char* cat, int n,
                    std::size_t member_bytes = 0) {
    const auto before = dstd::fs::mtime_ns(gz).value();
    write_events(gz, cat, n, member_bytes);
    REQUIRE(dstd::fs::set_mtime_ns(gz, before + 10'000'000'000ULL).has_value());
}

coro::CoroTask<void> build_tier(std::string gz, std::string index_dir = "") {
    dftracer::utils::index::build::ResolveAndBuildInput in;
    in.files.push_back(
        dftracer::utils::dstd::String(dftracer::utils::dstd::StringView(gz)));
    in.index_dir = std::move(index_dir);
    in.require_checkpoints = true;
    co_await coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        co_await dftracer::utils::index::build::resolve_and_build_index(
            std::move(in));
    });
}

coro::CoroTask<void> build_files(std::vector<std::string> files) {
    dftracer::utils::index::build::ResolveAndBuildInput in;
    for (const auto& file : files) in.files.emplace_back(file);
    in.require_checkpoints = true;
    co_await coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        co_await dftracer::utils::index::build::resolve_and_build_index(
            std::move(in));
    });
}

coro::CoroTask<dftracer::utils::index::build::ResolverResult> resolve_files(
    std::vector<std::string> files) {
    dftracer::utils::index::build::ResolverInput in;
    for (const auto& file : files) in.files.emplace_back(file);
    in.require_checkpoints = true;
    dftracer::utils::index::build::ResolverResult result;
    co_await coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        result = co_await dftracer::utils::index::build::Resolver{}(scope, in);
    });
    co_return result;
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
    void write(dstd::StringView) override {}
};

}  // namespace

TEST_SUITE("ViewIndexFreshness") {
    TEST_CASE("a rewritten trace gives the rows of its new content") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const std::string gz = env.get_dir() + "/t.pfw.gz";
        write_events(gz, "A", OLD_EVENTS, 0);
        (void)run(
            dir_view(env.get_dir()).agg({{AggOp::Count, "", "n"}}).collect());
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "A")") == OLD_EVENTS);
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "B")") == 0);

        rewrite_events(gz, "B", NEW_EVENTS);
        (void)run(
            dir_view(env.get_dir()).agg({{AggOp::Count, "", "n"}}).collect());
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "B")") == NEW_EVENTS);
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "A")") == 0);
    }

    TEST_CASE("an aggregation after a rewrite counts the new content") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const std::string gz = env.get_dir() + "/t.pfw.gz";
        write_events(gz, "A", OLD_EVENTS, 0);
        run(build_tier(gz));
        const auto first = by_cat(dir_view(env.get_dir()));
        CHECK(count_by(first, "a") == OLD_EVENTS);

        rewrite_events(gz, "B", NEW_EVENTS);
        const auto second = by_cat(dir_view(env.get_dir()));
        CHECK(count_by(second, "b") == NEW_EVENTS);
        CHECK(count_by(second, "a") == 0);
    }

    TEST_CASE(
        "an identical rewrite is rebuilt and then served from the index") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const std::string gz = env.get_dir() + "/t.pfw.gz";
        write_events(gz, "A", MEMBER_EVENTS, MEMBER_BYTES);
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "A")") == MEMBER_EVENTS);

        const auto before = dstd::fs::mtime_ns(gz).value();
        REQUIRE(
            dstd::fs::set_mtime_ns(gz, before + 10'000'000'000ULL).has_value());
        const auto want = static_cast<std::uint64_t>(
            st::internal::get_file_modification_time(gz));

        CHECK(rows(dir_view(env.get_dir()), R"(cat == "A")") == MEMBER_EVENTS);
        const auto snapshot = dftu_utils_test::index_snapshot(
            std::string(determine_index_path(gz, "")));
        const auto file = dftu_utils_test::require_indexed_file(snapshot, gz);
        CHECK(file.stat().mtime == want);
        CHECK(rows(dir_view(env.get_dir()), R"(cat == "B")") == 0);
    }

    TEST_CASE("a rebuild leaves the other directories of a shared root alone") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const std::string d1 = env.get_dir() + "/d1";
        const std::string d2 = env.get_dir() + "/d2";
        const std::string root = env.get_dir() + "/shared";
        REQUIRE(dstd::fs::create_directories(d1).has_value());
        REQUIRE(dstd::fs::create_directories(d2).has_value());
        const std::string f1 = d1 + "/a.pfw.gz";
        const std::string f2 = d2 + "/b.pfw.gz";
        write_events(f1, "A", OLD_EVENTS, 0);
        write_events(f2, "C", MEMBER_EVENTS, MEMBER_BYTES);
        run(build_tier(f1, root));
        run(build_tier(f2, root));
        CHECK(rows(dir_view(d1, root), R"(cat == "A")") == OLD_EVENTS);
        CHECK(rows(dir_view(d2, root), R"(cat == "C")") == MEMBER_EVENTS);

        const auto idx = std::string(determine_index_path(f1, root));

        rewrite_events(f1, "B", NEW_EVENTS);
        CHECK(rows(dir_view(d1, root), R"(cat == "B")") == NEW_EVENTS);

        const auto snapshot = dftu_utils_test::index_snapshot(idx);
        const auto other = dftu_utils_test::require_indexed_file(snapshot, f2);
        CHECK(other.stat().mtime ==
              static_cast<std::uint64_t>(
                  st::internal::get_file_modification_time(f2)));
        CHECK(rows(dir_view(d2, root), R"(cat == "C")") == MEMBER_EVENTS);
        CHECK(count_by(by_cat(dir_view(d2, root)), "c") == MEMBER_EVENTS);
    }

    TEST_CASE("the fresh check of many files flags exactly the changed ones") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        constexpr int FILES = 40;
        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i) {
            const std::string gz =
                env.get_dir() + "/t" + std::to_string(100 + i) + ".pfw.gz";
            write_events(gz, "A", 50, 0);
            files.push_back(gz);
        }
        run(build_files(files));
        const std::set<int> rewritten = {3, 17, 38};
        for (const int i : rewritten) rewrite_events(files[i], "B", 60);
        for (int i = FILES; i < FILES + 2; ++i) {
            const std::string gz =
                env.get_dir() + "/t" + std::to_string(100 + i) + ".pfw.gz";
            write_events(gz, "A", 50, 0);
            files.push_back(gz);
        }

        const auto result = run(resolve_files(files));
        CHECK(result.stale_detected);
        std::set<std::size_t> flagged;
        for (const auto& item : result.needs_checkpoint)
            flagged.insert(item.file_index);
        const std::set<std::size_t> expected = {3, 17, 38, FILES, FILES + 1};
        CHECK(flagged == expected);
        REQUIRE(result.cached.size() == FILES - rewritten.size());
        std::size_t last = 0;
        for (std::size_t i = 0; i < result.cached.size(); ++i) {
            const auto& cached = result.cached[i];
            CHECK(expected.count(cached.file_index) == 0);
            CHECK(dftracer::utils::dstd::StringView(cached.file_path) ==
                  dftracer::utils::dstd::StringView(files[cached.file_index]));
            if (i > 0) CHECK(cached.file_index > last);
            last = cached.file_index;
        }
    }

    TEST_CASE("from_file without an index path prunes with the default index") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const std::string gz = env.get_dir() + "/t.pfw.gz";
        write_events(gz, "A", MEMBER_EVENTS, MEMBER_BYTES, "RARE");
        REQUIRE(dftu_utils_test::build_index(gz));

        NullSink sink;
        const auto stats =
            View::from_file(gz).duql(R"(cat == "RARE")").sink_json(sink).get();
        CHECK(stats.events_matched == 3);
        CHECK(stats.chunks_skipped > 0);
    }
}
