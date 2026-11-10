// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/path.h>
#include <doctest/doctest.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

using namespace dftracer::utils;

namespace {

template <dstd::FixedString S, dstd::FmtArg... A>
dstd::Unexpected<dstd::Error> fail_t(const A&... a) {
    return common::make_error<S>(common::ErrorCode::Parse, a...);
}

template <dstd::FixedString S, dstd::FmtArg... A>
dstd::Unexpected<dstd::Error> fail_t(dstd::Format<S> f, const A&... a) {
    return common::make_error(common::ErrorCode::Parse, f, a...);
}

template <dstd::FixedString S, class... A>
void check_text(dstd::StringView want, const A&... a) {
    CHECK(dstd::fmt<S>(a...) == want);
    for (std::size_t cap = 0; cap <= want.size() + 2; ++cap) {
        char buf[1024];
        std::memset(buf, 0xBB, sizeof(buf));
        const auto r = dstd::fmt_to<S>(buf, cap, a...);
        for (std::size_t i = cap; i < sizeof(buf); ++i)
            REQUIRE(static_cast<unsigned char>(buf[i]) == 0xBB);
        const std::size_t kept = cap < want.size() ? cap : want.size();
        REQUIRE(r.size == kept);
        REQUIRE(r.truncated == (cap < want.size()));
        REQUIRE(dstd::StringView(buf, r.size) == want.substr(0, kept));
    }
}

}  // namespace

static_assert(dstd::FMT_HOLES<"a {} b {:x} {{}}"> == 2);
static_assert(dstd::FmtArg<int> && dstd::FmtArg<dstd::String> &&
              dstd::FmtArg<const char (&)[4]> && dstd::FmtArg<char>);
static_assert(!dstd::FmtArg<void*> && !dstd::FmtArg<std::nullptr_t>);

namespace inner::deep {
static dstd::String found_unqualified() { return dstd::fmt("v={}"_f, 7); }
}  // namespace inner::deep

TEST_CASE("every hole type") {
    CHECK(dstd::fmt<"{}">(std::int8_t{-5}) == "-5");
    CHECK(dstd::fmt<"{}">(std::uint8_t{250}) == "250");
    CHECK(dstd::fmt<"{}">(std::int16_t{-30000}) == "-30000");
    CHECK(dstd::fmt<"{}">(std::uint16_t{65535}) == "65535");
    CHECK(dstd::fmt<"{}">(std::int32_t{-2147483647 - 1}) == "-2147483648");
    CHECK(dstd::fmt<"{}">(std::uint32_t{4294967295u}) == "4294967295");
    CHECK(dstd::fmt<"{}">(std::numeric_limits<std::int64_t>::min()) ==
          "-9223372036854775808");
    CHECK(dstd::fmt<"{}">(std::numeric_limits<std::uint64_t>::max()) ==
          "18446744073709551615");
    CHECK(dstd::fmt<"{}">(true) == "true");
    CHECK(dstd::fmt<"{}">(false) == "false");
    CHECK(dstd::fmt<"{}">('z') == "z");
    CHECK(dstd::fmt<"{}">(2.5) == "2.5");
    CHECK(dstd::fmt<"{}">(0.1f) == "0.1");
    CHECK(dstd::fmt<"{}">(-1e300) == "-1e+300");
    CHECK(dstd::fmt<"{}">(dstd::StringView("sv")) == "sv");
    CHECK(dstd::fmt<"{}">(dstd::String("owned")) == "owned");
    CHECK(dstd::fmt<"{}">(std::string("std string")) == "std string");
    CHECK(dstd::fmt<"{}">("literal") == "literal");
    const char* p = "ptr";
    CHECK(dstd::fmt<"{}">(p) == "ptr");
    const char* none = nullptr;
    CHECK(dstd::fmt<"{}">(none) == "(null)");
    CHECK(dstd::fmt<"{}">(dstd::Path("/a/b")) == "/a/b");
    CHECK(dstd::fmt<"{}">(dstd::PathView("/c")) == "/c");
}

