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

TEST_CASE("DomParser gives each text a document that outlives the next parse") {
    json::DomParser parser;
    dstd::Vector<json::DomDocument> docs;
    const char* texts[] = {
        R"({"name":"first","n":1,"big":18446744073709551615,"f":1.5})",
        R"({"x":[1,"two",{"deep":"three"},[]],"empty":{}})", R"([7,8,9])",
        R"({"s":"café \"q\" \\ \n"})"};
    for (const char* text : texts) {
        auto doc = parser.parse(text);
        REQUIRE(doc.has_value());
        docs.push_back(std::move(*doc));
    }
    // A kept document holds what its text needs, not what the parser sized.
    for (const json::DomDocument& doc : docs) CHECK(doc.bytes() < 400);
    // A longer text parsed last must not disturb the shorter ones kept.
    auto longer = parser.parse(
        R"({"k":"a much longer string than any of the earlier texts hold","m":[1,2,3,4,5,6,7,8,9,10]})");
    REQUIRE(longer.has_value());
    CHECK(longer->root().find("k")->as_string().size() == 55);

    CHECK(docs[0].root().find("name")->as_string() == "first");
    CHECK(docs[0].root().find("n")->as_int() == 1);
    CHECK(docs[0].root().find("big")->as_uint() == 18446744073709551615ULL);
    CHECK(docs[0].root().find("f")->as_double() == 1.5);
    const auto x = docs[1].root().find("x");
    REQUIRE(x.has_value());
    dstd::Vector<json::DomValue::Type> types;
    x->for_each_element([&](json::DomValue e) { types.push_back(e.type()); });
    REQUIRE(types.size() == 4);
    CHECK(types[1] == json::DomValue::Type::String);
    CHECK(types[2] == json::DomValue::Type::Object);
    CHECK(docs[1].root().find("x")->type() == json::DomValue::Type::Array);
    CHECK(docs[2].root().type() == json::DomValue::Type::Array);
    std::int64_t sum = 0;
    docs[2].root().for_each_element(
        [&](json::DomValue e) { sum += e.as_int(); });
    CHECK(sum == 24);
    CHECK(docs[3].root().find("s")->as_string() == "caf\xc3\xa9 \"q\" \\ \n");
}

TEST_CASE("DomParser refuses what DomDocument refuses and stays usable") {
    json::DomParser parser;
    auto bad = parser.parse("{\"a\": 1,}");
    REQUIRE(!bad.has_value());
    CHECK(bad.error().message().starts_with("1:9:"));
    CHECK(!parser.parse("7").has_value());
    CHECK(!parser.parse("").has_value());
    auto good = parser.parse(R"({"a":2})");
    REQUIRE(good.has_value());
    CHECK(good->root().find("a")->as_int() == 2);
}
