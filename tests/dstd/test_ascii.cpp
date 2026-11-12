// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/ascii.h>
#include <doctest/doctest.h>

using namespace dftracer::utils;

namespace {

bool ref_digit(int b) { return b >= 0x30 && b <= 0x39; }
bool ref_lower(int b) { return b >= 0x61 && b <= 0x7a; }
bool ref_upper(int b) { return b >= 0x41 && b <= 0x5a; }
bool ref_xdigit(int b) {
    return ref_digit(b) || (b >= 0x61 && b <= 0x66) || (b >= 0x41 && b <= 0x46);
}
bool ref_space(int b) { return b == 0x20 || (b >= 0x09 && b <= 0x0d); }

}  // namespace

static_assert(dstd::is_ascii_digit('7'));
static_assert(!dstd::is_ascii_digit('a'));
static_assert(dstd::to_ascii_lower('Q') == 'q');
static_assert(dstd::to_ascii_upper('q') == 'Q');
static_assert(dstd::ascii_iequals("AbC", "aBc"));
static_assert(!dstd::ascii_iequals("ab", "abc"));

TEST_CASE("ascii classes over all bytes") {
    for (int b = 0; b < 256; ++b) {
        const char c = static_cast<char>(b);
        CAPTURE(b);
        CHECK(dstd::is_ascii_digit(c) == ref_digit(b));
        CHECK(dstd::is_ascii_lower(c) == ref_lower(b));
        CHECK(dstd::is_ascii_upper(c) == ref_upper(b));
        CHECK(dstd::is_ascii_alpha(c) == (ref_lower(b) || ref_upper(b)));
        CHECK(dstd::is_ascii_alnum(c) ==
              (ref_lower(b) || ref_upper(b) || ref_digit(b)));
        CHECK(dstd::is_ascii_xdigit(c) == ref_xdigit(b));
        CHECK(dstd::is_ascii_space(c) == ref_space(b));
        if (b >= 0x80) {
            CHECK_FALSE(dstd::is_ascii_digit(c));
            CHECK_FALSE(dstd::is_ascii_alpha(c));
            CHECK_FALSE(dstd::is_ascii_alnum(c));
            CHECK_FALSE(dstd::is_ascii_xdigit(c));
            CHECK_FALSE(dstd::is_ascii_space(c));
        }
    }
}

TEST_CASE("ascii case conversion over all bytes") {
    for (int b = 0; b < 256; ++b) {
        const char c = static_cast<char>(b);
        CAPTURE(b);
        const int lower = ref_upper(b) ? b + 32 : b;
        const int upper = ref_lower(b) ? b - 32 : b;
        CHECK(static_cast<unsigned char>(dstd::to_ascii_lower(c)) == lower);
        CHECK(static_cast<unsigned char>(dstd::to_ascii_upper(c)) == upper);
    }
}

TEST_CASE("ascii_iequals") {
    CHECK(dstd::ascii_iequals("", ""));
    CHECK_FALSE(dstd::ascii_iequals("", "a"));
    CHECK_FALSE(dstd::ascii_iequals("ab", "abc"));
    CHECK(dstd::ascii_iequals("Hello", "hELLO"));
    CHECK_FALSE(dstd::ascii_iequals("Hello", "Hellp"));
    CHECK_FALSE(dstd::ascii_iequals("a-", "a_"));
    CHECK(dstd::ascii_iequals("\xc3\xa9", "\xc3\xa9"));
    CHECK_FALSE(dstd::ascii_iequals("\xc3\x89", "\xc3\xa9"));
}
