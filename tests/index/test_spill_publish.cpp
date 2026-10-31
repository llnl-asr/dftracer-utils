#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
namespace idx = dftracer::utils::index;
namespace build = dftracer::utils::index::build;

namespace {

constexpr std::uint64_t MIB = std::uint64_t{1} << 20;

// Small gzip members, so the file has about a thousand chunks and a small
// budget forces the bloom tier to spill.
std::string write_trace(const std::string& dir, const std::string& name,
                        int events) {
    std::ostringstream out;
    out << "[\n";
    std::uint64_t ts = 1000000;
    for (int i = 0; i < events; ++i) {
        ts += 7 + static_cast<std::uint64_t>(i % 13);
        out << R"({"id":)" << i << R"(,"name":"op)" << (i / 50) % 7
            << R"(","cat":"C)" << i % 3 << R"(","pid":)" << 1 + i % 4
            << R"(,"tid":)" << 10 + i % 4 << R"(,"ph":"X","ts":)" << ts
            << R"(,"dur":)" << 5 + i % 97 << R"(,"args":{"k)" << i % 600
            << R"(":)" << i % 50 << "}}\n";
    }
    out << "]\n";
    const auto plain = dir + "/" + name + ".pfw";
    {
        std::ofstream(plain) << out.str();
    }
    const auto gz = plain + ".gz";
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, 2048));
    fs::remove(plain);
    return gz;
}

build::IndexBuildBatchResult run_build(const std::string& file,
                                       const std::string& index_dir,
                                       std::uint64_t memory_budget) {
    build::IndexBuildBatchResult out;
    Runtime rt(4);
    auto task = run_coro_scope(
        rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
            auto config = std::make_shared<build::IndexBuildBatchConfig>();
            config->file_paths = {file};
            config->index_dir = index_dir;
            config->parallelism = 2;
            config->memory_budget = memory_budget;
            out = co_await build::BatchBuilder::process(&scope,
                                                        std::move(config));
        });
    rt.submit(std::move(task), "test-spill-publish").wait();
    rt.shutdown();
    for (const auto& result : out.results)
        if (!result.success) MESSAGE("build failed: " << result.error_message);
    return out;
}

std::string index_root(const std::string& file, const std::string& dir) {
    return idx::store::internal::normalize_index_root(
        trace::internal::determine_index_path(file, dir));
}

}  // namespace

TEST_SUITE("SpillPublish") {
    TEST_CASE("a spilled file is never visible without its tier") {
        dftu_utils_test::TestEnvironment env(10);
        const auto file = write_trace(env.get_dir(), "spill", 12000);
        const auto dir = env.get_dir() + "/idx";
        const auto logical = idx::store::internal::get_logical_path(file);
        auto index = idx::Index::open(index_root(file, dir));
        REQUIRE(index);

        std::atomic<bool> stop{false};
        std::atomic<int> visible{0};
        std::atomic<int> without_tier{0};
        std::thread poller([&] {
            while (!stop.load()) {
                auto snapshot = index->snapshot();
                if (snapshot) {
                    const auto found = snapshot->file(logical);
                    if (found) {
                        visible.fetch_add(1);
                        const auto stats = idx::chunk_stats(*snapshot, *found);
                        if (!stats || stats->empty()) without_tier.fetch_add(1);
                    }
                }
                std::this_thread::yield();
            }
        });
        const auto built = run_build(file, dir, 1 * MIB);
        stop.store(true);
        poller.join();

        REQUIRE(built.indexed == 1);
        CHECK(built.metrics.files_spilled == 1);
        CHECK(without_tier.load() == 0);
        auto snapshot = index->snapshot();
        REQUIRE(snapshot);
        const auto found = snapshot->file(logical);
        REQUIRE(found);
        const auto stats = idx::chunk_stats(*snapshot, *found);
        REQUIRE(stats);
        CHECK_FALSE(stats->empty());
        MESSAGE("polls that saw the file: " << visible.load());
    }

    TEST_CASE("a full rebuild drops the plugin copies it does not write") {
        for (const std::uint64_t budget :
             {memory::NO_SPILL_BUDGET, std::uint64_t{1 * MIB}}) {
            CAPTURE(budget);
            dftu_utils_test::TestEnvironment env(10);
            const auto file = write_trace(env.get_dir(), "plugin", 12000);
            const auto dir = env.get_dir() + "/idx";
            const auto logical = idx::store::internal::get_logical_path(file);
            REQUIRE(run_build(file, dir, budget).indexed == 1);

            auto index = idx::Index::open(index_root(file, dir));
            REQUIRE(index);
            {
                auto stale = idx::rebuild(*index, logical);
                REQUIRE(stale);
                idx::put_plugin(*stale, "stale", 1, 5, {{0, dstd::String("x")}},
                                std::nullopt);
                REQUIRE(stale->commit().get());
            }
            auto plugin_count = [&] {
                auto snapshot = index->snapshot();
                REQUIRE(snapshot);
                const auto found = snapshot->file(logical);
                REQUIRE(found);
                const auto copies = idx::plugins(*snapshot, *found);
                REQUIRE(copies);
                return copies->size();
            };
            REQUIRE(plugin_count() == 1);

            const auto again = run_build(file, dir, budget);
            REQUIRE(again.indexed == 1);
            if (budget != memory::NO_SPILL_BUDGET)
                CHECK(again.metrics.files_spilled == 1);
            CHECK(plugin_count() == 0);
        }
    }
}
