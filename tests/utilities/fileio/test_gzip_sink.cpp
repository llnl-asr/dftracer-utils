#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/python/gzip_sink.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using dftracer::utils::python::GzipSink;

namespace {

// Inflates a multi-member gzip; returns the bytes and each member's last byte.
std::string decode(const std::string& path, std::string& last_bytes) {
    std::ifstream in(path, std::ios::binary);
    std::string gz((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
    z_stream zs{};
    REQUIRE(inflateInit2(&zs, 15 + 16) == Z_OK);
    zs.next_in = reinterpret_cast<Bytef*>(gz.data());
    zs.avail_in = static_cast<uInt>(gz.size());
    std::string out;
    std::vector<char> buf(1 << 16);
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
    fs::create_directories(dir);
    const std::string path = (dir / "out.pfw.gz").string();

    std::string expected;
    for (int i = 0; i < 9000; ++i) {
        expected += "{\"id\":" + std::to_string(i) + ",\"p\":\"" +
                    std::string(1000, 'x') + "\"}\n";
    }
    expected += "{\"unterminated\":1}";
    {
        GzipSink sink(path, 6);
        for (std::size_t off = 0; off < expected.size(); off += 7777) {
            sink.write(std::string_view(expected).substr(off, 7777));
        }
    }

    std::string last;
    CHECK(decode(path, last) == expected);
    REQUIRE(last.size() >= 2);
    for (std::size_t i = 0; i + 1 < last.size(); ++i) CHECK(last[i] == '\n');
    fs::remove_all(dir);
}
