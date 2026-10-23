#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/file_admission.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/index/build/parse_gate.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;
namespace store = dftracer::utils::index::store;

namespace {

constexpr std::size_t SINK_LIMIT = std::size_t{2} << 20;
constexpr std::size_t BUDGET = std::size_t{160} << 20;
constexpr std::size_t SLACK = std::size_t{24} << 20;
constexpr int SMALL_FILES = 96;
constexpr int SMALL_EVENTS = 3000;

std::string small_trace(const std::string& dir, int n) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < SMALL_EVENTS; ++i) {
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
        dir + "/s" + std::to_string(n) + ".pfw.gz", out.str());
}

// Segments of SEGMENT_LINES lines each walk the same names in another order,
// so the first appearance of a string depends on which segment is read first.
constexpr int SEGMENT_LINES = 4096;
constexpr int NAMES = 61;

std::string big_trace(const std::string& dir, const std::string& stem,
                      int lines, int variant) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < lines; ++i) {
        const int seg = i / SEGMENT_LINES;
        const int step = 2 * ((seg * 7 + variant) % 29) + 1;
        const int pick = (i * step + seg * 13) % NAMES;
        out << R"({"id":)" << i << R"(,"name":"op)" << pick << R"(","cat":"c)"
            << (pick * 3 + seg) % 11 << R"(","pid":)" << 1 + (i + seg) % 7
            << R"(,"tid":)" << 10 + i % 5 << R"(,"ph":"X","ts":)"
            << 1000 + i * 10 << R"(,"dur":)" << 3 + i % 50
            << R"(,"args":{"fname":"/data/s)" << seg << "/f" << (i * step) % 97
            << R"(","size":)" << (i * 17) % 4096 << R"(,"hhash":"h)"
            << (pick + variant) % 17 << R"(","path":"/p)" << seg % 5 << "/q"
            << (pick * 5 + i) % 53 << R"(/r)" << i % 31
            << R"(","note":"abcdefghijklmnopqrstuvwxyz0123456789"}})"
            << "\n";
    }
    out << "]\n";
    return dftu_utils_test::write_gz_trace(dir + "/" + stem + ".pfw.gz",
                                           out.str());
}

build::IndexBuildBatchResult build_with(const std::vector<std::string>& files,
                                        const std::string& index_dir,
                                        std::size_t workers,
                                        std::uint64_t budget) {
    build::IndexBuildBatchResult result;
    const std::string staging = index_dir + ".staging";
    auto counter = std::make_shared<std::atomic<std::size_t>>(0);
    Runtime rt(workers);
    auto task = run_coro_scope(
        rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
            auto config = std::make_shared<build::IndexBuildBatchConfig>();
            config->file_paths = files;
            config->index_dir = index_dir;
            config->parallelism = workers;
            config->memory_budget = budget;
            result = co_await build::BatchBuilder::process(&scope,
                                                           std::move(config));
        });
    rt.submit(std::move(task), "design-a").wait();
    rt.shutdown();
    return result;
}

struct Peak {
    std::size_t bytes = 0;
    std::size_t in_flight = 0;
};

Peak peak_of_build(const std::vector<std::string>& files,
                   const std::string& index_dir, std::size_t workers,
                   std::uint64_t budget) {
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
    const auto result = build_with(files, index_dir, workers, budget);
    done.store(true);
    sampler.join();
    REQUIRE(result.indexed == files.size());
    const auto top = peak.load();
    return {top > base ? top - base : 0, result.metrics.max_files_in_flight};
}

using Family = std::vector<std::pair<std::string, std::string>>;
using Dump = std::map<std::string, Family>;

Dump dump_index(const std::string& first_file, const std::string& index_dir) {
    const auto path =
        trace::internal::determine_index_path(first_file, index_dir);
    store::RocksDatabase db(path, store::RocksDatabase::OpenMode::ReadOnly);
    REQUIRE(db.is_open());
    Dump out;
    for (const auto cf : store::RocksDatabase::default_column_families()) {
        auto it = db.new_iterator(cf);
        Family& family = out[std::string(cf)];
        for (it->SeekToFirst(); it->Valid(); it->Next())
            family.emplace_back(it->key().ToString(), it->value().ToString());
        REQUIRE_MESSAGE(it->status().ok(), it->status().ToString());
    }
    return out;
}

void check_same(const Dump& a, const Dump& b, const char* what) {
    REQUIRE(a.size() == b.size());
    for (const auto& [name, rows] : a) {
        const auto it = b.find(name);
        REQUIRE(it != b.end());
        INFO(what << ": family " << name);
        REQUIRE(rows.size() == it->second.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i] != it->second[i]) {
                FAIL("row " << i << " differs in " << name << " (" << what
                            << ")");
            }
        }
    }
}

}  // namespace

