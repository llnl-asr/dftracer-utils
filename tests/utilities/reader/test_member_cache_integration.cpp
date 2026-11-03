// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/coro.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/reader/internal/member_decode_cache.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>
#include <testing_utilities.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::index::build;
using namespace dftracer::utils::utilities;

namespace {

dstd::String make_trace(int n) {
    dstd::String s = "[\n";
    for (int i = 0; i < n; ++i) {
        s += "{\"id\":" + dstd::to_string(i) +
             ",\"name\":\"op\",\"cat\":\"POSIX\",\"ts\":" +
             dstd::to_string(1000 + i) + ",\"dur\":10,\"ph\":\"X\"}\n";
    }
    return s;
}

void write_single_member(const dstd::String& path,
                         const dstd::String& content) {
    fileio::compress::GzipMemberCompressor comp;
    auto m = comp.compress_member(content.data(), content.size());
    REQUIRE(m.has_value());
    dstd::String ofs;
    ofs.append(reinterpret_cast<const char*>(m->data()), m->size());
    (void)dstd::fs::write_file(dstd::PathView(path), ofs);
}

std::int64_t read_line_count(const dstd::String& gz, const dstd::String& idx) {
    std::int64_t count = 0;
    dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        reader::TraceReader reader(
            reader::TraceReaderConfig{dstd::String(gz), dstd::String(idx)});
        auto gen = reader.read_lines(reader::ReadConfig{});
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
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String src = dstd::String((dir / "src.pfw.gz").str());
        const dstd::String gz = dstd::String((dir / "trace.pfw.gz").str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());

        write_single_member(src, make_trace(n));
        dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            co_await utilities::fileio::compress::gzip_rechunk_to_members(
                dstd::String(src), dstd::String(gz), /*member_size=*/4096,
                /*level=*/6);
            co_return;
        });
        {
            dftu_utils_test::run_coro(
                [&](coro::Scope&) -> coro::CoroTask<void> {
                    auto config = dstd::make_shared<IndexBuildBatchConfig>();
                    config->file_paths = {dstd::String(dstd::StringView(gz))};
                    config->index_dir = index_dir;
                    auto r = co_await BatchBuilder::process(std::move(config));
                    REQUIRE(r.indexed + r.skipped == 1);
                    co_return;
                });
        }

        // Baseline before the cache is configured (plain per-reader decode).
        REQUIRE(reader::internal::global_member_decode_cache() == nullptr);
        const std::int64_t baseline = read_line_count(gz, index_dir);
        CHECK(baseline >= n);  // n events plus the leading "[" line

        // Enable the process cache and read the same file concurrently.
        reader::internal::configure_global_member_decode_cache(256ull * 1024 *
                                                               1024);
        auto* cache = reader::internal::global_member_decode_cache();
        REQUIRE(cache != nullptr);
        const auto before = cache->stats();

        const int readers = 16;
        dstd::Vector<std::int64_t> counts(readers, 0);
        dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            dstd::Vector<coro::CoroTask<std::int64_t>> tasks;
            tasks.reserve(readers);
            for (int r = 0; r < readers; ++r) {
                tasks.push_back(
                    [](dstd::String gzp,
                       dstd::String idxp) -> coro::CoroTask<std::int64_t> {
                        reader::TraceReader reader(reader::TraceReaderConfig{
                            dstd::String(gzp), dstd::String(idxp)});
                        auto gen = reader.read_lines(reader::ReadConfig{});
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

        (void)dstd::fs::remove_all(dir);
    }
}
