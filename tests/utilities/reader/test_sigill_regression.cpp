// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <checkpoint_indexer_helpers.h>
#include <dftracer/utils/coro/detail/frame.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/memory/detail/heap.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/reader/internal/gzip_reader.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_runtime.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;

namespace {

// Leaves every heap size class a coroutine frame can use holding free blocks
// full of 0xFF, as a long run leaves them after other coroutines finish.
void dirty_frame_pool() {
    for (std::size_t size = 8; size <= 2048; size += 8) {
        dstd::Vector<void*> blocks;
        for (int i = 0; i < 16; ++i) {
            void* p = memory::detail::allocate(size, coro::detail::FRAME_ALIGN);
            std::memset(p, 0xFF, size);
            blocks.push_back(p);
        }
        for (void* p : blocks) memory::detail::deallocate(p, size);
    }
}

dstd::String numbered_lines(std::size_t bytes) {
    dstd::String s;
    s.reserve(bytes + 64);
    for (std::size_t i = 0; s.size() < bytes; ++i) {
        s += "{\"id\":" + dstd::to_string(i) + ",\"pad\":\"";
        s.append(40, 'x');
        s += "\"}\n";
    }
    return s;
}

}  // namespace

TEST_SUITE("sigill_regression") {
    TEST_CASE(
        "sigill_regression: a line stream restarts decompression on "
        "recycled coroutine frames") {
        const auto text = numbered_lines(1u << 20);
        fileio::compress::GzipMemberCompressor c;
        dstd::Vector<std::uint8_t> comp(c.bound(text.size()));
        auto n = c.compress(text.data(), text.size(), comp.data(), comp.size());
        REQUIRE(n.has_value());
        comp.resize(*n);

        dftu_utils_test::ScopedTestDir dir("sigill");
        const dstd::String gz_path = dir.file("t.gz");
        std::FILE* f = std::fopen(gz_path.c_str(), "wb");
        REQUIRE(f != nullptr);
        REQUIRE(std::fwrite(comp.data(), 1, comp.size(), f) == comp.size());
        std::fclose(f);
        auto indexer = dstd::make_shared<index::gzip::GzipIndexer>(
            dstd::String(dstd::StringView(gz_path)),
            dstd::String(dstd::StringView(dir.file("idx"))), 1u << 20, true);
        dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            co_await dftu_utils_test::build_indexer_async(*indexer);
        });

        const std::size_t start = text.size() / 2;
        const std::size_t line_start = text.rfind('\n', start - 1) + 1;

        dirty_frame_pool();

        reader::internal::GzipReader reader(indexer);
        auto stream = dftu_utils_test::ok(reader.stream(
            reader::internal::StreamConfig()
                .stream_type(reader::internal::StreamType::MultiLinesBytes)
                .range_type(reader::internal::RangeType::ByteRange)
                .from(start)
                .to(reader.max_bytes())));
        dstd::String out;
        dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            dstd::Vector<char> buf(1u << 20);
            while (true) {
                const auto got = dftu_utils_test::ok(
                    co_await stream->read_async(buf.data(), buf.size()));
                if (got == 0) break;
                out.append(buf.data(), got);
            }
        });
        CHECK(out == text.substr(line_start));
    }
}
