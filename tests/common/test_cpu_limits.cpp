// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/cpu_limits.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/common/worker_share.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/memory/budget.h>
#include <doctest/doctest.h>
#include <unistd.h>

#include <cstdlib>

using namespace dftracer::utils;

namespace {

// A fake /proc/self/cgroup and /sys/fs/cgroup under the working directory.
class FakeCgroupFs {
   public:
    FakeCgroupFs()
        : base_(dstd::fs::current_path().value() /
                dstd::fmt<"cgroup_fs_{}_{}">(::getpid(), next_id())) {
        REQUIRE(dstd::fs::remove_all(base_).has_value());
        REQUIRE(dstd::fs::create_directories(base_ / "sys").has_value());
    }
    FakeCgroupFs(const FakeCgroupFs&) = delete;
    FakeCgroupFs& operator=(const FakeCgroupFs&) = delete;
    ~FakeCgroupFs() { dstd::fs::remove_all(base_).discard(); }

    void listing(dstd::StringView text) {
        write(base_ / "proc_cgroup", text);
        proc_ = (base_ / "proc_cgroup").str();
        root_ = (base_ / "sys").str();
    }
    // `rel` is relative to the cgroup root, such as "a/b/cpu.max".
    void file(dstd::StringView rel, dstd::StringView text) {
        const dstd::Path path = base_ / "sys" / rel;
        REQUIRE(dstd::fs::create_directories(dstd::Path(path.view().parent()))
                    .has_value());
        write(path, text);
    }
    common::CgroupFs fs() const { return {proc_, root_}; }

   private:
    static void write(const dstd::Path& path, dstd::StringView text) {
        REQUIRE(dstd::fs::write_file(path, text).has_value());
    }
    static unsigned next_id() {
        static unsigned id = 0;
        return id++;
    }
    dstd::Path base_;
    dstd::String proc_;
    dstd::String root_;
};

}  // namespace

TEST_CASE("a v2 cpu quota rounds up and the smallest level wins") {
    FakeCgroupFs fake;
    fake.listing("0::/job/task\n");
    fake.file("job/task/cpu.max", "250000 100000\n");
    CHECK(common::cgroup_cpu_limit(fake.fs()) == 3);
    fake.file("job/cpu.max", "100000 100000\n");
    CHECK(common::cgroup_cpu_limit(fake.fs()) == 1);
    fake.file("job/cpu.max", "max 100000\n");
    fake.file("job/task/cpu.max", "max 100000\n");
    CHECK(common::cgroup_cpu_limit(fake.fs()) == 0);
}

TEST_CASE("a v1 cpu quota reads the cpu hierarchy") {
    FakeCgroupFs fake;
    fake.listing("4:cpu,cpuacct:/docker/abc\n1:name=systemd:/x\n");
    fake.file("cpu/docker/abc/cpu.cfs_quota_us", "150000\n");
    fake.file("cpu/docker/abc/cpu.cfs_period_us", "100000\n");
    CHECK(common::cgroup_cpu_limit(fake.fs()) == 2);
    fake.file("cpu/docker/abc/cpu.cfs_quota_us", "-1\n");
    CHECK(common::cgroup_cpu_limit(fake.fs()) == 0);
}

TEST_CASE("no cgroup files mean no cpu limit and no thread limit") {
    FakeCgroupFs fake;
    fake.listing("0::/\n");
    CHECK(common::cgroup_cpu_limit(fake.fs()) == 0);
    CHECK(common::cgroup_thread_room(fake.fs()) == common::UNLIMITED_THREADS);
}

TEST_CASE("the thread room is the smallest pids.max minus pids.current") {
    FakeCgroupFs fake;
    fake.listing("0::/job/task\n");
    fake.file("job/task/pids.max", "max\n");
    fake.file("job/task/pids.current", "40\n");
    CHECK(common::cgroup_thread_room(fake.fs()) == common::UNLIMITED_THREADS);
    fake.file("job/pids.max", "100\n");
    fake.file("job/pids.current", "70\n");
    CHECK(common::cgroup_thread_room(fake.fs()) == 30);
    fake.file("job/task/pids.max", "50\n");
    CHECK(common::cgroup_thread_room(fake.fs()) == 10);
    fake.file("job/task/pids.current", "50\n");
    CHECK(common::cgroup_thread_room(fake.fs()) == 0);
}

TEST_CASE("the v1 pids controller gives the same room") {
    FakeCgroupFs fake;
    fake.listing("7:pids:/user/x\n");
    fake.file("pids/user/x/pids.max", "64\n");
    fake.file("pids/user/x/pids.current", "60\n");
    CHECK(common::cgroup_thread_room(fake.fs()) == 4);
}

