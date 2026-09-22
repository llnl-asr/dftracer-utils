#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/reader/internal/member_decode_cache.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>
#include <testing_utilities.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer;
namespace mdc = dftracer::utils::utilities::reader::internal;
using dftracer::utils::utilities::reader::ReadConfig;
using dftracer::utils::utilities::reader::TraceReader;
using dftracer::utils::utilities::reader::TraceReaderConfig;
using dftu_utils_test::run_coro;

namespace {

std::string make_trace(int n) {
    std::string s = "[\n";
    for (int i = 0; i < n; ++i) {
        s += "{\"id\":" + std::to_string(i) +
             ",\"name\":\"op\",\"cat\":\"POSIX\",\"ts\":" +
             std::to_string(1000 + i) + ",\"dur\":10,\"ph\":\"X\"}\n";
    }
    return s;
}

void write_single_member(const std::string& path, const std::string& content) {
    using dftracer::utils::utilities::fileio::compress::GzipMemberCompressor;
    GzipMemberCompressor comp;
    auto m = comp.compress_member(content.data(), content.size());
    REQUIRE(m.has_value());
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(m->data()),
              static_cast<std::streamsize>(m->size()));
}

std::int64_t read_line_count(const std::string& gz, const std::string& idx) {
    std::int64_t count = 0;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        TraceReader reader(TraceReaderConfig{gz, idx});
        auto gen = reader.read_lines(ReadConfig{});
        while (co_await gen.next()) ++count;
        co_return;
    });
    return count;
}

}  // namespace

TEST_SUITE("member_cache_integration") {
    TEST_CASE("cached reads match uncached, and coalesce decodes") {
        const int n = 4000;
        auto dir = dftu_utils_test::make_unique_test_path("mcache");
        fs::create_directories(dir);
        const std::string src = (dir / "src.pfw.gz").string();
        const std::string gz = (dir / "trace.pfw.gz").string();
        const std::string index_dir = (dir / "idx").string();

        write_single_member(src, make_trace(n));
        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            co_await utilities::fileio::compress::gzip_rechunk_to_members(
                src, gz, /*member_size=*/4096, /*level=*/6);
            co_return;
        });
        {
            run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
                auto config = std::make_shared<IndexBuildBatchConfig>();
                config->file_paths = {gz};
                config->index_dir = index_dir;
                auto r = co_await IndexBatchBuilderUtility::process(
                    &scope, std::move(config));
                REQUIRE(r.indexed + r.skipped == 1);
                co_return;
            });
        }

        // Baseline before the cache is configured (plain per-reader decode).
        REQUIRE(mdc::global_member_decode_cache() == nullptr);
        const std::int64_t baseline = read_line_count(gz, index_dir);
        CHECK(baseline >= n);  // n events plus the leading "[" line

        // Enable the process cache and read the same file concurrently.
        mdc::configure_global_member_decode_cache(256ull * 1024 * 1024);
        auto* cache = mdc::global_member_decode_cache();
        REQUIRE(cache != nullptr);
        const auto before = cache->stats();

        const int readers = 16;
        std::vector<std::int64_t> counts(readers, 0);
        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            std::vector<coro::CoroTask<std::int64_t>> tasks;
            tasks.reserve(readers);
            for (int r = 0; r < readers; ++r) {
                tasks.push_back(
                    [](std::string gzp,
                       std::string idxp) -> coro::CoroTask<std::int64_t> {
                        TraceReader reader(TraceReaderConfig{gzp, idxp});
                        auto gen = reader.read_lines(ReadConfig{});
                        std::int64_t c = 0;
                        while (co_await gen.next()) ++c;
                        co_return c;
                    }(gz, index_dir));
            }
            auto out = co_await coro::when_all(std::move(tasks));
            for (int r = 0; r < readers; ++r) counts[r] = out[r];
            co_return;
        });

        // Equivalence: every cached reader sees exactly the baseline lines.
        for (int r = 0; r < readers; ++r) CHECK(counts[r] == baseline);

        const auto after = cache->stats();
        const std::uint64_t reqs = after.requests - before.requests;
        const std::uint64_t decs = after.decodes - before.decodes;
        MESSAGE("member requests=" << reqs << " decodes=" << decs
                                   << " (readers=" << readers << ")");
        // Coalescing: far fewer decodes than requests. Without the cache this
        // would be `readers` full decodes of every member.
        CHECK(reqs > 0);
        CHECK(decs < reqs);
        CHECK(decs * 2 < reqs);

        fs::remove_all(dir);
    }
}
