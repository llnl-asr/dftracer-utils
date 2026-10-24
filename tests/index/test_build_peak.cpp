#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;
namespace store = dftracer::utils::index::store;

namespace {

constexpr int EVENTS = 3000;

std::string write_trace(const std::string& dir, int n) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < EVENTS; ++i) {
        out << R"({"id":)" << i << R"(,"name":"op)" << (i + n) % 40
            << R"(","cat":"POSIX","pid":)" << 1 + n % 7 << R"(,"tid":)"
            << 10 + i % 5 << R"(,"ph":"X","ts":)" << 1000 + i * 10 + n * 7
            << R"(,"dur":)" << 3 + i % 50 << R"(,"args":{"fname":"/data/d)"
            << n % 9 << "/f" << (i * 31 + n) % 700 << R"(","size":)"
            << (i * 17) % 4096 << R"(,"hhash":"h)" << n << R"("}})"
            << "\n";
    }
    out << "]\n";
    return dftu_utils_test::write_gz_trace(
        dir + "/t" + std::to_string(n) + ".pfw.gz", out.str());
}

// Largest live byte count seen while a one-worker build of `files` runs.
std::size_t peak_live(const std::vector<std::string>& files,
                      const std::string& index_dir) {
    std::atomic<bool> done{false};
    std::atomic<std::size_t> peak{0};
    std::thread sampler([&] {
        while (!done.load(std::memory_order_relaxed)) {
            const auto live = memory::Manager::get().stats().live;
            auto seen = peak.load(std::memory_order_relaxed);
            while (live > seen && !peak.compare_exchange_weak(
                                      seen, live, std::memory_order_relaxed)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const auto base = memory::Manager::get().stats().live;
    const std::string staging = index_dir + ".staging";
    auto counter = std::make_shared<std::atomic<std::size_t>>(0);
    build::IndexBuildBatchResult result;
    {
        Runtime rt(1);
        auto task = run_coro_scope(
            rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
                auto config = std::make_shared<build::IndexBuildBatchConfig>();
                config->file_paths = files;
                config->index_dir = index_dir;
                config->parallelism = 1;
                config->sink_factory = [staging, counter] {
                    return std::make_unique<
                        store::IndexDatabaseSstWriterContext>(
                        staging,
                        "batch_" + std::to_string(counter->fetch_add(1)));
                };
                config->sink_commit = [](store::IndexWrite& sink) {
                    static_cast<store::IndexDatabaseSstWriterContext&>(sink)
                        .commit();
                };
                result = co_await build::BatchBuilder::process(
                    &scope, std::move(config));
            });
        rt.submit(std::move(task), "build-peak").wait();
        rt.shutdown();
    }
    done.store(true);
    sampler.join();
    REQUIRE(result.indexed == files.size());
    const auto top = peak.load();
    return top > base ? top - base : 0;
}

}  // namespace

TEST_SUITE("IndexBuildPeak") {
    TEST_CASE("one worker holds the state of a file only until it is written") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < 256; ++i)
            files.push_back(write_trace(env.get_dir(), i));
        const std::vector<std::string> few(files.begin(), files.begin() + 64);

        const auto small = peak_live(few, env.get_dir() + "/idx64");
        const auto large = peak_live(files, env.get_dir() + "/idx256");
        MESSAGE("peak live bytes: 64 files " << small << ", 256 files "
                                             << large);
        // One worker holds one file's state and a sink of up to its limit,
        // whatever the number of files.
        constexpr std::size_t BUDGET = std::size_t{48} << 20;
        CHECK(small < BUDGET);
        CHECK(large < BUDGET);
    }

    TEST_CASE(
        "a sink counts the memory it holds, not only its keys and values") {
        dftu_utils_test::TestEnvironment env(10);
        store::IndexDatabaseSstWriterContext sink(env.get_dir() + "/staging",
                                                  "batch_0");
        constexpr std::size_t ENTRIES = 20000;
        std::size_t staged = 0;
        for (std::size_t i = 0; i < ENTRIES; ++i) {
            char key[24];
            std::snprintf(key, sizeof(key), "key%08zu", ENTRIES - i);
            const std::string value(48, 'v');
            sink.put(store::layout::Family::Registry, key, value);
            staged += std::strlen(key) + value.size();
        }
        CHECK(sink.buffered_bytes() >= staged);
        CHECK(sink.buffered_bytes() < staged * 4);
        const auto artifacts = sink.commit();
        CHECK_FALSE(artifacts.empty());
        CHECK(sink.buffered_bytes() == 0);
    }
}
