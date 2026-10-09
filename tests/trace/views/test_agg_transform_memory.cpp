// A group-key transform must stream: the engine adds each transformed key
// column to a morsel as it passes and never holds the scanned rows. Each run is
// a child process (this binary started again with --child) so its peak RSS is
// measured alone; the transform run may exceed the same aggregation without a
// transform by a small margin only.
#define DOCTEST_CONFIG_IMPLEMENT
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <doctest/doctest.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "agg_parity_common.h"

namespace scan = dftracer::utils::trace::views::detail::scan;
namespace scan_detail = dftracer::utils::trace::views::detail;

namespace {

constexpr int EVENTS = 1'500'000;
constexpr std::uint64_t BUDGET = 4ULL << 20;
constexpr std::uint64_t MAX_EXTRA = 24ULL << 20;
constexpr int RUNS = 3;

std::string g_self;

std::string write_trace(const std::string& dir) {
    const std::string pfw = dir + "/transform_memory.pfw";
    {
        std::ofstream ofs(pfw);
        for (int i = 0; i < EVENTS; ++i)
            ofs << R"({"ph":"X","name":"read","cat":"category_)" << (i % 500)
                << R"(","pid":1,"tid":1,"ts":)" << (1000 + i * 10)
                << R"(,"dur":)" << (5 + i % 17) << R"(,"args":{}})" << "\n";
    }
    const std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

int child_main(const std::string& mode, const std::string& gz) {
    const std::string idx = determine_index_path(gz, "");
    GroupKey key = GroupKey::cat();
    if (mode == "transform") {
        key.transform = GroupKey::Transform::Bucket;
        key.transform_args = {"category_1"};
    }
    scan::ScanPlan plan = scan::agg(
        scan::group_by(scan::memory_budget(scan::from_file(gz, idx), BUDGET),
                       {key}),
        {{AggOp::Sum, "dur", "sum_dur"}});
    static dftracer::utils::Runtime rt;
    std::int64_t rows = 0;
    rt.run_blocking("transform-memory",
                    [&](dftracer::utils::CoroScope&)
                        -> dftracer::utils::coro::CoroTask<void> {
                        auto df =
                            co_await scan_detail::run_collect_via_engine(*plan);
                        rows = df.num_rows();
                    });
    std::printf("rows=%lld\n", static_cast<long long>(rows));
    return 0;
}

std::uint64_t run_child(const std::string& mode, const std::string& gz) {
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::execl(g_self.c_str(), g_self.c_str(), "--child", mode.c_str(),
                gz.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
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

bool under_instrumentation() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
    return true;
#endif
#endif
    const char* preload = std::getenv("LD_PRELOAD");
    return preload && std::strstr(preload, "vgpreload") != nullptr;
}

}  // namespace

TEST_CASE("a group-key transform holds no more than one morsel" *
          doctest::skip(under_instrumentation())) {
    TestEnvironment env(200);
    REQUIRE(env.is_valid());
    const std::string gz = write_trace(env.get_dir());
    // One run's peak swings by about 20 MiB, so each side is the lowest of a
    // few runs.
    run_child("plain", gz);
    std::uint64_t plain = ~std::uint64_t{0};
    std::uint64_t transformed = ~std::uint64_t{0};
    for (int i = 0; i < RUNS; ++i) {
        plain = std::min(plain, run_child("plain", gz));
        transformed = std::min(transformed, run_child("transform", gz));
    }
    const std::uint64_t extra = transformed > plain ? transformed - plain : 0;
    CHECK_MESSAGE(extra <= MAX_EXTRA, "the transform holds ", extra >> 20,
                  " MiB above the same aggregation without one (limit ",
                  MAX_EXTRA >> 20, " MiB)");
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--child")
        return child_main(argv[2], argv[3]);
    char* resolved = ::realpath(argv[0], nullptr);
    g_self = resolved ? resolved : argv[0];
    std::free(resolved);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
