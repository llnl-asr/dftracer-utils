// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/open_count.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <trace/bootstrap_test_common.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;

namespace {

constexpr int FILES = 96;
constexpr std::size_t WORKERS = 24;
#ifdef DFTRACER_UTILS_VALGRIND_MODE
// A Valgrind build caps every executor at 2 threads (thread_pool_executor.cpp).
constexpr std::size_t RUNNING_WORKERS = 2;
#else
constexpr std::size_t RUNNING_WORKERS = WORKERS;
#endif
constexpr int EVENTS = 1000;

dstd::String write_trace(const dstd::String& dir, int n) {
    dftu_utils_test::TextOut out;
    out << "[\n";
    for (int i = 0; i < EVENTS; ++i) {
        out << R"({"id":)" << i << R"(,"name":"op)" << (i + n) % 7
            << R"(","cat":"POSIX","pid":1,"tid":1,"ph":"X","ts":)"
            << 1000 + i * 10 << R"(,"dur":3,"args":{}})" << "\n";
    }
    out << "]\n";
    char name[32];
    std::snprintf(name, sizeof(name), "/t%04d.pfw.gz", n);
    return dftu_utils_test::write_gz_trace(dir + name, out.str());
}

class LineSink : public ExportSink {
   public:
    void write(dstd::StringView data) override { text.append(data); }
    dstd::String text;

    // A first touch dumps the raw file, array brackets included.
    dstd::Vector<dstd::String> lines() const {
        dstd::Vector<dstd::String> out;
        dftu_utils_test::LineStream in(text);
        for (dstd::String line; in.next(line);)
            if (!line.empty() && line.front() == '{') out.push_back(line);
        std::sort(out.begin(), out.end());
        return out;
    }
};

dstd::Vector<dstd::String> export_lines(const dstd::Vector<dstd::String>& files,
                                        const dstd::String& index_dir) {
    const auto view =
        View::from_files(bootstrap_test::view_files_of(files, index_dir));
    LineSink sink;
    default_runtime().block_on(
        coro::named("bootstrap-export",
                    coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                        co_await view.sink_json(sink);
                    })));
    return sink.lines();
}

}  // namespace

TEST_SUITE("BootstrapParallelism") {
    TEST_CASE("a cold bootstrap keeps a file in flight per worker") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Vector<dstd::String> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i));
        const auto index_dir = env.get_dir() + "/idx";

        configure_default_runtime({.threads = WORKERS, .eager = true});
        REQUIRE(default_runtime().threads() == WORKERS);

        trace::views::detail::bootstrap_counters().reset();
        const auto store_opens_before =
            dftracer::utils::store::internal::opens();
        const auto cold = bootstrap_test::count_by_name(files, index_dir);
        const auto store_opens =
            dftracer::utils::store::internal::opens() - store_opens_before;

        CHECK(trace::views::detail::bootstrap_counters().max_in_flight.load() ==
              RUNNING_WORKERS);
        CHECK(store_opens == 1);

        const auto warm = bootstrap_test::count_by_name(files, index_dir);
        CHECK(bootstrap_test::counts(cold) == bootstrap_test::counts(warm));
        std::int64_t total = 0;
        for (const auto& [name, n] : bootstrap_test::counts(cold)) total += n;
        CHECK(total == FILES * EVENTS);
    }

    TEST_CASE(
        "an export bootstrap opens the store once and equals a warm one") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Vector<dstd::String> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i));
        const auto index_dir = env.get_dir() + "/idx";

        configure_default_runtime({.threads = WORKERS, .eager = true});
        const auto store_opens_before =
            dftracer::utils::store::internal::opens();
        const auto cold = export_lines(files, index_dir);
        const auto store_opens =
            dftracer::utils::store::internal::opens() - store_opens_before;
        CHECK(store_opens == 1);
        CHECK(cold.size() == static_cast<std::size_t>(FILES) * EVENTS);

        const auto warm = export_lines(files, index_dir);
        CHECK(cold == warm);
        CHECK(bootstrap_test::counts(
                  bootstrap_test::count_by_name(files, index_dir))
                  .size() == 7);
        CHECK_FALSE(memory::detail::Manager::get().leaked());
    }
}