TEST_CASE("every spec") {
    CHECK(dstd::fmt<"{:x}">(255) == "ff");
    CHECK(dstd::fmt<"{:X}">(255) == "FF");
    CHECK(dstd::fmt<"{:x}">(-1) == "ffffffff");
    CHECK(dstd::fmt<"{:x}">(std::int64_t{-1}) == "ffffffffffffffff");
    CHECK(dstd::fmt<"{:x}">(0) == "0");
    CHECK(dstd::fmt<"{:08}">(42) == "00000042");
    CHECK(dstd::fmt<"{:08}">(-42) == "-0000042");
    CHECK(dstd::fmt<"{:02}">(12345) == "12345");
    CHECK(dstd::fmt<"{:016x}">(std::uint64_t{0xdeadbeef}) ==
          "00000000deadbeef");
    CHECK(dstd::fmt<"{:08X}">(std::uint32_t{0xabc}) == "00000ABC");
    CHECK(dstd::fmt<"{:.3f}">(3.14159) == "3.142");
    CHECK(dstd::fmt<"{:.0f}">(2.5) == "2");
    CHECK(dstd::fmt<"{:.2f}">(0.125) == "0.12");
    CHECK(dstd::fmt<"{:f}">(1.5) == "1.500000");
    CHECK(dstd::fmt<"{:.3f}">(-0.0004) == "-0.000");
    CHECK(dstd::fmt<"{:08.3f}">(-3.14159) == "-003.142");
    CHECK(dstd::fmt<"{:.1f}">(1e20) == "100000000000000000000.0");
    CHECK(dstd::fmt<"{:.2f}">(1e300).size() == 304);
    CHECK(dstd::fmt<"{:.2f}">(1.5f) == "1.50");
}

TEST_CASE("literal braces, empty format and many arguments") {
    CHECK(dstd::fmt<"">().empty());
    CHECK(dstd::fmt<"plain">() == "plain");
    CHECK(dstd::fmt<R"({"a":{},"b":"{}"})">(1, "x") == R"({"a":1,"b":"x"})");
    CHECK(dstd::fmt<"{{}}">() == "{}");
    CHECK(dstd::fmt<"{{}} {}">(3) == "{} 3");
    CHECK(dstd::fmt<"{ } {x} }{">() == "{ } {x} }{");
    CHECK(dstd::fmt<"{}{}{}{}{}{}{}{}{}{}{}{}">(1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                                11, 12) == "123456789101112");
    CHECK(dstd::fmt<"{}-{}">("a", "b") == "a-b");
    CHECK(dstd::fmt<"{}{}">("", "") == "");
}

TEST_CASE("long strings") {
    const dstd::String big(10000, 'q');
    const dstd::String out = dstd::fmt<"<{}|{}>">(big, 7);
    CHECK(out.size() == 10000 + 4);
    CHECK(out.front() == '<');
    CHECK(out.substr(out.size() - 3) == "|7>");
}

TEST_CASE("both call forms give the same text") {
    CHECK(dstd::fmt<"open {}: {} {:04x}">("a", 3, 255) ==
          dstd::fmt("open {}: {} {:04x}"_f, "a", 3, 255));
    CHECK(dstd::fmt("none"_f) == "none");
    CHECK(inner::deep::found_unqualified() == "v=7");
}

TEST_CASE("fmt_to on an exact, a short and a zero buffer") {
    char buf[64];
    auto r = dstd::fmt_to<"ab{}cd">(buf, 5, 7);
    CHECK(!r.truncated);
    CHECK(dstd::StringView(buf, r.size) == "ab7cd");

    char tight[5];
    r = dstd::fmt_to<"ab{}cd">(tight, 5, 7);
    CHECK(!r.truncated);
    CHECK(dstd::StringView(tight, r.size) == "ab7cd");

    char shorter[4];
    r = dstd::fmt_to<"ab{}cd">(shorter, 4, 7);
    CHECK(r.truncated);
    CHECK(r.size == 4);
    CHECK(dstd::StringView(shorter, r.size) == "ab7c");

    r = dstd::fmt_to<"ab{}cd">(buf, 2, 7);
    CHECK(r.truncated);
    CHECK(dstd::StringView(buf, r.size) == "ab");

    r = dstd::fmt_to<"ab{}cd">(buf, 0, 7);
    CHECK(r.truncated);
    CHECK(r.size == 0);

    r = dstd::fmt_to<"{}">(buf, 3, dstd::String("abcdef"));
    CHECK(r.truncated);
    CHECK(dstd::StringView(buf, r.size) == "abc");

    r = dstd::fmt_to<"{:.2f}|">(buf, 6, 1e300);
    CHECK(r.truncated);
    CHECK(r.size == 6);

    r = dstd::fmt_to<"">(buf, 0);
    CHECK(!r.truncated);
    CHECK(r.size == 0);

    r = dstd::fmt_to(buf, sizeof(buf), "{}!"_f, 9);
    CHECK(dstd::StringView(buf, r.size) == "9!");
}