TEST_CASE("a worker's share divides the CPUs and the memory budget") {
    ::unsetenv("DFTRACER_UTILS_MEM_BUDGET");
    ::setenv("DFTRACER_UTILS_HW_CONCURRENCY", "128", 1);
    ::setenv("DFTRACER_UTILS_MEM_BUDGET", "48GB", 1);
    const auto one = common::worker_share(1);
    REQUIRE(one);
    CHECK(one->threads == 128);
    CHECK(one->io_threads == 128);
    CHECK(one->memory_budget == std::size_t{48} << 30);
    const auto many = common::worker_share(48);
    REQUIRE(many);
    CHECK(many->threads == 2);
    CHECK(many->io_threads == 2);
    CHECK(many->memory_budget == (std::size_t{48} << 30) / 48);
    // More workers than CPUs still leave one thread each, and a share under
    // the floor is raised to it.
    const auto crowded = common::worker_share(1000);
    REQUIRE(crowded);
    CHECK(crowded->threads == 1);
    CHECK(crowded->memory_budget == memory::MIN_MEMORY_BUDGET_BYTES);
    ::setenv("DFTRACER_UTILS_MEM_BUDGET", "100MB", 1);
    const auto floored = common::worker_share(48);
    REQUIRE(floored);
    CHECK(floored->memory_budget == memory::MIN_MEMORY_BUDGET_BYTES);
    ::setenv("DFTRACER_UTILS_MEM_BUDGET", "lots", 1);
    CHECK_FALSE(common::worker_share(2));
    ::unsetenv("DFTRACER_UTILS_MEM_BUDGET");
    ::unsetenv("DFTRACER_UTILS_HW_CONCURRENCY");
}

TEST_CASE("a worker's share without a configured budget is a third of memory") {
    ::unsetenv("DFTRACER_UTILS_MEM_BUDGET");
    const auto share = common::worker_share(4);
    REQUIRE(share);
    // Free memory moves between this read and the one inside worker_share,
    // so the two agree to within a percent, not to the byte.
    const std::size_t expected = std::max<std::size_t>(
        memory::resolve_spill_budget(0) / 4, memory::MIN_MEMORY_BUDGET_BYTES);
    const std::size_t got = share->memory_budget;
    const std::size_t diff = got > expected ? got - expected : expected - got;
    CHECK(diff <= expected / 100);
}

TEST_CASE("the default worker count fits the thread room beside an I/O pool") {
    using common::default_worker_threads;
    constexpr std::size_t FIXED = common::FIXED_RUNTIME_THREADS;
    // No limit keeps every CPU.
    CHECK(default_worker_threads(128, common::UNLIMITED_THREADS) == 128);
    // A large room keeps the CPUs; a small one is shared by workers and I/O
    // threads after the fixed threads.
    CHECK(default_worker_threads(8, 1000) == 8);
    CHECK(default_worker_threads(128, FIXED + 20) == 10);
    CHECK(default_worker_threads(4, FIXED + 20) == 4);
    // Never 0, also with no room at all.
    CHECK(default_worker_threads(128, FIXED + 1) == 1);
    CHECK(default_worker_threads(128, 0) == 1);
}

TEST_CASE("the default I/O thread count fits the thread room beside workers") {
    using common::default_io_threads;
    constexpr std::size_t FIXED = common::FIXED_RUNTIME_THREADS;
    CHECK(default_io_threads(64, common::UNLIMITED_THREADS) == 64);
    CHECK(default_io_threads(16, 1000) == 16);
    // Workers and the fixed threads come first.
    CHECK(default_io_threads(16, FIXED + 16 + 5) == 5);
    CHECK(default_io_threads(16, FIXED + 16 + 100) == 16);
    // An explicit worker count above the room still leaves one I/O thread.
    CHECK(default_io_threads(100, FIXED + 20) == 1);
    CHECK(default_io_threads(16, 0) == 1);
}

// Last, because a pinned call remembers its budget for the rest of the process.
TEST_CASE("a pinned share divides the budget from before the first pin") {
    ::setenv("DFTRACER_UTILS_MEM_BUDGET", "48GB", 1);
    const auto first = common::worker_share(4, true);
    REQUIRE(first);
    CHECK(first->memory_budget == (std::size_t{48} << 30) / 4);
    // What an apply does: write the share back to the variable.
    ::setenv("DFTRACER_UTILS_MEM_BUDGET",
             dstd::fmt<"{}">(first->memory_budget).c_str(), 1);
    const auto again = common::worker_share(4, true);
    REQUIRE(again);
    CHECK(again->memory_budget == first->memory_budget);
    const auto fewer = common::worker_share(2, true);
    REQUIRE(fewer);
    CHECK(fewer->memory_budget == (std::size_t{48} << 30) / 2);
    // An unpinned call reads the variable as it is now.
    const auto unpinned = common::worker_share(4);
    REQUIRE(unpinned);
    CHECK(unpinned->memory_budget == first->memory_budget / 4);
    ::unsetenv("DFTRACER_UTILS_MEM_BUDGET");
}
