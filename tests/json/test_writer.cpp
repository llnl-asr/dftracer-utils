// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/writer.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

TEST_CASE("json::Writer writes compact nested JSON") {
    dstd::String out;
    {
        json::Writer w(out);
        w.begin_object();
        w.member("name", "demo");
        w.key("sizes");
        w.begin_array();
        w.value(std::int64_t{1});
        w.value(2.0);
        w.begin_object();
        w.end_object();
        w.end_array();
        w.member("ok", true);
        w.key("none");
        w.null();
        w.end_object();
    }
    CHECK(out == R"({"name":"demo","sizes":[1,2.0,{}],"ok":true,"none":null})");
}

TEST_CASE("json::Writer escapes strings and keys and reads back") {
    dstd::String out;
    {
        json::Writer w(out);
        w.begin_object();
        w.member("k\"ey", "line\nbreak \\ \x01");
        w.end_object();
    }
    json::Document doc;
    auto root = doc.parse(out);
    REQUIRE(root.has_value());
    auto member = root->find("k\\\"ey");
    REQUIRE(member.has_value());
    CHECK(member->as_string().value() == "line\nbreak \\ \x01");
}

TEST_CASE("json::Writer writes a double so that it reads back as a double") {
    dstd::String out;
    {
        json::Writer w(out);
        w.begin_array();
        w.value(1e20);
        w.value(-0.5);
        w.value(std::uint64_t{18446744073709551615ull});
        w.end_array();
    }
    CHECK(out == "[1.0e+20,-0.5,18446744073709551615]");
}
