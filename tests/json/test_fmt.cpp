// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/json/fmt.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

TEST_CASE("json::fmt escapes quotes, backslash and control characters") {
    CHECK(json::fmt<R"("{}")">(dstd::StringView("a\"b")) == R"("a\"b")");
    CHECK(json::fmt<"{}">(dstd::StringView("a\\b")) == "a\\\\b");
    CHECK(json::fmt<"{}">(dstd::StringView("l1\nl2\tx\r")) == "l1\\nl2\\tx\\r");
    CHECK(json::fmt<"{}">(dstd::StringView("\x01\x1f")) == "\\u0001\\u001f");
    CHECK(json::fmt<"{}">(dstd::StringView("\b\f")) == "\\b\\f");
}

TEST_CASE("json::fmt passes UTF-8 bytes through") {
    const char* s = "caf\xc3\xa9 \xe2\x82\xac";
    CHECK(json::fmt<"{}">(s) == s);
}

TEST_CASE("json::fmt leaves numbers, bools, chars and RawText alone") {
    const dstd::fmt_detail::RawText raw{"a\"b\n"};
    CHECK(json::fmt<R"({"n":{},"b":{},"c":{},"r":{},"f":{:.2f}})">(
              std::int64_t{-7}, true, '"', raw, 1.5) ==
          "{\"n\":-7,\"b\":true,\"c\":\",\"r\":a\"b\n,\"f\":1.50}");
}

TEST_CASE("json::fmt_to fits and truncates like dstd::fmt_to") {
    char buf[64];
    auto fit = json::fmt_to<R"({"k":"{}"})">(buf, sizeof(buf),
                                             dstd::StringView("a\"b"));
    CHECK_FALSE(fit.truncated);
    CHECK(dstd::StringView(buf, fit.size) == R"({"k":"a\"b"})");

    char small[8];
    auto cut = json::fmt_to<R"({"k":"{}"})">(small, sizeof(small),
                                             dstd::StringView("a\"bcdef"));
    CHECK(cut.truncated);
    CHECK(cut.size == sizeof(small));
    CHECK(dstd::StringView(small, cut.size) == "{\"k\":\"a\\");

    auto plain = dstd::fmt_to<"{}">(small, sizeof(small),
                                    dstd::StringView("0123456789"));
    CHECK(plain.truncated);
    CHECK(plain.size == sizeof(small));
}

TEST_CASE("json::fmt_to truncates a number hole like dstd::fmt_to") {
    char small[3];
    auto cut = json::fmt_to<"x{}">(small, sizeof(small), std::uint64_t{12345});
    CHECK(cut.truncated);
    CHECK(dstd::StringView(small, cut.size) == "x12");
}

TEST_CASE("escaped_json_size is the exact size of the escaped text") {
    for (const char* text :
         {"", "plain", "a\"b\\c\n", "\x01\x02\x1f", "caf\xc3\xa9\t"}) {
        const dstd::String s(text);
        CHECK(json::escaped_json_size(s) == json::escape_json_string(s).size());
    }
    dstd::String mixed;
    for (int i = 0; i < 300; ++i) mixed += static_cast<char>(i % 128);
    CHECK(json::escaped_json_size(mixed) ==
          json::escape_json_string(mixed).size());
}

TEST_CASE("a long string reserves its exact escaped size") {
    const dstd::String clean(100000, 'a');
    CHECK(json::JsonEscape::bound(clean) == clean.size());
    dstd::String dirty;
    for (int i = 0; i < 1000; ++i) dirty += "ab\"cd\x01 ef\n";
    const std::size_t exact = json::escape_json_string(dirty).size();
    CHECK(json::JsonEscape::bound(dirty) == exact);
    CHECK(exact == dirty.size() + 1000 * (1 + 5 + 1));
    CHECK(json::fmt<"[{}]">(dirty).size() == exact + 2);
    CHECK(json::JsonEscape::bound(dstd::StringView("\x01\x01")) == 12);
    const dstd::String edge(json::JsonEscape::LONG_STRING - 1, '\x01');
    CHECK(json::JsonEscape::bound(edge) == 6 * edge.size());
    const dstd::String at(json::JsonEscape::LONG_STRING, '\x01');
    CHECK(json::JsonEscape::bound(at) == 6 * at.size());
    const dstd::String at_n(json::JsonEscape::LONG_STRING, '\n');
    CHECK(json::JsonEscape::bound(at_n) == 2 * at_n.size());
}

TEST_CASE("the escaped text of a long string with runs is correct") {
    dstd::String s(200, 'x');
    s[0] = '"';
    s[100] = '\\';
    s[199] = '\n';
    const dstd::String out = json::fmt<"{}">(s);
    CHECK(out.size() == 203);
    CHECK(out.substr(0, 3) == "\\\"x");
    CHECK(out.substr(out.size() - 3) == "x\\n");
}
