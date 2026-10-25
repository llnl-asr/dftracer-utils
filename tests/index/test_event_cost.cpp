// Per-event cost of the index fold: schema leaves are interned once per record
// shape, chunk statistics are found by interned id, a bloom insert hashes its
// value once, and the stored index does not change. The cases count work, not
// time.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/event_cost.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <rocksdb/iterator.h>
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
// Distinct leaf paths over the three record shapes of write_trace.
constexpr std::uint64_t LEAF_PATHS = 24;
constexpr std::uint64_t MIN_REDUCTION = 20;
// Three record shapes in rotation: flat args, no args and nested args with a
// null and an array. Names, categories and pids repeat, as in a real trace.
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
    // The index stores the trace's modification time.
    const timespec fixed[2] = {{FIXED_MTIME, 0}, {FIXED_MTIME, 0}};
    REQUIRE(::utimensat(AT_FDCWD, path.c_str(), fixed, 0) == 0);
    return path;
}

void build_one(const std::string& file, const std::string& index_dir) {
    Runtime rt(4);
    build::IndexBuildBatchResult result;
    auto task = run_coro_scope(
        rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
            auto config = std::make_shared<build::IndexBuildBatchConfig>();
            config->file_paths = {file};
            config->index_dir = index_dir;
            config->parallelism = 4;
            result = co_await build::BatchBuilder::process(&scope,
                                                           std::move(config));
        });
    rt.submit(std::move(task), "event-cost").wait();
    rt.shutdown();
    REQUIRE(result.indexed == 1);
}

// The digest of each column family of the index write_trace builds, recorded
// from the build before the per-event cost work. The members digest, which
// holds compressed offsets, was recorded again when the trace moved to stored
// gzip blocks, on a build whose other digests still matched. A family that
// holds no rows digests to the FNV offset.
struct FamilyDigest {
    std::string_view family;
    std::uint64_t digest;
};
constexpr FamilyDigest RECORDED[] = {
    {"default", 12842990505823536473ULL},
    {"members", 15479869490099263789ULL},
    {"granule", 7655030195474814324ULL},
    {"blob", 5112685343885646931ULL},
    {"postings", 1070782933062292960ULL},
    {"system_metrics", 1469598103934665603ULL},
    {"rollup", 1469598103934665603ULL},
};

// FNV-1a of every key and value of one column family. A row that names the
// test directory (the trace's path, whose length varies) is left out.
std::uint64_t family_digest(store::RocksDatabase& db, std::string_view cf,
                            const std::string& dir) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&h](std::string_view bytes) {
        for (unsigned char c : bytes) h = (h ^ c) * 1099511628211ULL;
        h = (h ^ 0xFF) * 1099511628211ULL;
    };
    auto it = db.new_iterator(cf);
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const std::string_view key = it->key().ToStringView();
        const std::string_view value = it->value().ToStringView();
        if (key.find(dir) != std::string_view::npos ||
            value.find(dir) != std::string_view::npos)
            continue;
        mix(key);
        mix(value);
    }
    REQUIRE(it->status().ok());
    return h;
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
            std::vector<std::pair<std::string, std::uint64_t>> out;
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

    TEST_CASE("the index of a fixed trace is unchanged") {
        dftu_utils_test::TestEnvironment env(10);
        const std::string file = write_trace(env.get_dir());
        build_one(file, env.get_dir() + "/idx");
        const std::string path =
            trace::internal::determine_index_path(file, env.get_dir() + "/idx");
        store::RocksDatabase db(path, store::RocksDatabase::OpenMode::ReadOnly);
        REQUIRE(db.is_open());
        std::size_t checked = 0;
        for (const auto cf : store::RocksDatabase::default_column_families()) {
            const std::uint64_t digest = family_digest(db, cf, env.get_dir());
            MESSAGE("family " << std::string(cf) << " " << digest);
            for (const auto& want : RECORDED)
                if (want.family == cf) {
                    CHECK(digest == want.digest);
                    ++checked;
                }
        }
        CHECK(checked == std::size(RECORDED));
    }
}
