// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/open_extent.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

using namespace dftracer::utils;

namespace {

constexpr int EVENTS = 3000;

dstd::String write_trace(const dstd::String& dir, int n) {
    dstd::String out;
    out += "[\n";
    for (int i = 0; i < EVENTS; ++i) {
        out += common::str_cat(
            R"({"id":)", i, R"(,"name":"op)", (i + n) % 40,
            R"(","cat":"POSIX","pid":)", 1 + n % 7, R"(,"tid":)", 10 + i % 5,
            R"(,"ph":"X","ts":)", 1000 + i * 10 + n * 7, R"(,"dur":)",
            3 + i % 50, R"(,"args":{"fname":"/data/d)", n % 9, "/f",
            (i * 31 + n) % 700, R"(","size":)", (i * 17) % 4096,
            R"(,"hhash":"h)", n, R"("}})", "\n");
    }
    out += "]\n";
    return dftu_utils_test::write_gz_trace(
        dir + "/t" + dstd::String(dstd::to_string(n)) + ".pfw.gz", out);
}

std::size_t peak_live(const dstd::Vector<dstd::String>& files,
                      const dstd::String& index_dir) {
    std::atomic<bool> done{false};
    std::atomic<std::size_t> peak{0};
    dstd::Thread sampler([&] {
        while (!done.load(std::memory_order_relaxed)) {
            const auto live = memory::detail::Manager::get().stats().live;
            auto seen = peak.load(std::memory_order_relaxed);
            while (live > seen && !peak.compare_exchange_weak(
                                      seen, live, std::memory_order_relaxed)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const auto base = memory::detail::Manager::get().stats().live;
    index::build::IndexBuildBatchResult result;
    {
        Runtime rt(1);
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            auto config =
                dstd::make_shared<index::build::IndexBuildBatchConfig>();
            config->file_paths.assign(files.begin(), files.end());
            config->index_dir = index_dir;
            config->parallelism = 1;
            result =
                co_await index::build::BatchBuilder::process(std::move(config));
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
        dstd::Vector<dstd::String> files;
        for (int i = 0; i < 64; ++i)
            files.emplace_back(write_trace(env.dir(), i));
        const dstd::Vector<dstd::String> few(files.begin(), files.begin() + 16);
        // The store trains each namespace's dictionary on samples that fill
        // within the first files, so 16 files hold as many sample bytes as 64.
        const auto samples =
            dftracer::utils::store::engine::g_dict_sample_bytes.exchange(
                std::size_t{256} << 10);

        const auto small = peak_live(few, env.dir() + "/idx16");
        const auto large = peak_live(files, env.dir() + "/idx64");
        dftracer::utils::store::engine::g_dict_sample_bytes.store(samples);
        MESSAGE("peak live bytes: 16 files " << small << ", 64 files "
                                             << large);
        constexpr std::size_t BUDGET = std::size_t{48} << 20;
        // Four times the files may not add more than a few hundred KB each.
        constexpr std::size_t GROWTH = std::size_t{8} << 20;
        CHECK(small < BUDGET);
        CHECK(large < BUDGET);
        CHECK(large < small + GROWTH);
    }
}
