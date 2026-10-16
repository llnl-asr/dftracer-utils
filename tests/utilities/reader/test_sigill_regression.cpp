#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/object_pool.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/reader/internal/gzip_reader.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::index::gzip::GzipIndexer;
using dftracer::utils::utilities::reader::internal::GzipReader;
using dftracer::utils::utilities::reader::internal::RangeType;
using dftracer::utils::utilities::reader::internal::StreamConfig;
using dftracer::utils::utilities::reader::internal::StreamType;
using dftu_utils_test::run_coro;
using dftu_utils_test::ScopedTestDir;
namespace gz = dftracer::utils::utilities::fileio::compress;

namespace {

// Leaves every pooled coroutine frame size class holding recycled blocks full
// of 0xFF, as a long run leaves them after other coroutines finish.
void dirty_frame_pool() {
    auto& pool = ObjectPool::instance();
    for (std::size_t size = 8; size <= 2048; size += 8) {
        std::vector<void*> blocks;
        for (int i = 0; i < 16; ++i) {
            void* p = pool.allocate(size);
            std::memset(p, 0xFF, size);
            blocks.push_back(p);
        }
        for (void* p : blocks) pool.deallocate(p, size);
    }
}

std::string numbered_lines(std::size_t bytes) {
    std::string s;
    s.reserve(bytes + 64);
    for (std::size_t i = 0; s.size() < bytes; ++i) {
        s += "{\"id\":" + std::to_string(i) + ",\"pad\":\"";
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
        gz::GzipMemberCompressor c;
        std::vector<std::uint8_t> comp(c.bound(text.size()));
        auto n = c.compress(text.data(), text.size(), comp.data(), comp.size());
        REQUIRE(n.has_value());
        comp.resize(*n);

        ScopedTestDir dir("sigill");
        const std::string gz_path = dir.file("t.gz");
        std::FILE* f = std::fopen(gz_path.c_str(), "wb");
        REQUIRE(f != nullptr);
        REQUIRE(std::fwrite(comp.data(), 1, comp.size(), f) == comp.size());
        std::fclose(f);
        auto indexer = std::make_shared<GzipIndexer>(gz_path, dir.file("idx"),
                                                     1u << 20, true);
        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            co_await indexer->build_async();
        });

        const std::size_t start = text.size() / 2;
        const std::size_t line_start = text.rfind('\n', start - 1) + 1;

        dirty_frame_pool();

        GzipReader reader(indexer);
        auto stream =
            reader.stream(StreamConfig()
                              .stream_type(StreamType::MultiLinesBytes)
                              .range_type(RangeType::ByteRange)
                              .from(start)
                              .to(reader.get_max_bytes()));
        std::string out;
        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            std::vector<char> buf(1u << 20);
            while (true) {
                const auto got =
                    co_await stream->read_async(buf.data(), buf.size());
                if (got == 0) break;
                out.append(buf.data(), got);
            }
        });
        CHECK(out == text.substr(line_start));
    }
}
