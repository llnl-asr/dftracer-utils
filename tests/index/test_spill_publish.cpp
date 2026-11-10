// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/index/build/aggregation_run.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <atomic>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::index;

namespace {

constexpr std::uint64_t MIB = std::uint64_t{1} << 20;

// Small gzip members, so the file has about a thousand chunks and a small
// budget forces the bloom tier to spill.
dstd::String write_trace(const dstd::String& dir, const dstd::String& name,
                         int events) {
    dstd::String out;
    out += "[\n";
    std::uint64_t ts = 1000000;
    for (int i = 0; i < events; ++i) {
        ts += 7 + static_cast<std::uint64_t>(i % 13);
        out += common::str_cat(R"({"id":)", i, R"(,"name":"op)", (i / 50) % 7,
                               R"(","cat":"C)", i % 3, R"(","pid":)", 1 + i % 4,
                               R"(,"tid":)", 10 + i % 4, R"(,"ph":"X","ts":)",
                               ts, R"(,"dur":)", 5 + i % 97, R"(,"args":{"k)",
                               i % 600, R"(":)", i % 50, "}}\n");
    }
    out += "]\n";
    const auto plain = dir + "/" + name + ".pfw";
    {
        dstd::fs::write_file(dstd::PathView(plain), out).discard();
    }
    const auto gz = plain + ".gz";
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, 2048));
    dstd::fs::remove(plain).discard();
    return gz;
}

index::build::IndexBuildBatchResult run_build(
    const dstd::String& file, const dstd::String& index_dir,
    std::uint64_t memory_budget,
    index::build::IndexBuildBatchConfig::AggFoldFactory agg_fold_factory = {}) {
    index::build::IndexBuildBatchResult out;
    Runtime rt(4);
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        auto config = dstd::make_shared<index::build::IndexBuildBatchConfig>();
        config->file_paths = {dstd::String(dstd::StringView(file))};
        config->index_dir = index_dir;
        config->parallelism = 2;
        config->memory_budget = memory_budget;
        config->agg_fold_factory = std::move(agg_fold_factory);
        out = co_await index::build::BatchBuilder::process(std::move(config));
    });
    dftu_utils_test::ok(
        rt.submit(std::move(task), "test-spill-publish").wait());
    rt.shutdown();
    for (const auto& result : out.results)
        if (!result.success) MESSAGE("build failed: " << result.error_message);
    return out;
}

dstd::String index_root(const dstd::String& file, const dstd::String& dir) {
    return dstd::String(index::normalize_index_root(
        trace::internal::determine_index_path(file, dir)));
}

}  // namespace

TEST_SUITE("SpillPublish") {
    TEST_CASE("a spilled file is never visible without its tier") {
        dftu_utils_test::TestEnvironment env(10);
        const auto file = write_trace(env.dir(), "spill", 12000);
        const auto dir = env.dir() + "/idx";
        const auto logical = index::store::internal::get_logical_path(file);
        auto index = index::Index::open(index_root(file, dir));
        REQUIRE(index);

        std::atomic<bool> stop{false};
        std::atomic<int> visible{0};
        std::atomic<int> without_tier{0};
        dstd::Thread poller([&] {
            while (!stop.load()) {
                auto snapshot = index->snapshot();
                if (snapshot) {
                    const auto found = snapshot->file(logical);
                    if (found) {
                        visible.fetch_add(1);
                        const auto stats =
                            index::chunk_stats(*snapshot, *found);
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
        const auto stats = index::chunk_stats(*snapshot, *found);
        REQUIRE(stats);
        CHECK_FALSE(stats->empty());
        MESSAGE("polls that saw the file: " << visible.load());
    }

    TEST_CASE("a file written through is never visible without its rows") {
        dftu_utils_test::TestEnvironment env(10);
        const auto file = write_trace(env.dir(), "through", 12000);
        const auto dir = env.dir() + "/idx";
        const auto logical = index::store::internal::get_logical_path(file);
        const auto path =
            dstd::String(trace::internal::determine_index_path(file, dir));
        auto index = index::Index::open(index_root(file, dir));
        REQUIRE(index);

        schemas::dft::agg::AggregationConfig config;
        config.time_interval_us = 100'000;
        auto run =
            dftu_utils_test::ok(index::build::start_aggregation(path, config));
        const auto run_bytes = schemas::dft::agg::g_agg_run_bytes.exchange(1);
#if DFTRACER_UTILS_TEST_PROBES
        const auto staged = schemas::dft::agg::g_agg_runs_staged.load();
#endif

        std::atomic<bool> stop{false};
        std::atomic<int> visible{0};
        std::atomic<int> without_rows{0};
        dstd::Thread poller([&] {
            while (!stop.load()) {
                auto read = index::plan::open_snapshot(path);
                if (read) {
                    const auto ref = read->file(logical);
                    if (ref) {
                        visible.fetch_add(1);
                        std::size_t rows = 0;
                        auto scanned = index::for_each_agg_partial(
                            *read, *ref,
                            [&](std::uint64_t, std::uint32_t, std::uint32_t,
                                std::uint32_t, std::uint32_t,
                                schemas::dft::agg::PartialKind,
                                dstd::Span<const std::uint8_t>) {
                                ++rows;
                                return false;
                            });
                        if (!scanned || rows == 0) without_rows.fetch_add(1);
                    }
                }
                std::this_thread::yield();
            }
        });
        const auto built =
            run_build(file, dir, 1 * MIB, std::move(run.factory));
        stop.store(true);
        poller.join();
        schemas::dft::agg::g_agg_run_bytes.store(run_bytes);

        REQUIRE(built.indexed == 1);
#if DFTRACER_UTILS_TEST_PROBES
        CHECK(schemas::dft::agg::g_agg_runs_staged.load() > staged);
#endif
        CHECK(without_rows.load() == 0);
        MESSAGE("polls that saw the file: " << visible.load());
    }

    TEST_CASE("a full rebuild drops the plugin copies it does not write") {
        for (const std::uint64_t budget :
             {memory::NO_SPILL_BUDGET, std::uint64_t{1 * MIB}}) {
            CAPTURE(budget);
            dftu_utils_test::TestEnvironment env(10);
            const auto file = write_trace(env.dir(), "plugin", 12000);
            const auto dir = env.dir() + "/idx";
            const auto logical = index::store::internal::get_logical_path(file);
            REQUIRE(run_build(file, dir, budget).indexed == 1);

            auto index = index::Index::open(index_root(file, dir));
            REQUIRE(index);
            {
                auto stale = index::rebuild(*index, logical);
                REQUIRE(stale);
                index::put_plugin(*stale, "stale", 1, 5,
                                  {{0, dstd::String("x")}}, dstd::nullopt);
                REQUIRE(stale->commit().get());
            }
            auto plugin_count = [&] {
                auto snapshot = index->snapshot();
                REQUIRE(snapshot);
                const auto found = snapshot->file(logical);
                REQUIRE(found);
                const auto copies = index::plugins(*snapshot, *found);
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
