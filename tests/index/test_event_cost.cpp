// SPDX-License-Identifier: MIT
// The cases count work, not time.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/event_cost.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <testing_utilities.h>

#include <cstdint>
#include <ctime>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;
namespace cost = dftracer::utils::index::event_cost;
namespace store = dftracer::utils::index::store;
namespace ext = dftracer::utils::index::extensions;
namespace ix = dftracer::utils::index;

namespace {

constexpr int EVENTS = 6000;
constexpr int SHAPES = 3;
constexpr std::time_t FIXED_MTIME = 1700000000;
constexpr std::uint64_t LEAF_PATHS = 24;
constexpr std::uint64_t MIN_REDUCTION = 20;
std::string write_trace(const std::string& dir) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < EVENTS; ++i) {
        const int pid = 1 + i % 4;
        const int ts = 1000 + i * 10;
        out << R"({"id":)" << i;
        switch (i % SHAPES) {
            case 0:
                out << R"(,"name":"read","cat":"POSIX","pid":)" << pid
                    << R"(,"tid":1,"ph":"X","ts":)" << ts
                    << R"(,"dur":3,"args":{"fname":"/data/f)" << i % 37
                    << R"(","size":)" << i % 4096
                    << R"(,"hhash":"a1b2c3d4e5f60718","level":)" << i % 3
                    << "}}\n";
                break;
            case 1:
                out << R"(,"name":"compute","cat":"APP","pid":)" << pid
                    << R"(,"tid":2,"ph":"X","ts":)" << ts << R"(,"dur":7})"
                    << "\n";
                break;
            default:
                out << R"(,"name":"write","cat":"POSIX","pid":)" << pid
                    << R"(,"tid":1,"ph":"X","ts":)" << ts
                    << R"(,"dur":5,"args":{"io":{"off":)" << i
                    << R"(,"len":512},"hosts":["n1","n2"],"note":null,)"
                    << R"("ratio":0.5}})"
                    << "\n";
        }
    }
    out << "]\n";
    const std::string path = dftu_utils_test::write_stored_gz_trace(
        dir + "/trace.pfw.gz", out.str());
    const timespec fixed[2] = {{FIXED_MTIME, 0}, {FIXED_MTIME, 0}};
    REQUIRE(::utimensat(AT_FDCWD, path.c_str(), fixed, 0) == 0);
    return path;
}

void build_one(const std::string& file, const std::string& index_dir) {
    Runtime rt(4);
    build::IndexBuildBatchResult result;
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        auto config = std::make_shared<build::IndexBuildBatchConfig>();
        config->file_paths = {file};
        config->index_dir = index_dir;
        config->parallelism = 4;
        result = co_await build::BatchBuilder::process(std::move(config));
    });
    rt.submit(std::move(task), "event-cost").wait();
    rt.shutdown();
    REQUIRE(result.indexed == 1);
}

}  // namespace

