#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <memory>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/reader/internal/chunk_geometry.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <testing_runtime.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer;
using dftracer::utils::utilities::reader::ReadConfig;
using dftracer::utils::utilities::reader::TraceReader;
using dftracer::utils::utilities::reader::TraceReaderConfig;
using dftracer::utils::utilities::reader::internal::enumerate_work_items;
using dftu_utils_test::run_coro;

namespace {

// One event per line; ts = 1000 + i, dur = 100. Returns "[\n" + lines.
std::string make_trace(int n) {
    std::string s = "[\n";
    for (int i = 0; i < n; ++i) {
        s += "{\"id\":" + std::to_string(i) +
             ",\"name\":\"op\",\"cat\":\"POSIX\",\"pid\":1,\"tid\":1,\"ts\":" +
             std::to_string(1000 + i) + ",\"dur\":100,\"ph\":\"X\"}\n";
    }
    return s;
}

std::string write_single_member(const std::string& path,
                                const std::string& content) {
    using dftracer::utils::utilities::fileio::compress::GzipMemberCompressor;
    GzipMemberCompressor comp;
    auto member = comp.compress_member(content.data(), content.size());
    REQUIRE(member.has_value());
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(member->data()),
              static_cast<std::streamsize>(member->size()));
    return path;
}

// Sum rows read for `query` across the enumerated work items, replicating the
// worker's ArrowWorkItem -> ReadConfig mapping. `apply_sub` toggles the
// sub-chunk skip so the same read can run with and without it.
std::int64_t read_count(const std::string& gz, const std::string& index_dir,
                        const std::string& query, bool apply_sub) {
    std::int64_t total = 0;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        auto items = enumerate_work_items({gz}, index_dir, query, 4);
        TraceReader reader(TraceReaderConfig{gz, index_dir});
        for (const auto& item : items) {
            ReadConfig rc;
            rc.query = query;
            rc.skip_pruning = true;
            rc.start_byte = item.start_byte;
            rc.end_byte = item.end_byte;
            rc.start_at_checkpoint = item.start_at_checkpoint;
            rc.end_at_checkpoint = item.end_at_checkpoint;
            if (item.chunk_prune_only) rc.chunk_prune_only = true;
            if (apply_sub) {
                rc.sub_event_counts = item.sub_event_counts;
                rc.sub_keep = item.sub_keep;
            }
            auto gen = reader.read_arrow(rc, 4096);
            while (auto batch = co_await gen.next()) {
                total += batch->num_rows();
            }
        }
        co_return;
    });
    return total;
}

void build_index(const std::string& gz, const std::string& index_dir,
                 std::size_t sub_chunk_events) {
    run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
        auto config = std::make_shared<IndexBuildBatchConfig>();
        config->file_paths = {gz};
        config->index_dir = index_dir;
        config->bloom_config.sub_chunk_events = sub_chunk_events;
        auto r = co_await IndexBatchBuilderUtility::process(&scope,
                                                            std::move(config));
        REQUIRE(r.indexed + r.skipped == 1);
    });
}

}  // namespace

TEST_SUITE("sub_chunk_skip") {
    TEST_CASE("skip returns the same rows as a full scan (multi-member)") {
        const int n = 4000;
        auto dir = dftu_utils_test::make_unique_test_path("sub_skip");
        fs::create_directories(dir);
        const std::string src = (dir / "src.pfw.gz").string();
        const std::string gz = (dir / "trace.pfw.gz").string();
        const std::string index_dir = (dir / "idx").string();

        write_single_member(src, make_trace(n));
        // Rechunk into many small (\n-led) members so reads cross boundaries.
        namespace gzc = utilities::fileio::compress;
        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            co_await gzc::gzip_rechunk_to_members(src, gz, /*member_size=*/4096,
                                                  /*level=*/6);
            co_return;
        });
        // Small buckets so a ts window excludes many within each member.
        build_index(gz, index_dir, /*sub_chunk_events=*/64);

        // ts = 1000 + i; window [2000, 3000] -> i in [1000, 2000] -> 1001 rows.
        const std::string query = "ts >= 2000 and ts <= 3000";
        const std::int64_t expected = 1001;

        std::int64_t with_skip = read_count(gz, index_dir, query, true);
        std::int64_t without_skip = read_count(gz, index_dir, query, false);

        CHECK(without_skip == expected);  // baseline: full scan is correct
        CHECK(with_skip == expected);     // skip drops no matching event

        fs::remove_all(dir);
    }
}

#else
TEST_SUITE("sub_chunk_skip") {
    TEST_CASE("arrow disabled") { CHECK(true); }
}
#endif