TEST_SUITE("DesignA") {
    TEST_CASE("the peak stays under the budget at 2 and at 16 workers") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < SMALL_FILES; ++i)
            files.push_back(small_trace(env.get_dir(), i));

        const auto limit = build::g_max_sink_bytes.exchange(SINK_LIMIT);
        const auto few =
            peak_of_build(files, env.get_dir() + "/idx2", 2, BUDGET);
        const auto many =
            peak_of_build(files, env.get_dir() + "/idx16", 16, BUDGET);
        build::g_max_sink_bytes.store(limit);
        MESSAGE("peak live bytes: 2 workers "
                << few.bytes << " (" << few.in_flight
                << " files at once), 16 workers " << many.bytes << " ("
                << many.in_flight << " files at once)");
        CHECK(few.bytes < BUDGET + SLACK);
        CHECK(many.bytes < BUDGET + SLACK);
        // The budget, not the worker count, decides how many files run.
        CHECK(many.in_flight >= 4);
        CHECK(many.in_flight < 16);
        CHECK(few.in_flight <= 2);
    }

    TEST_CASE("one large file is granted a parse window of most workers") {
        constexpr std::size_t WORKERS = 8;
        // One file in flight and the rest of the workers free: the gate grants
        // a slot and the bytes for every other worker, and no more.
        build::ParseGate gate(nullptr, WORKERS);
        gate.file_in();
        REQUIRE(gate.spare_workers());
        std::size_t pieces = 0;
        memory::Reservation unused;
        gate.take_slot();
        ++pieces;
        while (gate.try_slot() && gate.try_piece(0, unused)) ++pieces;
        CHECK(pieces == WORKERS);
        CHECK_FALSE(gate.try_slot());
        for (std::size_t i = 0; i < pieces; ++i) gate.give_slot();

        // A file for every worker keeps each file to its own worker.
        build::ParseGate busy(nullptr, WORKERS);
        for (std::size_t i = 0; i < WORKERS; ++i) busy.file_in();
        CHECK_FALSE(busy.spare_workers());
    }

    TEST_CASE(
        "a file's plaintext estimate is one function for index and view") {
        using build::FileAdmission;
        constexpr std::uint64_t PACKED = 3 << 20;
        CHECK(FileAdmission::plaintext_estimate(0, PACKED) ==
              PACKED *
                  constants::file_admission::PLAINTEXT_PER_COMPRESSED_BYTE);
        CHECK(FileAdmission::plaintext_estimate(50 << 20, PACKED) ==
              (std::uint64_t{50} << 20));
    }

    TEST_CASE("the parse probes cost nothing and count only when enabled") {
        constexpr int BIG_LINES = SEGMENT_LINES * 8;
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        files.push_back(big_trace(env.get_dir(), "big", BIG_LINES, 0));

        build::g_probe_parse.store(false);
        build::g_max_parse_in_flight.store(0);
        const auto off = build_with(files, env.get_dir() + "/off", 4, 0);
        REQUIRE(off.indexed == 1);
        CHECK(build::g_max_parse_in_flight.load() == 0);

        build::g_probe_parse.store(true);
        const auto on = build_with(files, env.get_dir() + "/on", 4, 0);
        build::g_probe_parse.store(false);
        REQUIRE(on.indexed == 1);
        CHECK(build::g_max_parse_in_flight.load() >= 1);
        CHECK(build::g_max_parse_in_flight.load() <= 5);
    }

    TEST_CASE("a parallel build equals a one worker build byte for byte") {
        constexpr std::size_t WORKERS = 8;
        constexpr int LINES = SEGMENT_LINES * 12;
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        files.push_back(big_trace(env.get_dir(), "a", LINES, 1));
        files.push_back(small_trace(env.get_dir(), 0));

        build::g_probe_intern_order.store(true);
        build::g_intern_order_digest.store(0);
        const auto one = build_with(files, env.get_dir() + "/idx1", 1, 0);
        REQUIRE(one.indexed == files.size());
        const auto order = build::g_intern_order_digest.load();
        const auto expect = dump_index(files[0], env.get_dir() + "/idx1");
        for (int run = 0; run < 3; ++run) {
            const auto dir = env.get_dir() + "/idx_par" + std::to_string(run);
            build::g_intern_order_digest.store(0);
            const auto many = build_with(files, dir, WORKERS, 0);
            REQUIRE(many.indexed == files.size());
            check_same(expect, dump_index(files[0], dir), "8 workers");
            CHECK(build::g_intern_order_digest.load() == order);
        }
        build::g_probe_intern_order.store(false);
    }
}