TEST_SUITE("IndexEventCost") {
    TEST_CASE(
        "schema leaf paths are interned per record shape, not per event") {
        dftu_utils_test::TestEnvironment env(10);
        const std::string file = write_trace(env.get_dir());
        const auto before = cost::g_leaf_path_interns.load();
        build_one(file, env.get_dir() + "/idx");
        const auto interns = cost::g_leaf_path_interns.load() - before;
        MESSAGE("leaf path interns: " << interns << " for " << EVENTS
                                      << " events");
        CHECK(interns > 0);
        CHECK(interns <= LEAF_PATHS * SHAPES * 8);
        CHECK(interns * MIN_REDUCTION < std::uint64_t{EVENTS} * LEAF_PATHS);
    }

    TEST_CASE("chunk statistics find known keys by interned id") {
        dftu_utils_test::TestEnvironment env(10);
        const std::string file = write_trace(env.get_dir());
        const auto before = cost::g_stats_string_lookups.load();
        build_one(file, env.get_dir() + "/idx");
        const auto lookups = cost::g_stats_string_lookups.load() - before;
        MESSAGE("statistics string lookups: " << lookups << " for " << EVENTS
                                              << " events");
        CHECK(lookups * MIN_REDUCTION < std::uint64_t{EVENTS});
    }

    TEST_CASE("a bloom insert computes its hashes once") {
        ext::ScalableBloomFilter bloom(64, 0.01);
        constexpr std::size_t DISTINCT = 500;
        for (std::size_t i = 0; i < DISTINCT; ++i)
            bloom.add("value_" + std::to_string(i));
        CHECK(bloom.hash_computations() == DISTINCT);
        for (std::size_t i = 0; i < DISTINCT; ++i)
            bloom.add("value_" + std::to_string(i));
        CHECK(bloom.hash_computations() == 2 * DISTINCT);
        for (std::size_t i = 0; i < DISTINCT; ++i)
            CHECK(bloom.possibly_contains("value_" + std::to_string(i)));
        CHECK(bloom.size_bytes() > 0);
    }

    TEST_CASE("statistics found by slot equal statistics found by string") {
        using Stats = ix::schemas::dft::ChunkStatistics;
        constexpr const char* NAMES[] = {"read", "write", "open", "close",
                                         "seek", "stat",  "sync"};
        constexpr const char* CATS[] = {"POSIX", "STDIO", "APP"};
        Stats by_string;
        Stats by_slot;
        // Fewer slots than keys, so a slot is refilled while its key is live.
        constexpr std::size_t SLOT_COUNT = 4;
        Stats::NameSlots names[SLOT_COUNT];
        Stats::PidSlots pids[SLOT_COUNT];
        std::size_t name_key[SLOT_COUNT] = {};
        std::uint64_t pid_key[SLOT_COUNT] = {};
        std::uint64_t ts = 100;
        for (int i = 0; i < 3000; ++i) {
            const std::size_t n = static_cast<std::size_t>((i * 5 + i / 7) % 7);
            const std::size_t c = static_cast<std::size_t>((i / 3) % 3);
            const std::uint64_t pid = 1 + (i / 5) % 5;
            const std::uint64_t tid = 1 + i % 3;
            const bool has_dur = i % 11 != 0;
            ts += 1 + static_cast<std::uint64_t>(i % 13);
            const std::uint64_t dur = 5 + static_cast<std::uint64_t>(i % 97);
            by_string.update_from_event(NAMES[n], CATS[c], pid, tid, ts, dur,
                                        has_dur);
            const std::size_t nk = n * 3 + c + 1;
            const std::size_t na = nk % SLOT_COUNT;
            if (name_key[na] != nk) {
                names[na] = {};
                name_key[na] = nk;
            }
            const std::uint64_t pk = pid * 8 + tid;
            const std::size_t pa = static_cast<std::size_t>(pk) % SLOT_COUNT;
            if (pid_key[pa] != pk) {
                pids[pa] = {};
                pid_key[pa] = pk;
            }
            by_slot.update_from_event(names[na], pids[pa], NAMES[n], CATS[c],
                                      pid, tid, ts, dur, has_dur);
        }
        const auto keys = [](const auto& map) {
            std::vector<dstd::Pair<std::string, std::uint64_t>> out;
            for (const auto& [k, v] : map)
                out.emplace_back(k, static_cast<std::uint64_t>(v));
            return out;
        };
        CHECK(keys(by_slot.category_counts) == keys(by_string.category_counts));
        CHECK(keys(by_slot.name_counts) == keys(by_string.name_counts));
        CHECK(keys(by_slot.pid_tid_counts) == keys(by_string.pid_tid_counts));
        CHECK(by_slot.total_events == by_string.total_events);
        CHECK(by_slot.duration_count == by_string.duration_count);
        CHECK(by_slot.name_category_json() == by_string.name_category_json());
        CHECK(by_slot.name_duration_sums_json() ==
              by_string.name_duration_sums_json());
        CHECK(by_slot.name_duration_sum_sqs_json() ==
              by_string.name_duration_sum_sqs_json());
        CHECK(by_slot.name_duration_histograms_json() ==
              by_string.name_duration_histograms_json());
        CHECK(by_slot.cat_duration_sums_json() ==
              by_string.cat_duration_sums_json());
        CHECK(by_slot.pid_duration_sums_json() ==
              by_string.pid_duration_sums_json());
        CHECK(by_slot.serialize_name_duration_sketches() ==
              by_string.serialize_name_duration_sketches());
        CHECK(Stats::serialize_sketch_map(by_slot.cat_duration_sketches) ==
              Stats::serialize_sketch_map(by_string.cat_duration_sketches));
        CHECK(Stats::serialize_sketch_map(by_slot.pid_duration_sketches) ==
              Stats::serialize_sketch_map(by_string.pid_duration_sketches));
    }
}
