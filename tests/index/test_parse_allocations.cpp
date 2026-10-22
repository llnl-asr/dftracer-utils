#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::atomic<std::uint64_t> g_news{0};
}  // namespace

// Counts every global allocation of the process, so a test can bound how
// many the index parse makes per event.
void* operator new(std::size_t n) {
    g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;

namespace {

constexpr int FILES = 8;
constexpr int EVENTS = 4000;
// 14.1 per event before; 6.6 once the parse kept no args map it never read
// and every file's fold driver shared one pool of event buffers.
constexpr double MAX_PER_EVENT = 8.0;
// 4,250 per file of 500 events before; 3,527 once a fresh file id skipped
// its tier clears.
constexpr double MAX_PER_FILE = 3900.0;

std::string write_trace(const std::string& dir, int n, int events = EVENTS) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < events; ++i)
        out << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX","pid":)"
            << 1 + i % 4 << R"(,"tid":1,"ph":"X","ts":)" << 1000 + i * 10
            << R"(,"dur":3,"args":{"fname":"/data/file_number_)" << n << "_"
            << i % 37 << R"(","size":)" << i % 4096
            << R"(,"hhash":"a1b2c3d4e5f60718","level":)" << i % 3 << "}}\n";
    out << "]\n";
    return dftu_utils_test::write_gz_trace(
        dir + "/t" + std::to_string(n) + ".pfw.gz", out.str());
}

}  // namespace

TEST_SUITE("IndexParseAllocations") {
    TEST_CASE("the index parse makes few global allocations per event") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i));

        Runtime rt(4);
        build::IndexBuildBatchResult result;
        const auto before = g_news.load();
        auto task = run_coro_scope(
            rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
                auto config = std::make_shared<build::IndexBuildBatchConfig>();
                config->file_paths = files;
                config->index_dir = env.get_dir() + "/idx";
                config->parallelism = 4;
                result = co_await build::BatchBuilder::process(
                    &scope, std::move(config));
            });
        rt.submit(std::move(task), "parse-allocations").wait();
        const auto made = g_news.load() - before;
        rt.shutdown();

        REQUIRE(result.indexed == FILES);
        const double per_event = static_cast<double>(made) / (FILES * EVENTS);
        MESSAGE("global allocations per event: " << per_event);
        CHECK(per_event < MAX_PER_EVENT);
    }

    TEST_CASE(
        "a fresh build of many small files makes few allocations "
        "per file") {
        // Small files make the per-file work count: each new file's tier
        // clears were range deletes, each a memtable allocation, though a
        // fresh file id has nothing to clear.
        constexpr int SMALL_FILES = 64;
        constexpr int SMALL_EVENTS = 500;
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < SMALL_FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i, SMALL_EVENTS));

        Runtime rt(4);
        build::IndexBuildBatchResult result;
        const auto before = g_news.load();
        auto task = run_coro_scope(
            rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
                auto config = std::make_shared<build::IndexBuildBatchConfig>();
                config->file_paths = files;
                config->index_dir = env.get_dir() + "/idx";
                config->parallelism = 4;
                result = co_await build::BatchBuilder::process(
                    &scope, std::move(config));
            });
        rt.submit(std::move(task), "parse-allocations-small").wait();
        const auto made = g_news.load() - before;
        rt.shutdown();

        REQUIRE(result.indexed == SMALL_FILES);
        const double per_file = static_cast<double>(made) / SMALL_FILES;
        MESSAGE("global allocations per file: " << per_file);
        CHECK(per_file < MAX_PER_FILE);
    }
}
