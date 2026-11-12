// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>

using namespace dftracer::utils;

namespace {

dstd::Vector<std::uint8_t> gzip(const dstd::String& text) {
    fileio::compress::GzipMemberCompressor c;
    dstd::Vector<std::uint8_t> out(c.bound(text.size()));
    auto n = c.compress(text.data(), text.size(), out.data(), out.size());
    REQUIRE(n.has_value());
    out.resize(*n);
    return out;
}

// Reference: zlib's own prefix decode, then the complete lines of it.
std::size_t reference_complete_lines(const dstd::Vector<std::uint8_t>& comp) {
    z_stream zs{};
    REQUIRE(inflateInit2(&zs, 16 + MAX_WBITS) == Z_OK);
    dstd::Vector<unsigned char> out(1 << 22);
    zs.next_in = const_cast<Bytef*>(comp.data());
    zs.avail_in = static_cast<uInt>(comp.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    inflate(&zs, Z_SYNC_FLUSH);
    const std::size_t produced = out.size() - zs.avail_out;
    inflateEnd(&zs);
    return static_cast<std::size_t>(
        std::count(out.begin(), out.begin() + produced, '\n'));
}

dstd::String lines(int n) {
    dstd::String s;
    for (int i = 0; i < n; ++i)
        s +=
            dstd::fmt<"{\"id\":{},\"name\":\"read\",\"ph\":\"X\",\"ts\":{}}\n">(
                i, i * 7);
    return s;
}

}  // namespace

TEST_CASE("a complete member keeps all output") {
    const auto text = lines(500);
    const auto comp = gzip(text);
    dstd::Vector<std::uint8_t> out;
    auto keep = fileio::compress::decode_truncated_member(comp.data(),
                                                          comp.size(), out);
    REQUIRE(keep.has_value());
    CHECK(*keep == text.size());
    CHECK(dstd::String(out.begin(), out.begin() + *keep) == text);
}

TEST_CASE("a cut member keeps the complete lines of the prefix") {
    const auto text = lines(20000);
    const auto comp = gzip(text);
    for (double frac : {0.05, 0.5, 0.9, 0.99}) {
        CAPTURE(frac);
        dstd::Vector<std::uint8_t> cut(
            comp.begin(),
            comp.begin() + static_cast<std::ptrdiff_t>(
                               static_cast<double>(comp.size()) * frac));
        dstd::Vector<std::uint8_t> out;
        auto keep = fileio::compress::decode_truncated_member(cut.data(),
                                                              cut.size(), out);
        REQUIRE(keep.has_value());
        const auto kept_lines = static_cast<std::size_t>(
            std::count(out.begin(), out.begin() + *keep, '\n'));
        CHECK(kept_lines == reference_complete_lines(cut));
        CHECK(kept_lines > 0);
        CHECK(out[*keep - 1] == '\n');
        CHECK(dstd::String(out.begin(), out.begin() + *keep) ==
              text.substr(0, *keep));
    }
}

TEST_CASE("a cut inside the first line keeps nothing") {
    const dstd::String text(200000, 'x');
    const auto comp = gzip(dstd::fmt<"{}\n">(text));
    dstd::Vector<std::uint8_t> cut(comp.begin(), comp.begin() + 40);
    dstd::Vector<std::uint8_t> out;
    auto keep =
        fileio::compress::decode_truncated_member(cut.data(), cut.size(), out);
    REQUIRE(keep.has_value());
    CHECK(*keep == 0);
}

TEST_CASE("invalid deflate data is not recovered") {
    auto comp = gzip(lines(100));
    std::fill(comp.begin() + 10, comp.end(), 0xFF);  // reserved block type
    dstd::Vector<std::uint8_t> out;
    CHECK_FALSE(
        fileio::compress::decode_truncated_member(comp.data(), comp.size(), out)
            .has_value());
}
