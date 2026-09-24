#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace gz = dftracer::utils::utilities::fileio::compress;

namespace {

std::vector<std::uint8_t> gzip(const std::string& text) {
    gz::GzipMemberCompressor c;
    std::vector<std::uint8_t> out(c.bound(text.size()));
    auto n = c.compress(text.data(), text.size(), out.data(), out.size());
    REQUIRE(n.has_value());
    out.resize(*n);
    return out;
}

// Reference: zlib's own prefix decode, then the complete lines of it.
std::size_t reference_complete_lines(const std::vector<std::uint8_t>& comp) {
    z_stream zs{};
    REQUIRE(inflateInit2(&zs, 16 + MAX_WBITS) == Z_OK);
    std::vector<unsigned char> out(1 << 22);
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

std::string lines(int n) {
    std::string s;
    for (int i = 0; i < n; ++i)
        s += "{\"id\":" + std::to_string(i) +
             ",\"name\":\"read\",\"ph\":\"X\",\"ts\":" + std::to_string(i * 7) +
             "}\n";
    return s;
}

}  // namespace

TEST_CASE("a complete member keeps all output") {
    const auto text = lines(500);
    const auto comp = gzip(text);
    std::vector<std::uint8_t> out;
    auto keep = gz::decode_truncated_member(comp.data(), comp.size(), out);
    REQUIRE(keep.has_value());
    CHECK(*keep == text.size());
    CHECK(std::string(out.begin(), out.begin() + *keep) == text);
}

TEST_CASE("a cut member keeps the complete lines of the prefix") {
    const auto text = lines(20000);
    const auto comp = gzip(text);
    for (double frac : {0.05, 0.5, 0.9, 0.99}) {
        CAPTURE(frac);
        std::vector<std::uint8_t> cut(
            comp.begin(),
            comp.begin() + static_cast<std::ptrdiff_t>(
                               static_cast<double>(comp.size()) * frac));
        std::vector<std::uint8_t> out;
        auto keep = gz::decode_truncated_member(cut.data(), cut.size(), out);
        REQUIRE(keep.has_value());
        const auto kept_lines = static_cast<std::size_t>(
            std::count(out.begin(), out.begin() + *keep, '\n'));
        CHECK(kept_lines == reference_complete_lines(cut));
        CHECK(kept_lines > 0);
        CHECK(out[*keep - 1] == '\n');
        CHECK(std::string(out.begin(), out.begin() + *keep) ==
              text.substr(0, *keep));
    }
}

TEST_CASE("a cut inside the first line keeps nothing") {
    const std::string text(200000, 'x');
    const auto comp = gzip(text + "\n");
    std::vector<std::uint8_t> cut(comp.begin(), comp.begin() + 40);
    std::vector<std::uint8_t> out;
    auto keep = gz::decode_truncated_member(cut.data(), cut.size(), out);
    REQUIRE(keep.has_value());
    CHECK(*keep == 0);
}

TEST_CASE("invalid deflate data is not recovered") {
    auto comp = gzip(lines(100));
    std::fill(comp.begin() + 10, comp.end(), 0xFF);  // reserved block type
    std::vector<std::uint8_t> out;
    CHECK_FALSE(
        gz::decode_truncated_member(comp.data(), comp.size(), out).has_value());
}
