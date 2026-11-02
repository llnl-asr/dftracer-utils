// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/open_extent.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
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
    build::IndexBuildBatchResult result;
    {
        Runtime rt(1);
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            auto config = std::make_shared<build::IndexBuildBatchConfig>();
            config->file_paths = files;
            config->index_dir = index_dir;
            config->parallelism = 1;
            result = co_await build::BatchBuilder::process(std::move(config));
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
        // The store trains each namespace's dictionary on samples that fill
        // within the first files, so 64 files hold as many sample bytes as 256.
        const auto samples =
            dftracer::utils::store::engine::g_dict_sample_bytes.exchange(
                std::size_t{256} << 10);

        const auto small = peak_live(few, env.get_dir() + "/idx64");
        const auto large = peak_live(files, env.get_dir() + "/idx256");
        dftracer::utils::store::engine::g_dict_sample_bytes.store(samples);
        MESSAGE("peak live bytes: 64 files " << small << ", 256 files "
                                             << large);
        constexpr std::size_t BUDGET = std::size_t{48} << 20;
        CHECK(small < BUDGET);
        CHECK(large < BUDGET);
    }
}
