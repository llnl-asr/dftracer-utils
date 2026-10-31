#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/file_admission.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <rocksdb/iterator.h>
#include <testing_utilities.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;
namespace store = dftracer::utils::index::store;

namespace {

// `events` events over small gzip members. Each event carries a few of
// `paths` distinct args keys (numbers and strings), names from a small set,
// and a time jump halfway, so chunks differ in every kind.
std::string write_trace(const std::string& dir, const std::string& name,
                        int events, int paths,
                        std::size_t member_bytes = 16 * 1024) {
    std::ostringstream out;
    out << "[\n";
    std::uint64_t ts = 1000000;
    for (int i = 0; i < events; ++i) {
        ts += 7 + static_cast<std::uint64_t>(i % 13);
        if (i == events / 2) ts += 500000;
        out << R"({"id":)" << i << R"(,"name":"op)" << (i / 50) % 7
            << R"(","cat":"C)" << i % 3 << R"(","pid":)" << 1 + i % 4
            << R"(,"tid":)" << 10 + i % 4 << R"(,"ph":"X","ts":)" << ts
            << R"(,"dur":)" << 5 + i % 97 << R"(,"args":{)";
        for (int k = 0; k < 3; ++k) {
            const int p = (i * 7 + k * 131) % paths;
            if (k) out << ",";
            out << "\"k" << p << "\":";
            if (p % 2)
                out << i % 50;
            else
                out << "\"v" << i % 11 << "\"";
        }
        out << "}}\n";
    }
    out << "]\n";
    const auto plain = dir + "/" + name + ".pfw";
    {
        std::ofstream(plain) << out.str();
    }
    const auto gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               member_bytes));
    fs::remove(plain);
    return gz;
}

build::IndexBuildBatchResult run_build(const std::vector<std::string>& files,
                                       const std::string& index_dir,
                                       std::uint64_t memory_budget,
                                       std::size_t path_budget = 0,
                                       std::size_t parallelism = 4) {
    build::IndexBuildBatchResult out;
    Runtime rt(4);
    auto task = run_coro_scope(
        rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
            auto config = std::make_shared<build::IndexBuildBatchConfig>();
            config->file_paths = files;
            config->index_dir = index_dir;
            config->parallelism = parallelism;
            config->memory_budget = memory_budget;
            config->bloom_config.path_budget = path_budget;
            out = co_await build::BatchBuilder::process(&scope,
                                                        std::move(config));
        });
    rt.submit(std::move(task), "test-build-memory").wait();
    rt.shutdown();
    return out;
}

// Every live key and value of every family, in order.
std::vector<std::pair<std::string, std::string>> contents(
    const std::string& index_path) {
    store::RocksDBManager::instance().reset(index_path);
    store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t f = 0; f < store::layout::FAMILY_COUNT; ++f) {
        const auto family = static_cast<store::layout::Family>(f);
        auto it = db.db()->new_iterator(store::layout::family_name(family));
        if (!it) continue;
        for (it->SeekToFirst(); it->Valid(); it->Next())
            out.emplace_back(std::string(store::layout::family_name(family)) +
                                 "|" + it->key().ToString(),
                             it->value().ToString());
        // A scan that stopped on an error, such as too many open SSTs, would
        // read as a smaller index.
        REQUIRE_MESSAGE(it->status().ok(), it->status().ToString());
    }
    return out;
}

std::string index_of(const std::string& trace, const std::string& dir) {
    return trace::internal::determine_index_path(trace, dir);
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

// A reservation of all the budget from a pool of its own, which fails when
// anything else is reserved and posts the shortfall to the holders.
memory::Status ask_for_budget() {
    memory::PoolHandle pool = memory::Manager::get().root().child("ask");
    REQUIRE(pool);
    return pool->reserve(memory::Manager::get().budget()).status();
}

}  // namespace

