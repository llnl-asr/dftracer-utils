#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/extent_codec.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <random>

namespace {

namespace dstd = dftracer::utils::dstd;
namespace store = dftracer::utils::store;

using dftracer::utils::ByteView;

constexpr std::size_t ALIGN = 4096;

ByteView view_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

/// Repeated event-like text, which zstd shrinks well.
dstd::Vector<std::uint8_t> text_bytes(std::size_t length) {
    static constexpr char LINE[] =
        "{\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":7,\"dur\":12}\n";
    dstd::Vector<std::uint8_t> bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
        bytes[i] = static_cast<std::uint8_t>(LINE[i % (sizeof(LINE) - 1)]);
    }
    return bytes;
}

/// Uniform random bytes, which zstd cannot shrink.
dstd::Vector<std::uint8_t> random_bytes(std::size_t length) {
    std::mt19937_64 rng(42);
    dstd::Vector<std::uint8_t> bytes(length);
    for (std::uint8_t& byte : bytes) byte = static_cast<std::uint8_t>(rng());
    return bytes;
}

store::ExtentCodec codec_of(const dstd::Vector<std::uint8_t>& stored) {
    return static_cast<store::ExtentCodec>(stored[0]);
}

}  // namespace

TEST_SUITE("store extent codec") {
    TEST_CASE("an extent that compresses is stored as zstd and decodes back") {
        const dstd::Vector<std::uint8_t> raw = text_bytes(60000);
        auto stored = store::encode_extent(view_of(raw), ALIGN);
        REQUIRE(stored.has_value());
        CHECK(codec_of(*stored) == store::ExtentCodec::Zstd);
        CHECK(stored->size() % ALIGN == 0);
        CHECK(stored->size() < raw.size() / 4);
        CHECK(dftracer::utils::read_u32_le(stored->data() + 4) == raw.size());

        auto decoded = store::decode_extent(*stored);
        REQUIRE(decoded.has_value());
        CHECK(*stored == raw);
    }

    TEST_CASE("an extent that does not compress is stored raw") {
        const dstd::Vector<std::uint8_t> raw = random_bytes(10000);
        auto stored = store::encode_extent(view_of(raw), ALIGN);
        REQUIRE(stored.has_value());
        CHECK(codec_of(*stored) == store::ExtentCodec::None);
        CHECK(stored->size() ==
              (store::EXTENT_HEADER_BYTES + raw.size() + ALIGN - 1) / ALIGN *
                  ALIGN);

        auto decoded = store::decode_extent(*stored);
        REQUIRE(decoded.has_value());
        CHECK(*stored == raw);
    }

    TEST_CASE("an extent with no alignment keeps no padding") {
        const dstd::Vector<std::uint8_t> raw = random_bytes(100);
        auto stored = store::encode_extent(view_of(raw), 1);
        REQUIRE(stored.has_value());
        CHECK(stored->size() == store::EXTENT_HEADER_BYTES + raw.size());
    }

    TEST_CASE("an empty extent is refused") {
        const dstd::Vector<std::uint8_t> raw;
        CHECK_FALSE(store::encode_extent(view_of(raw), ALIGN).has_value());
    }

    TEST_CASE("a stored extent that does not decode is a Parse error") {
        const dstd::Vector<std::uint8_t> raw = text_bytes(20000);
        auto stored = store::encode_extent(view_of(raw), ALIGN);
        REQUIRE(stored.has_value());
        REQUIRE(codec_of(*stored) == store::ExtentCodec::Zstd);

        SUBCASE("unknown codec") {
            dstd::Vector<std::uint8_t> bad = *stored;
            bad[0] = 7;
            auto decoded = store::decode_extent(bad);
            REQUIRE_FALSE(decoded.has_value());
            CHECK(decoded.error().is(dstd::Condition::Parse));
        }
        SUBCASE("a reserved byte set") {
            dstd::Vector<std::uint8_t> bad = *stored;
            bad[13] = 1;
            CHECK_FALSE(store::decode_extent(bad).has_value());
        }
        SUBCASE("a payload longer than the extent") {
            dstd::Vector<std::uint8_t> bad = *stored;
            dftracer::utils::write_u32_le(
                bad.data() + 8, static_cast<std::uint32_t>(bad.size()));
            CHECK_FALSE(store::decode_extent(bad).has_value());
        }
        SUBCASE("a raw length the payload does not give") {
            dstd::Vector<std::uint8_t> bad = *stored;
            dftracer::utils::write_u32_le(
                bad.data() + 4, static_cast<std::uint32_t>(raw.size() + 1));
            CHECK_FALSE(store::decode_extent(bad).has_value());
        }
        SUBCASE("a raw length past the bound") {
            dstd::Vector<std::uint8_t> bad = *stored;
            dftracer::utils::write_u32_le(bad.data() + 4,
                                          store::EXTENT_MAX_RAW_BYTES + 1U);
            CHECK_FALSE(store::decode_extent(bad).has_value());
        }
        SUBCASE("a damaged payload") {
            dstd::Vector<std::uint8_t> bad = *stored;
            for (std::size_t i = store::EXTENT_HEADER_BYTES;
                 i < store::EXTENT_HEADER_BYTES + 64; ++i) {
                bad[i] ^= 0xA5U;
            }
            CHECK_FALSE(store::decode_extent(bad).has_value());
        }
        SUBCASE("fewer bytes than a header") {
            dstd::Vector<std::uint8_t> bad(store::EXTENT_HEADER_BYTES - 1, 0);
            CHECK_FALSE(store::decode_extent(bad).has_value());
        }
    }
}
