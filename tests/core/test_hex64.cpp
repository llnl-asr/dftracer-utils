#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/hash/hex64.h>
#include <doctest/doctest.h>

#include <string>

using dftracer::utils::hash::format_hex64;
using dftracer::utils::hash::parse_hex64;

TEST_SUITE("hex64") {
    TEST_CASE("hex64 - round trips every value") {
        for (std::uint64_t v :
             {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0xdef456},
              std::uint64_t{~0ull}, std::uint64_t{0xf07c4ebf132e3799ull}}) {
            auto text = format_hex64(v);
            CHECK(text.size() == 16);
            auto back = parse_hex64(text);
            REQUIRE(back.has_value());
            CHECK(*back == v);
        }
    }

    TEST_CASE("hex64 - matches what dftracer writes") {
        CHECK(*parse_hex64("f07c4ebf132e3799") == 0xf07c4ebf132e3799ull);
        CHECK(format_hex64(0xf07c4ebf132e3799ull) == "f07c4ebf132e3799");
        CHECK(format_hex64(0) == "0000000000000000");
    }

    TEST_CASE("hex64 - rejects anything without a round trip") {
        // Uppercase and short forms parse to the same value as their canonical
        // spelling, so accepting them would render back as a different string
        // and miss the hash table lookup.
        CHECK_FALSE(parse_hex64("F07C4EBF132E3799").has_value());
        CHECK_FALSE(parse_hex64("def456").has_value());
        CHECK_FALSE(parse_hex64("").has_value());
        CHECK_FALSE(parse_hex64("f07c4ebf132e379").has_value());
        CHECK_FALSE(parse_hex64("f07c4ebf132e37990").has_value());
        CHECK_FALSE(parse_hex64("/some/path/file.h").has_value());
        CHECK_FALSE(parse_hex64("g07c4ebf132e3799").has_value());
    }
}