TEST_CASE("make_error<S> carries the code and the formatted message") {
    const auto e = common::make_error<"open {}: {} ({:04x})">(
        common::ErrorCode::Io, dstd::String("a.pfw"), "denied", 255);
    CHECK(e.error().is_code(common::ErrorCode::Io));
    CHECK(e.error().message() == "open a.pfw: denied (00ff)");
    CHECK(dstd::StringView(e.error().c_str()) == e.error().message());
    const auto copy = e.error();
    CHECK(copy.message() == e.error().message());

    const auto v = common::make_error(common::ErrorCode::NotFound,
                                      "no {} in {}"_f, "key", 3);
    CHECK(v.error().is_code(common::ErrorCode::NotFound));
    CHECK(v.error().message() == "no key in 3");

    const auto none = common::make_error<"no holes">(common::ErrorCode::Parse);
    CHECK(none.error().message() == "no holes");
    const auto empty = common::make_error<"">(common::ErrorCode::Parse);
    CHECK(empty.error().message().empty());

    const dstd::String big(5000, 'z');
    const auto large = common::make_error<"[{}]">(common::ErrorCode::Io, big);
    CHECK(large.error().message().size() == 5002);
}

TEST_CASE("helpers forward a format in both shapes") {
    const auto a = fail_t<"{} is bad">("x");
    CHECK(a.error().message() == "x is bad");
    CHECK(a.error().is_code(common::ErrorCode::Parse));
    const auto b = fail_t("{} is worse"_f, 12);
    CHECK(b.error().message() == "12 is worse");

    const auto fail = [&](auto f, const auto&... args) {
        return common::make_error(common::ErrorCode::Duql, f, args...);
    };
    const auto c =
        fail("'{}' sums a column that is not a typed number"_f, "dur");
    CHECK(c.error().message() ==
          "'dur' sums a column that is not a typed number");
    CHECK(c.error().is_code(common::ErrorCode::Duql));
    CHECK(fail("plain"_f).error().message() == "plain");
}

TEST_CASE("bound edge cases keep inside the buffer") {
    const double nan = std::nan("");
    const double inf = HUGE_VAL;
    check_text<"{}">("nan", nan);
    check_text<"{}">("inf", inf);
    check_text<"{}">("-inf", -inf);
    check_text<"{}">("-0", -0.0);
    check_text<"{:.3f}">("nan", nan);
    check_text<"{:.3f}">("inf", inf);
    check_text<"{:.3f}">("-inf", -inf);
    check_text<"{:010.3f}">("-00000.000", -0.0);
    check_text<"{:021}">("-09223372036854775808",
                         std::numeric_limits<std::int64_t>::min());
    check_text<"{:x}">("8000000000000000",
                       std::numeric_limits<std::int64_t>::min());
    check_text<"{:x}">("ffffffffffffffff",
                       std::numeric_limits<std::uint64_t>::max());
    check_text<"{:X}">("FFFFFFFFFFFFFFFF",
                       std::numeric_limits<std::uint64_t>::max());
    check_text<"{}">("-9223372036854775808",
                     std::numeric_limits<std::int64_t>::min());
    check_text<"{:x}">("ff", std::int8_t{-1});
    check_text<"{:01}">("-128", std::int8_t{-128});

    const dstd::String wide = dstd::fmt<"{:0255}">(5);
    CHECK(wide.size() == 255);
    CHECK(wide.back() == '5');
    check_text<"{:0255}">(wide, 5);

    for (const double v : {DBL_MAX, -DBL_MAX}) {
        const dstd::String t = dstd::fmt<"{:.99f}">(v);
        CHECK(t.size() == 309 + (v < 0) + 1 + 99);
        check_text<"{:.99f}">(t, v);
    }
    const dstd::String tiny = dstd::fmt<"{:.99f}">(4.9406564584124654e-324);
    CHECK(tiny.size() == 101);
    check_text<"{:.99f}">(tiny, 4.9406564584124654e-324);
    check_text<"{:.99f}">(dstd::fmt<"{:.99f}">(-4.9406564584124654e-324),
                          -4.9406564584124654e-324);
    check_text<"{}">("1.1754944e-38", FLT_MIN);
    check_text<"{:.45f}">(dstd::fmt<"{:.45f}">(FLT_MIN), FLT_MIN);
}

TEST_CASE("fmt_to keeps its canaries for every cap") {
    check_text<"ab{}cd">("ab7cd", 7);
    check_text<"{}|{:x}|{}">("hello|ff|true", "hello", 255, true);
    check_text<"">("");
    check_text<"{}">("(null)", static_cast<const char*>(nullptr));
    check_text<"{} {:.2f}">("q 1.50", dstd::String("q"), 1.5);
}
