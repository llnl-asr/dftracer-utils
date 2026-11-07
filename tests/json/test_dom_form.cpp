// SPDX-License-Identifier: MIT
// A line the DOM parser refuses, written as a line it accepts.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/json/dom_form.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <cstdint>
#include <limits>

using namespace dftracer::utils;

namespace {

constexpr double LARGEST = std::numeric_limits<double>::max();

struct Dom {
    simdjson::dom::parser parser;
    simdjson::dom::element root;

    void read(dstd::StringView text) {
        REQUIRE(parser.parse(text.data(), text.size()).get(root) ==
                simdjson::SUCCESS);
    }
};

dstd::String form(dstd::StringView line) {
    dstd::String out;
    REQUIRE(json::dom_form(line, out));
    return out;
}

}  // namespace

TEST_CASE("the lines the DOM parser refuses are refused to begin with") {
    simdjson::dom::parser parser;
    for (const char* line :
         {R"({"d":1e400})", R"({"s":"a\qb"})", R"({"i":123456789012345678901})",
          "{\"s\":\"a\xff\"}", "{\"s\":\"a\tb\"}"}) {
        dstd::String text(line);
        CHECK(parser.parse(text.data(), text.size()).error() !=
              simdjson::SUCCESS);
    }
}

TEST_CASE("a number too large for a double is the largest of its sign") {
    Dom dom;
    dom.read(form(R"({"ph":"X","dur":1e400,"low":-1e400,"ok":2.5})"));
    CHECK(dom.root["ph"].get_string().value() == "X");
    CHECK(dom.root["dur"].get_double().value() == LARGEST);
    CHECK(dom.root["low"].get_double().value() == -LARGEST);
    CHECK(dom.root["ok"].get_double().value() == 2.5);
}

TEST_CASE("an integer wider than 64 bits is a double and a fitting one stays") {
    Dom dom;
    dom.read(form(
        R"({"wide":123456789012345678901234,"top":18446744073709551615,"neg":-5})"));
    CHECK(dom.root["wide"].get_double().value() ==
          doctest::Approx(1.2345e23).epsilon(1e-3));
    CHECK(dom.root["top"].get_uint64().value() == 18446744073709551615ULL);
    CHECK(dom.root["neg"].get_int64().value() == -5);
}

TEST_CASE("a string with an invalid escape is its raw text") {
    Dom dom;
    dom.read(form(R"({"name":"a\qb","key\q":"v","good":"x\ny"})"));
    CHECK(dom.root["name"].get_string().value() == "a\\qb");
    CHECK(dom.root["key\\q"].get_string().value() == "v");
    CHECK(dom.root["good"].get_string().value() == "x\ny");
}

TEST_CASE("a bad byte in a string is a question mark") {
    Dom dom;
    dom.read(form("{\"a\":\"x\xff\xfey\",\"b\":\"p\tq\"}"));
    CHECK(dom.root["a"].get_string().value() == "x??y");
    CHECK(dom.root["b"].get_string().value() == "p?q");
}

TEST_CASE("a part nested past 128 levels is null and the rest stays") {
    dstd::String line = "{\"a\":";
    for (int i = 0; i < 1100; ++i) line += '[';
    for (int i = 0; i < 1100; ++i) line += ']';
    line += ",\"b\":7}";
    Dom dom;
    dom.read(form(line));
    CHECK(dom.root["b"].get_int64().value() == 7);
    CHECK(dom.root["a"].is_array());
}

TEST_CASE("members keep their order and a repeated key stays repeated") {
    CHECK(
        form(R"({"a":1,"b":[true,null,"s",{"c":-2}],"a":3,"x":1e400})") ==
        R"({"a":1,"b":[true,null,"s",{"c":-2}],"a":3,"x":1.7976931348623157e308})");
}

TEST_CASE("a line that cannot be read in full gives false and no text") {
    dstd::String out = "stale";
    CHECK_FALSE(json::dom_form(R"({"a":1,"b":)", out));
    CHECK(out.empty());
    CHECK_FALSE(json::dom_form(R"({"a":tru,"b":1})", out));
    CHECK(out.empty());
    CHECK_FALSE(json::dom_form("", out));
}

TEST_CASE("the tail of a cut line is not a line") {
    dstd::String out;
    for (const char* tail : {R"(x":1})", R"("a":1})", "}}", R"(1,"b":2})"})
        CHECK_FALSE(json::dom_form(tail, out));
}

TEST_CASE(
    "a value nested 100000 deep is kept below level 128 as its raw text") {
    constexpr int DEPTH = 100000;
    constexpr int KEPT = 127;
    dstd::String line = "{\"a\":";
    line.append(DEPTH, '[');
    line.append(DEPTH, ']');
    line += ",\"b\":7}";
    Dom dom;
    dom.read(form(line));
    CHECK(dom.root["b"].get_int64().value() == 7);
    simdjson::dom::element e = dom.root["a"].value();
    for (int i = 0; i < KEPT; ++i) {
        REQUIRE(e.is_array());
        e = e.get_array().at(0).value();
    }
    REQUIRE(e.is_string());
    dstd::String want;
    want.append(DEPTH - KEPT, '[');
    want.append(DEPTH - KEPT, ']');
    CHECK(dstd::String(e.get_string().value()) == want);
}

TEST_CASE("an integer of 20 to 22 digits is a number the DOM accepts") {
    for (const char* digits :
         {"12345678901234567890", "123456789012345678901",
          "1234567890123456789012", "18446744073709551616"}) {
        for (const char* sign : {"", "-"}) {
            dstd::String line = R"({"a":)";
            line += sign;
            line += digits;
            line += R"(,"b":1})";
            dstd::String out;
            REQUIRE(json::dom_form(line, out));
            simdjson::dom::parser parser;
            auto doc = parser.parse(out.data(), out.size());
            REQUIRE(doc.error() == simdjson::SUCCESS);
            CHECK(doc.value()["b"].get_int64().value() == 1);
            const double a = doc.value()["a"].get_double().value();
            CHECK(std::abs(a) > 1.2e19);
            CHECK((a < 0) == (sign[0] == '-'));
        }
    }
}

TEST_CASE("text after the first record is not read in full") {
    dstd::String out;
    CHECK_FALSE(json::dom_form(R"({"a":1e400} {"b":2})", out));
    CHECK(out.empty());
    CHECK_FALSE(json::dom_form(R"({"a":1e400,"b":1}})", out));
    CHECK(json::dom_form("{\"a\":1e400}  \r\n", out));
}
