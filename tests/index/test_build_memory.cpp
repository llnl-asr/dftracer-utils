// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/file_admission.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/memory/detail/heap.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/memory/detail/pool_access.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::index;

namespace {

// `events` events over small gzip members. Each event carries a few of
// `paths` distinct args keys (numbers and strings), names from a small set,
// and a time jump halfway, so chunks differ in every kind.
dstd::String write_trace(const dstd::String& dir, const dstd::String& name,
                         int events, int paths,
                         std::size_t member_bytes = 16 * 1024) {
    dstd::String out;
    out += "[\n";
    std::uint64_t ts = 1000000;
    for (int i = 0; i < events; ++i) {
        ts += 7 + static_cast<std::uint64_t>(i % 13);
        if (i == events / 2) ts += 500000;
        out += common::str_cat(R"({"id":)", i, R"(,"name":"op)", (i / 50) % 7,
                               R"(","cat":"C)", i % 3, R"(","pid":)", 1 + i % 4,
                               R"(,"tid":)", 10 + i % 4, R"(,"ph":"X","ts":)",
                               ts, R"(,"dur":)", 5 + i % 97, R"(,"args":{)");
        for (int k = 0; k < 3; ++k) {
            const int p = (i * 7 + k * 131) % paths;
            if (k) out += ",";
            out += common::str_cat("\"k", p, "\":");
            if (p % 2)
                out += common::str_cat(i % 50);
            else
                out += common::str_cat("\"v", i % 11, "\"");
        }
        out += "}}\n";
    }
    out += "]\n";
    const auto plain = dir + "/" + name + ".pfw";
    {
        (void)dstd::fs::write_file(dstd::PathView(plain), out);
    }
    const auto gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               member_bytes));
    (void)dstd::fs::remove(plain);
    return gz;
}

index::build::IndexBuildBatchResult run_build(
    const dstd::Vector<dstd::String>& files, const dstd::String& index_dir,
    std::uint64_t memory_budget, std::size_t path_budget = 0,
    std::size_t parallelism = 4) {
    index::build::IndexBuildBatchResult out;
    Runtime rt(4);
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        auto config = dstd::make_shared<index::build::IndexBuildBatchConfig>();
        for (const auto& file : files) config->file_paths.emplace_back(file);
        config->index_dir = dstd::String(index_dir);
        config->parallelism = parallelism;
        config->memory_budget = memory_budget;
        config->bloom_config.path_budget = path_budget;
        out = co_await index::build::BatchBuilder::process(std::move(config));
    });
    rt.submit(std::move(task), "test-build-memory").wait();
    rt.shutdown();
    for (const auto& file : out.results)
        if (!file.success) MESSAGE("build failed: " << file.error_message);
    return out;
}

// Every live record of every family, in order.
dstd::Vector<dftu_utils_test::IndexRecord> contents(
    const dstd::String& index_path) {
    return dftu_utils_test::dump_index(index_path);
}

dstd::String index_of(const dstd::String& trace, const dstd::String& dir) {
    return dstd::String(trace::internal::determine_index_path(trace, dir));
}

constexpr std::uint64_t MIB = std::uint64_t{1} << 20;

struct Budget {
    explicit Budget(std::size_t bytes) {
        memory::Config c;
        c.budget = bytes;
        memory::configure(c);
    }
    ~Budget() { memory::configure(memory::Config{}); }
};

// Fails when anything else is reserved and posts the shortfall to the holders.
memory::Status ask_for_budget() {
    memory::PoolHandle pool =
        memory::detail::Manager::get().root().child("ask");
    REQUIRE(pool);
    return memory::detail::PoolAccess::reserve(
               *pool, memory::detail::Manager::get().budget())
        .status();
}

}  // namespace

