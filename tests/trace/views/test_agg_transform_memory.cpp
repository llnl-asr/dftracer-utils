// SPDX-License-Identifier: MIT
// A group-key transform must stream: the engine adds each transformed key
// column to a morsel as it passes and never holds the scanned rows. Each run is
// a child process (this binary started again with --child) so its peak RSS is
// measured alone; the transform run may exceed the same aggregation without a
// transform by a small margin only.
#define DOCTEST_CONFIG_IMPLEMENT
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <doctest/doctest.h>
#include <instrumentation.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <trace/views/agg_parity_common.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

using namespace dftracer::utils;

using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;

using namespace dftracer::utils::trace;

namespace {

constexpr std::uint64_t BUDGET = 4ULL << 20;
constexpr std::uint64_t MAX_EXTRA = 24ULL << 20;
constexpr int RUNS = 3;

// Keys padded to KEY_PAD bytes, so a transform that held the scanned keys would
// hold twice MAX_EXTRA.
constexpr std::size_t KEY_PAD = 320;
constexpr int EVENTS = static_cast<int>((2 * MAX_EXTRA) / KEY_PAD);

dstd::String g_self;

dstd::String write_trace(const dstd::String& dir) {
    const dstd::String pfw = dir + "/transform_memory.pfw";
    const dstd::String pad = KEY_PAD ? "_" + dstd::String(KEY_PAD, 'x') : "";
    {
        dftu_utils_test::FileOut ofs(pfw);
        for (int i = 0; i < EVENTS; ++i)
            ofs << R"({"ph":"X","name":"read","cat":"category_)" << (i % 500)
                << pad << R"(","pid":1,"tid":1,"ts":)" << (1000 + i * 10)
                << R"(,"dur":)" << (5 + i % 17) << R"(,"args":{}})" << "\n";
    }
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    (void)dstd::fs::remove(pfw);
    return gz;
}

int child_main(const dstd::String& mode, const dstd::String& gz) {
    const dstd::String idx = dstd::String(determine_index_path(gz, ""));
    GroupKey key = GroupKey::cat();
    if (mode == "transform") {
        key.transform = GroupKey::Transform::Bucket;
        key.transform_args = {"category_1"};
    }
    views::detail::scan::ScanPlan plan = views::detail::scan::agg(
        views::detail::scan::group_by(
            views::detail::scan::memory_budget(
                views::detail::scan::from_file(dstd::String(gz),
                                               dstd::String(idx)),
                BUDGET),
            {key}),
        {{AggOp::Sum, "dur", "sum_dur"}});
    static dftracer::utils::Runtime rt;
    std::int64_t rows = 0;
    rt.block_on(dftracer::utils::coro::named(
        "transform-memory",
        dftracer::utils::coro::with_scope(
            [&](dftracer::utils::coro::Scope&)
                -> dftracer::utils::coro::CoroTask<void> {
                auto df = co_await views::detail::run_collect_via_engine(*plan);
                rows = df.num_rows();
            })));
    std::printf("rows=%lld\n", static_cast<long long>(rows));
    return 0;
}

pid_t start_child(const dstd::String& mode, const dstd::String& gz) {
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::execl(g_self.c_str(), g_self.c_str(), "--child", mode.c_str(),
                gz.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return pid;
}

std::uint64_t peak_rss(pid_t pid) {
    int status = 0;
    struct rusage ru{};
    ::wait4(pid, &status, 0, &ru);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);
#else
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;
#endif
}

}  // namespace

TEST_CASE("a group-key transform holds no more than one morsel" *
          doctest::skip(instrumentation::active())) {
    TestEnvironment env(200);
    REQUIRE(env.is_valid());
    const dstd::String gz = write_trace(env.dir());
    // Under load a run's peak can drop by tens of MiB, so each side is the
    // median of a few runs; the lowest let one side dip alone. The first run
    // builds the index. The runs go one at a time, as children that compete
    // for cores hold more memory while they wait.
    peak_rss(start_child("plain", gz));
    std::uint64_t plain_runs[RUNS];
    std::uint64_t transform_runs[RUNS];
    for (int i = 0; i < RUNS; ++i) {
        plain_runs[i] = peak_rss(start_child("plain", gz));
        transform_runs[i] = peak_rss(start_child("transform", gz));
    }
    std::sort(plain_runs, plain_runs + RUNS);
    std::sort(transform_runs, transform_runs + RUNS);
    const std::uint64_t plain = plain_runs[RUNS / 2];
    const std::uint64_t transformed = transform_runs[RUNS / 2];
    const std::uint64_t extra = transformed > plain ? transformed - plain : 0;
    MESSAGE("peak RSS without a transform ", plain >> 20, " MiB, with one ",
            transformed >> 20, " MiB");
    CHECK_MESSAGE(extra <= MAX_EXTRA, "the transform holds ", extra >> 20,
                  " MiB above the same aggregation without one (limit ",
                  MAX_EXTRA >> 20, " MiB)");
}

int main(int argc, char** argv) {
    if (argc >= 4 && dstd::String(argv[1]) == "--child")
        return child_main(argv[2], argv[3]);
    char* resolved = ::realpath(argv[0], nullptr);
    g_self = resolved ? resolved : argv[0];
    std::free(resolved);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
