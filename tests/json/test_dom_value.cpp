// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/dom_value.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

TEST_CASE("DomDocument reads a nested document in full") {
    auto doc = json::DomDocument::parse(
        R"({"a":{"b":[1,-2,3.5,"x",true,null,18446744073709551615]},"s":"t\n"})");
    REQUIRE(doc.has_value());
    const json::DomValue root = doc->root();
    CHECK(root.type() == json::DomValue::Type::Object);
    dstd::Vector<dstd::String> keys;
    root.for_each_member([&](dstd::StringView key, json::DomValue value) {
        keys.push_back(dstd::String(key));
        if (key == "s") CHECK(value.as_string() == "t\n");
        if (key != "a") return;
        value.for_each_member([&](dstd::StringView, json::DomValue b) {
            dstd::Vector<json::DomValue::Type> types;
            b.for_each_element(
                [&](json::DomValue e) { types.push_back(e.type()); });
            REQUIRE(types.size() == 7);
            CHECK(types[0] == json::DomValue::Type::Int);
            CHECK(types[2] == json::DomValue::Type::Double);
            CHECK(types[3] == json::DomValue::Type::String);
            CHECK(types[4] == json::DomValue::Type::Bool);
            CHECK(types[5] == json::DomValue::Type::Null);
            CHECK(types[6] == json::DomValue::Type::Uint);
        });
    });
    CHECK(keys.size() == 2);
}

TEST_CASE("DomDocument refuses what it must, with line and column") {
    struct Case {
        const char* text;
        const char* at;
    };
    const Case cases[] = {
        {"{\"a\": 1,}", "1:9:"},
        {"{\"a\": 1} x", "1:10:"},
        {"[1, 2,\n  oops]", "2:3:"},
        {"", "1:1:"},
        {"7", "1:1:"},
        {"{\"a\" 1}", "1:6:"},
        {"{\"a\": 123456789012345678901}", "1:7:"},
        {"{\"a\": 1e999}", "1:7:"},
        {"{\"a\": \"\xff\"}", "1:8:"},
    };
    for (const Case& c : cases) {
        auto doc = json::DomDocument::parse(c.text);
        REQUIRE_MESSAGE(!doc.has_value(), c.text);
        CHECK(doc.error().message().substr(0, dstd::StringView(c.at).size()) ==
              dstd::StringView(c.at));
    }
}

TEST_CASE("DomValue::find reads a member by name") {
    auto doc = json::DomDocument::parse(R"({"a":1,"b":{"c":"d"}})");
    REQUIRE(doc.has_value());
    const auto b = doc->root().find("b");
    REQUIRE(b.has_value());
    const auto c = b->find("c");
    REQUIRE(c.has_value());
    CHECK(c->as_string() == "d");
    CHECK(!doc->root().find("missing").has_value());
    CHECK(!c->find("x").has_value());
}