TEST_SUITE("IndexBuildMemory") {
    TEST_CASE("files are admitted by the bytes they hold, not one per worker") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < 4; ++i)
            files.push_back(
                write_trace(env.get_dir(), "t" + std::to_string(i), 2000, 20));
        constexpr std::size_t WORKERS = 4;
        constexpr std::uint64_t HELD_QUANTUM = MIB;
        const std::uint64_t held =
            (build::FileAdmission::estimate(
                 build::FileAdmission::plaintext_estimate(
                     0, fs::file_size(files[0])))
                 .total +
             HELD_QUANTUM - 1) /
            HELD_QUANTUM * HELD_QUANTUM;
        const std::uint64_t tight =
            4 * constants::file_admission::MIN_SINK_BYTES *
            constants::file_admission::SINK_BUDGET_DIVISOR;
        const auto plan = build::FileAdmission::plan_sinks(
            tight, WORKERS, build::g_max_sink_bytes.load());
        const std::uint64_t room =
            tight - plan.workers * plan.sink_bytes -
            tight / constants::file_admission::UNTRACKED_DIVISOR;
        // The pool holds fewer files than the build has workers.
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
        const std::vector<std::string> files = {
            write_trace(env.get_dir(), "small", 6000, 600),
            write_trace(env.get_dir(), "large", 60000, 600, 6u << 20)};
        for (const auto& file : files) {
            for (std::uint64_t budget :
                 {memory::NO_SPILL_BUDGET, std::uint64_t{64 << 10}}) {
                CAPTURE(file);
                CAPTURE(budget);
                const auto tag =
                    fs::path(file).stem().string() +
                    std::to_string(budget == memory::NO_SPILL_BUDGET);
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
        const std::vector<std::string> files = {
            write_trace(env.get_dir(), "many", 12000, 600, 2048)};
        const auto big_dir = env.get_dir() + "/big";
        const auto big = run_build(files, big_dir, memory::NO_SPILL_BUDGET);
        REQUIRE(big.indexed == 1);
        const auto expect = contents(index_of(files[0], big_dir));
        REQUIRE_FALSE(expect.empty());
        // A budget far below one file's state is slow, not wrong: the spill
        // share has a floor, so the runs stay few and every family matches.
        for (std::uint64_t budget : {1 * MIB, 8 * MIB}) {
            CAPTURE(budget);
            const auto dir = env.get_dir() + "/small" + std::to_string(budget);
            const auto small = run_build(files, dir, budget);
            REQUIRE(small.indexed == 1);
            CHECK(small.failed == 0);
            CHECK(small.metrics.files_spilled == 1);
            CHECK(small.metrics.max_spill_runs >= 1);
            CHECK(small.metrics.max_spill_runs <=
                  constants::file_admission::MAX_SPILL_RUNS);
            const auto got = contents(index_of(files[0], dir));
            REQUIRE(got.size() == expect.size());
            CHECK(got == expect);
        }
    }

    TEST_CASE("the size of a spill run grows so the run count stays bounded") {
        namespace ext = dftracer::utils::index::extensions;
        constexpr std::uint64_t SHARE = 1 * MIB;
        constexpr std::uint64_t TIER =
            constants::file_admission::MAX_SPILL_TIER_BYTES;
        std::uint64_t written = 0;
        std::size_t runs = 0;
        for (; written < TIER; ++runs)
            written += ext::BloomFold::run_limit(SHARE, runs);
        CHECK(runs <= constants::file_admission::MAX_SPILL_RUNS);
        CHECK(ext::BloomFold::run_limit(SHARE, 0) ==
              constants::file_admission::MIN_SPILL_RUN_BYTES);
        CHECK(ext::BloomFold::run_limit(64 * MIB, 0) == 16 * MIB);
    }

    TEST_CASE("a spill run that cannot be written fails the build") {
        dftu_utils_test::TestEnvironment env(10);
        const std::vector<std::string> files = {
            write_trace(env.get_dir(), "blocked", 12000, 600, 2048)};
        const auto dir = env.get_dir() + "/blocked_idx";
        const auto index = index_of(files[0], dir);
        // A regular file where the spill directory goes: no run can open.
        fs::create_directories(fs::path(index).parent_path());
        {
            std::ofstream(index + ".spill") << "not a directory";
        }
        const auto r = run_build(files, dir, 1 * MIB);
        CHECK(r.indexed == 0);
        CHECK(r.failed == 1);
        REQUIRE(r.results.size() == 1);
        CHECK_FALSE(r.results[0].success);
        CHECK_FALSE(r.results[0].error_message.empty());
    }

    TEST_CASE("a spilled build writes the same index as an unbounded one") {
        dftu_utils_test::TestEnvironment env(10);
        const std::vector<std::string> files = {
            write_trace(env.get_dir(), "narrow", 6000, 12),
            write_trace(env.get_dir(), "wide", 6000, 600)};
        for (std::size_t path_budget : {std::size_t{0}, std::size_t{40}}) {
            CAPTURE(path_budget);
            const auto big_dir =
                env.get_dir() + "/big" + std::to_string(path_budget);
            const auto small_dir =
                env.get_dir() + "/small" + std::to_string(path_budget);
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
            CHECK_FALSE(fs::exists(index_of(files[0], small_dir) + ".spill"));
        }
    }

    TEST_CASE("idle fold buffers are freed when a reservation asks for bytes") {
        Budget budget(64 * MIB);
        build::FoldBufferPool buffers;
        std::vector<std::unique_ptr<build::FoldBufferPool::EventBuffer>> out;
        for (int b = 0; b < 4; ++b) {
            out.push_back(buffers.take());
            for (int e = 0; e < 1000; ++e) out.back()->append();
        }
        for (auto& b : out) buffers.give(std::move(b));
        REQUIRE(buffers.idle() == 4);

        // The shortfall is past every idle byte, so every idle buffer goes.
        memory::PoolHandle hold = memory::Manager::get().root().child("hold");
        memory::Reservation held = hold->reserve(32 * MIB);
        REQUIRE(held);
        const std::uint64_t freed = build::g_fold_buffers_freed.load();
        CHECK(ask_for_budget() == memory::Status::Pending);
        auto again = buffers.take();
        CHECK(buffers.idle() == 0);
        CHECK(build::g_fold_buffers_freed.load() == freed + 4);
        buffers.give(std::move(again));
        CHECK(buffers.idle() == 1);
    }

    TEST_CASE(
        "a build whose fold buffers are asked for writes the same index") {
        dftu_utils_test::TestEnvironment env(10);
        const std::vector<std::string> files = {
            write_trace(env.get_dir(), "asked", 60000, 600, 1u << 20)};
        const auto plain_dir = env.get_dir() + "/plain";
        const auto asked_dir = env.get_dir() + "/asked";
        const auto plain = run_build(files, plain_dir, memory::NO_SPILL_BUDGET);
        REQUIRE(plain.indexed == 1);

        Budget budget(64 * MIB);
        memory::PoolHandle hold = memory::Manager::get().root().child("hold");
        memory::Reservation held = hold->reserve(MIB);
        REQUIRE(held);
        const std::uint64_t freed = build::g_fold_buffers_freed.load();
        std::atomic<bool> done{false};
        std::thread asker([&] {
            while (!done.load(std::memory_order_acquire)) {
                (void)ask_for_budget();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        const auto asked = run_build(files, asked_dir, memory::NO_SPILL_BUDGET);
        done.store(true, std::memory_order_release);
        asker.join();
        REQUIRE(asked.indexed == 1);
        CHECK(build::g_fold_buffers_freed.load() > freed);
        CHECK(contents(index_of(files[0], plain_dir)) ==
              contents(index_of(files[0], asked_dir)));
    }
}