TEST_SUITE("IndexBuildMemory") {
    TEST_CASE("files are admitted by the bytes they hold, not one per worker") {
        dftu_utils_test::TestEnvironment env(10);
        // Sinks of 32 MiB leave the tight budget room for fewer files than
        // workers.
        struct SinkLimit {
            std::size_t previous =
                index::build::g_max_sink_bytes.exchange(std::size_t{32} << 20);
            ~SinkLimit() { index::build::g_max_sink_bytes.store(previous); }
        } sink_limit;
        dstd::Vector<dstd::String> files;
        for (int i = 0; i < 4; ++i)
            files.push_back(
                write_trace(env.get_dir(), "t" + dstd::to_string(i), 2000, 20));
        constexpr std::size_t WORKERS = 4;
        constexpr std::uint64_t HELD_QUANTUM = MIB;
        const std::uint64_t held =
            (index::build::FileAdmission::estimate(
                 index::build::FileAdmission::plaintext_estimate(
                     0, dstd::fs::file_size(files[0]).value()))
                 .total +
             HELD_QUANTUM - 1) /
            HELD_QUANTUM * HELD_QUANTUM;
        const std::uint64_t tight =
            4 * common::constants::file_admission::MIN_SINK_BYTES *
            common::constants::file_admission::SINK_BUDGET_DIVISOR;
        const auto plan = index::build::FileAdmission::plan_sinks(
            tight, WORKERS, index::build::g_max_sink_bytes.load());
        const std::uint64_t room =
            tight - plan.workers * plan.sink_bytes -
            tight / common::constants::file_admission::UNTRACKED_DIVISOR;
        REQUIRE(room / held < WORKERS);
        const auto few = run_build(files, env.get_dir() + "/few", tight);
        const auto many = run_build(files, env.get_dir() + "/many", 32 * tight);
        REQUIRE(few.indexed == 4);
        REQUIRE(many.indexed == 4);
        CHECK(few.failed == 0);
        CHECK(few.metrics.max_files_in_flight >= 1);
        CHECK(few.metrics.max_files_in_flight <=
              std::max<std::uint64_t>(room / held, 1));
        CHECK(many.metrics.max_files_in_flight >= 2);
    }

    TEST_CASE("parsing ahead writes the same index as one parse at a time") {
        dftu_utils_test::TestEnvironment env(10);
        // Small members are one parse piece each; a 6 MB member is several
        // pieces of one checkpoint.
        const dstd::Vector<dstd::String> files = {
            write_trace(env.get_dir(), "small", 6000, 600),
            write_trace(env.get_dir(), "large", 60000, 600, 6u << 20)};
        for (const auto& file : files) {
            for (std::uint64_t budget :
                 {memory::NO_SPILL_BUDGET, std::uint64_t{64 << 10}}) {
                CAPTURE(file);
                CAPTURE(budget);
                const auto tag = dstd::String(dstd::PathView(file).stem()) +
                                 dstd::to_string(static_cast<int>(
                                     budget == memory::NO_SPILL_BUDGET));
                const auto serial_dir = env.get_dir() + "/serial" + tag;
                const auto ahead_dir = env.get_dir() + "/ahead" + tag;
                const auto serial = run_build({file}, serial_dir, budget, 0, 1);
                const auto ahead = run_build({file}, ahead_dir, budget, 0, 4);
                REQUIRE(serial.indexed == 1);
                REQUIRE(ahead.indexed == 1);
                CHECK(contents(index_of(file, serial_dir)) ==
                      contents(index_of(file, ahead_dir)));
            }
        }
    }

    TEST_CASE("a spill of thousands of chunks writes the same index") {
        // 2 KB members give about 1200 chunks, so the last spill writes them
        // into several runs at once.
        dftu_utils_test::TestEnvironment env(10);
        const dstd::Vector<dstd::String> files = {
            write_trace(env.get_dir(), "many", 12000, 600, 2048)};
        const auto big_dir = env.get_dir() + "/big";
        const auto big = run_build(files, big_dir, memory::NO_SPILL_BUDGET);
        REQUIRE(big.indexed == 1);
        const auto expect = contents(index_of(files[0], big_dir));
        REQUIRE_FALSE(expect.empty());
        // A budget far below one file's state is slow, not wrong. The spill
        // share has a floor, so the runs stay few.
        for (std::uint64_t budget : {1 * MIB, 8 * MIB}) {
            CAPTURE(budget);
            const auto dir = env.get_dir() + "/small" + dstd::to_string(budget);
            const auto small = run_build(files, dir, budget);
            REQUIRE(small.indexed == 1);
            CHECK(small.failed == 0);
            CHECK(small.metrics.files_spilled == 1);
            CHECK(small.metrics.max_spill_runs >= 1);
            CHECK(small.metrics.max_spill_runs <=
                  common::constants::file_admission::MAX_SPILL_RUNS);
            const auto got = contents(index_of(files[0], dir));
            REQUIRE(got.size() == expect.size());
            CHECK(got == expect);
        }
    }

    TEST_CASE("the size of a spill run grows so the run count stays bounded") {
        constexpr std::uint64_t SHARE = 1 * MIB;
        constexpr std::uint64_t TIER =
            common::constants::file_admission::MAX_SPILL_TIER_BYTES;
        std::uint64_t written = 0;
        std::size_t runs = 0;
        for (; written < TIER; ++runs)
            written += extensions::BloomFold::run_limit(SHARE, runs);
        CHECK(runs <= common::constants::file_admission::MAX_SPILL_RUNS);
        CHECK(extensions::BloomFold::run_limit(SHARE, 0) ==
              common::constants::file_admission::MIN_SPILL_RUN_BYTES);
        CHECK(extensions::BloomFold::run_limit(64 * MIB, 0) == 16 * MIB);
    }

    TEST_CASE("a spilled build writes the same index as an unbounded one") {
        dftu_utils_test::TestEnvironment env(10);
        const dstd::Vector<dstd::String> files = {
            write_trace(env.get_dir(), "narrow", 6000, 12),
            write_trace(env.get_dir(), "wide", 6000, 600)};
        for (std::size_t path_budget : {std::size_t{0}, std::size_t{40}}) {
            CAPTURE(path_budget);
            const auto big_dir =
                env.get_dir() + "/big" + dstd::to_string(path_budget);
            const auto small_dir =
                env.get_dir() + "/small" + dstd::to_string(path_budget);
            const auto big =
                run_build(files, big_dir, memory::NO_SPILL_BUDGET, path_budget);
            const auto small =
                run_build(files, small_dir, 64 * 1024, path_budget);
            REQUIRE(big.indexed == 2);
            REQUIRE(small.indexed == 2);
            CHECK(big.metrics.files_spilled == 0);
            CHECK(small.metrics.files_spilled == 2);
            const auto a = contents(index_of(files[0], big_dir));
            const auto b = contents(index_of(files[0], small_dir));
            CHECK(a.size() == b.size());
            CHECK(a == b);
            CHECK_FALSE(
                dstd::fs::exists(index_of(files[0], small_dir) + ".spill")
                    .value());
        }
    }

    TEST_CASE("idle fold buffers are freed when a reservation asks for bytes") {
        Budget budget(64 * MIB);
        index::build::FoldBufferPool buffers;
        dstd::Vector<dstd::UniquePtr<index::build::FoldBufferPool::EventBuffer>>
            out;
        for (int b = 0; b < 4; ++b) {
            out.push_back(buffers.take());
            for (int e = 0; e < 1000; ++e) out.back()->append();
        }
        for (auto& b : out) buffers.give(std::move(b));
        REQUIRE(buffers.idle() == 4);

        memory::PoolHandle hold =
            memory::detail::Manager::get().root().child("hold");
        memory::Reservation held =
            memory::detail::PoolAccess::reserve(*hold, 32 * MIB);
        REQUIRE(held);
        const std::uint64_t freed = index::build::g_fold_buffers_freed.load();
        CHECK(ask_for_budget() == memory::Status::Pending);
        auto again = buffers.take();
        CHECK(buffers.idle() == 0);
        CHECK(index::build::g_fold_buffers_freed.load() == freed + 4);
        buffers.give(std::move(again));
        CHECK(buffers.idle() == 1);
    }

    TEST_CASE(
        "a build whose fold buffers are asked for writes the same index") {
        dftu_utils_test::TestEnvironment env(10);
        const dstd::Vector<dstd::String> files = {
            write_trace(env.get_dir(), "asked", 60000, 600, 1u << 20)};
        const auto plain_dir = env.get_dir() + "/plain";
        const auto asked_dir = env.get_dir() + "/asked";
        const auto plain = run_build(files, plain_dir, memory::NO_SPILL_BUDGET);
        REQUIRE(plain.indexed == 1);

        Budget budget(64 * MIB);
        memory::PoolHandle hold =
            memory::detail::Manager::get().root().child("hold");
        memory::Reservation held =
            memory::detail::PoolAccess::reserve(*hold, MIB);
        REQUIRE(held);
        const std::uint64_t freed = index::build::g_fold_buffers_freed.load();
        std::atomic<bool> done{false};
        dstd::Thread asker([&] {
            while (!done.load(std::memory_order_acquire)) {
                (void)ask_for_budget();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        const auto asked = run_build(files, asked_dir, memory::NO_SPILL_BUDGET);
        done.store(true, std::memory_order_release);
        asker.join();
        REQUIRE(asked.indexed == 1);
        CHECK(index::build::g_fold_buffers_freed.load() > freed);
        CHECK(contents(index_of(files[0], plain_dir)) ==
              contents(index_of(files[0], asked_dir)));
    }
}
