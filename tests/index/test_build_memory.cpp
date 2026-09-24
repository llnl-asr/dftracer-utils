#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <rocksdb/iterator.h>
#include <testing_utilities.h>

#include <fstream>
#include <sstream>
#include <string>
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
                        int events, int paths) {
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
                                                               16 * 1024));
    fs::remove(plain);
    return gz;
}

build::IndexBuildBatchResult run_build(const std::vector<std::string>& files,
                                       const std::string& index_dir,
                                       std::uint64_t memory_budget,
                                       std::size_t path_budget = 1024,
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
    }
    return out;
}

std::string index_of(const std::string& trace, const std::string& dir) {
    return trace::internal::determine_index_path(trace, dir);
}

}  // namespace

TEST_SUITE("IndexBuildMemory") {
    TEST_CASE("large files are admitted one at a time") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < 4; ++i)
            files.push_back(
                write_trace(env.get_dir(), "t" + std::to_string(i), 2000, 20));
        // Each file's estimate is at least 64 MB, above half of 100 MB.
        const auto r = run_build(files, env.get_dir() + "/idx", 100ULL << 20);
        CHECK(r.indexed == 4);
        CHECK(r.failed == 0);
        CHECK(r.metrics.max_files_in_flight == 1);
    }

    TEST_CASE("a spilled build writes the same index as an unbounded one") {
        dftu_utils_test::TestEnvironment env(10);
        const std::vector<std::string> files = {
            write_trace(env.get_dir(), "narrow", 6000, 12),
            write_trace(env.get_dir(), "wide", 6000, 600)};
        for (std::size_t path_budget : {std::size_t{1024}, std::size_t{40}}) {
            CAPTURE(path_budget);
            const auto big_dir =
                env.get_dir() + "/big" + std::to_string(path_budget);
            const auto small_dir =
                env.get_dir() + "/small" + std::to_string(path_budget);
            const auto big =
                run_build(files, big_dir, NO_SPILL_BUDGET, path_budget);
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
}
