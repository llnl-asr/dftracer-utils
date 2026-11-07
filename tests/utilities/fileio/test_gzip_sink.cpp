// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/gzip_sink.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <iterator>

using namespace dftracer::utils;

namespace {

// Inflates a multi-member gzip; returns the bytes and each member's last byte.
dstd::String decode(const dstd::String& path, dstd::String& last_bytes) {
    dstd::String gz =
        dstd::fs::read_file(dstd::PathView(path)).value_or(dstd::String());
    z_stream zs{};
    REQUIRE(inflateInit2(&zs, 15 + 16) == Z_OK);
    zs.next_in = reinterpret_cast<Bytef*>(gz.data());
    zs.avail_in = static_cast<uInt>(gz.size());
    dstd::String out;
    dstd::Vector<char> buf(1 << 16);
    int rc = Z_OK;
    std::size_t member_start = 0;
    while (zs.avail_in > 0) {
        zs.next_out = reinterpret_cast<Bytef*>(buf.data());
        zs.avail_out = static_cast<uInt>(buf.size());
        rc = inflate(&zs, Z_NO_FLUSH);
        REQUIRE((rc == Z_OK || rc == Z_STREAM_END));
        out.append(buf.data(), buf.size() - zs.avail_out);
        if (rc == Z_STREAM_END) {
            REQUIRE(out.size() > member_start);
            last_bytes.push_back(out.back());
            member_start = out.size();
            REQUIRE(inflateReset(&zs) == Z_OK);
        }
    }
    inflateEnd(&zs);
    return out;
}

}  // namespace

TEST_CASE("GzipSink - pieces split mid-line") {
    auto dir = dftu_utils_test::make_unique_test_path("dftracer_test_sink");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String path = dstd::String((dir / "out.pfw.gz").str());

    dstd::String expected;
    for (int i = 0; i < 9000; ++i) {
        expected += "{\"id\":" + dstd::to_string(i) + ",\"p\":\"" +
                    dstd::String(1000, 'x') + "\"}\n";
    }
    expected += "{\"unterminated\":1}";
    {
        python::GzipSink sink(path, 6);
        for (std::size_t off = 0; off < expected.size(); off += 7777) {
            sink.write(dstd::StringView(expected).substr(off, 7777));
        }
    }

    dstd::String last;
    CHECK(decode(path, last) == expected);
    REQUIRE(last.size() >= 2);
    for (std::size_t i = 0; i + 1 < last.size(); ++i) CHECK(last[i] == '\n');
    (void)dstd::fs::remove_all(dir);
}
