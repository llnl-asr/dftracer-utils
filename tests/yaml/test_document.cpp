// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/yaml/document.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cmath>
#include <cstdint>
#include <limits>

using namespace dftracer::utils;
using yaml::Document;
using yaml::Value;

namespace {

Document must_parse(dstd::StringView text) {
    auto doc = Document::parse(dstd::String(text), "");
    REQUIRE(doc.has_value());
    return std::move(*doc);
}

dstd::Optional<bool> bool_of(dstd::StringView spelling) {
    const Document doc =
        must_parse(dstd::String("v: ") + dstd::String(spelling));
    return doc.root().find("v")->as_bool();
}

}  // namespace

TEST_CASE("yaml: objects, arrays and strings") {
    const Document doc = must_parse(
        "name: demo\n"
        "count: 12\n"
        "ratio: 0.25\n"
        "items:\n"
        "  - a\n"
        "  - \"b c\"\n"
        "nested:\n"
        "  inner: yes\n");
    const Value root = doc.root();
    REQUIRE(root.is_object());
    CHECK(root.find("name")->is_string());
    CHECK(root.find("name")->raw() == "demo");
    CHECK(root.find("name")->as_string() == dstd::StringView("demo"));
    CHECK(root.find("count")->as_uint() == 12);
    CHECK(root.find("count")->as_int() == 12);
    CHECK(root.find("ratio")->as_double() == 0.25);
    CHECK(root.find("nested")->is_object());
    CHECK(root.find("nested")->find("inner")->as_bool() == true);

    const auto items = root.find("items");
    REQUIRE(items);
    CHECK(items->is_array());
    dstd::String elements;
    items->for_each_element([&](Value v) {
        elements += v.raw();
        elements += '|';
    });
    CHECK(elements == "a|b c|");

    dstd::String keys;
    root.for_each_member([&](dstd::StringView k, Value) {
        keys += k;
        keys += ' ';
    });
    CHECK(keys == "name count ratio items nested ");
}

TEST_CASE("yaml: a JSON text parses") {
    const Document doc = must_parse("{\"id\": \"x\", \"list\": [1, 2]}");
    CHECK(doc.root().find("id")->raw() == "x");
    CHECK(doc.root().find("list")->is_array());
}

TEST_CASE("yaml: a missing key and a wrong kind give nothing") {
    const Document doc = must_parse("a: 1\nb: [1]\nc:\n");
    const Value root = doc.root();
    CHECK_FALSE(root.find("missing"));
    CHECK_FALSE(root.find("a")->is_object());
    CHECK_FALSE(root.find("a")->find("x"));
    int calls = 0;
    root.find("a")->for_each_member([&](dstd::StringView, Value) { ++calls; });
    root.find("a")->for_each_element([&](Value) { ++calls; });
    root.find("b")->for_each_member([&](dstd::StringView, Value) { ++calls; });
    CHECK(calls == 0);
    CHECK(root.find("b")->raw().empty());
    CHECK_FALSE(root.find("b")->as_string());
    CHECK_FALSE(root.find("b")->as_bool());
    CHECK_FALSE(root.find("b")->as_double());
    CHECK(root.find("c")->is_null());
    CHECK_FALSE(root.find("c")->is_string());
    CHECK_FALSE(root.find("c")->as_uint());
    CHECK_FALSE(root.find("a")->as_bool());
}

TEST_CASE("yaml: empty text is a null root") {
    const Document doc = must_parse("");
    CHECK(doc.root().is_null());
    CHECK_FALSE(doc.root().is_object());
    CHECK_FALSE(doc.root().find("a"));
}

TEST_CASE("yaml: a quoted null is a string and a bare null is not") {
    const Document doc = must_parse("a: \"null\"\nb: null\nc: ~\n");
    CHECK(doc.root().find("a")->is_string());
    CHECK(doc.root().find("a")->raw() == "null");
    CHECK(doc.root().find("b")->is_null());
    CHECK(doc.root().find("c")->is_null());
}

TEST_CASE("yaml: an alias reads as the value it names") {
    const Document doc = must_parse("a: &x 5\nb: *x\n");
    CHECK(doc.root().find("b")->as_uint() == 5);
}

TEST_CASE("yaml: an undefined alias among many anchors is an error") {
    dstd::String text;
    for (int i = 0; i < 40; ++i) text += dstd::fmt<"k{}: &a{} v\n">(i, i);
    text += "bad: *missing\n";
    for (int round = 0; round < 3; ++round) {
        auto doc = Document::parse(text, "alias.yaml");
        REQUIRE_FALSE(doc.has_value());
        CHECK(doc.error().is_code(common::ErrorCode::InvalidArgument));
    }
}

TEST_CASE("yaml: a duplicate key is an error") {
    const auto message_of = [](dstd::StringView text) {
        auto doc = Document::parse(dstd::String(text), "dup.yaml");
        REQUIRE_FALSE(doc.has_value());
        CHECK(doc.error().is_code(common::ErrorCode::InvalidArgument));
        return dstd::String(doc.error().message());
    };
    CHECK(message_of("a: 1\nb: 2\na: 3\n") == "dup.yaml: duplicate key 'a'");
    CHECK(message_of("{\"a\": 1, \"a\": 2}") == "dup.yaml: duplicate key 'a'");
    CHECK(message_of("x:\n  y:\n    k: 1\n    k: 2\n") ==
          "dup.yaml: duplicate key 'k'");
    CHECK(message_of("items:\n  - k: 1\n    k: 2\n") ==
          "dup.yaml: duplicate key 'k'");
    CHECK(message_of("\"a\": 1\na: 2\n") == "dup.yaml: duplicate key 'a'");
    CHECK(message_of("'a': 1\n\"a\": 2\n") == "dup.yaml: duplicate key 'a'");
    auto unnamed = Document::parse("a: 1\na: 2\n", "");
    REQUIRE_FALSE(unnamed.has_value());
    CHECK(dstd::String(unnamed.error().message()) == "duplicate key 'a'");
}

TEST_CASE("yaml: a duplicate key is found in a large map too") {
    dstd::String text;
    for (int i = 0; i < 100; ++i) text += dstd::fmt<"key{}: {}\n">(i, i);
    CHECK(Document::parse(text, "").has_value());
    text += "key57: again\n";
    auto doc = Document::parse(text, "big.yaml");
    REQUIRE_FALSE(doc.has_value());
    CHECK(dstd::String(doc.error().message()) ==
          "big.yaml: duplicate key 'key57'");
}

TEST_CASE("yaml: distinct keys and the same key in two maps are fine") {
    const Document doc = must_parse(
        "a: 1\nab: 2\nabc: 3\n"
        "x:\n  a: 1\ny:\n  a: 2\n"
        "list:\n  - k: 1\n  - k: 2\n");
    CHECK(doc.root().find("ab")->as_uint() == 2);
    CHECK(doc.root().find("abc")->as_uint() == 3);
    CHECK(Document::parse("{\"a\": 1, \"ab\": 2}", "").has_value());
}

TEST_CASE("yaml: a merge key may override a key it merges") {
    const Document doc = must_parse(
        "base: &b {k: 1, z: 9}\n"
        "item:\n  <<: *b\n  k: 2\n");
    CHECK(doc.root().find("item")->find("k")->as_uint() == 2);
    CHECK(doc.root().find("item")->find("z")->as_uint() == 9);
}

TEST_CASE("yaml: bool spellings") {
    for (const char* t : {"y", "Y", "yes", "Yes", "YES", "true", "True", "TRUE",
                          "on", "On", "ON"}) {
        CAPTURE(t);
        CHECK(bool_of(t) == true);
    }
    for (const char* f : {"n", "N", "no", "No", "NO", "false", "False", "FALSE",
                          "off", "Off", "OFF"}) {
        CAPTURE(f);
        CHECK(bool_of(f) == false);
    }
    for (const char* bad : {"tRue", "yEs", "1", "0", "maybe", "truee", "ofF"}) {
        CAPTURE(bad);
        CHECK_FALSE(bool_of(bad));
    }
}

TEST_CASE("yaml: numbers") {
    const Document doc = must_parse(
        "a: 1e3\nb: -2.5\nc: .inf\nd: -.inf\ne: .nan\nf: abc\ng: -1\nh: 1.5\n"
        "i: +7\nj: 12x\nk: +0.5\nl: 18446744073709551615\n"
        "m: 18446744073709551616\nn: -9223372036854775808\n"
        "o: 9223372036854775808\n");
    const Value root = doc.root();
    CHECK(root.find("a")->as_double() == 1000.0);
    CHECK(root.find("b")->as_double() == -2.5);
    CHECK(std::isinf(*root.find("c")->as_double()));
    CHECK(*root.find("d")->as_double() < 0);
    CHECK(std::isnan(*root.find("e")->as_double()));
    CHECK_FALSE(root.find("f")->as_double());
    CHECK(root.find("g")->as_int() == -1);
    CHECK_FALSE(root.find("g")->as_uint());
    CHECK_FALSE(root.find("h")->as_uint());
    CHECK_FALSE(root.find("h")->as_int());
    CHECK(root.find("i")->as_uint() == 7);
    CHECK(root.find("i")->as_int() == 7);
    CHECK_FALSE(root.find("j")->as_uint());
    CHECK_FALSE(root.find("j")->as_double());
    CHECK(root.find("k")->as_double() == 0.5);
    CHECK(root.find("l")->as_uint() ==
          std::numeric_limits<std::uint64_t>::max());
    CHECK_FALSE(root.find("l")->as_int());
    CHECK_FALSE(root.find("m")->as_uint());
    CHECK(root.find("n")->as_int() == std::numeric_limits<std::int64_t>::min());
    CHECK_FALSE(root.find("o")->as_int());
}

TEST_CASE("yaml: a syntax error is InvalidArgument with a location") {
    auto doc = Document::parse("a: 1\nb: [1, 2\nc: 3\n", "spec.yaml");
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error().is_code(common::ErrorCode::InvalidArgument));
    const dstd::String message(doc.error().message());
    CHECK(message.find("spec.yaml:4:") == 0);
}

TEST_CASE("yaml: an error without a source name starts with the line") {
    auto doc = Document::parse("a: [1\nb: 2\n", "");
    REQUIRE_FALSE(doc.has_value());
    const dstd::String message(doc.error().message());
    CHECK(message.find("3:") == 0);
}

TEST_CASE("yaml: a parse after an error works") {
    for (int round = 0; round < 3; ++round) {
        auto bad = Document::parse("a: {b: [", "bad");
        CHECK_FALSE(bad.has_value());
        const Document good = must_parse("k: v\n");
        CHECK(good.root().find("k")->raw() == "v");
    }
}

TEST_CASE("yaml: a document survives a move") {
    Document first = must_parse("k: a value long enough to leave SSO ....\n");
    Document second = std::move(first);
    CHECK(second.root().find("k")->raw() ==
          "a value long enough to leave SSO ....");
}

TEST_CASE("yaml: a string moved in is the buffer of the document") {
    dstd::String text(1000, ' ');
    text += "k: v\n";
    const char* data = text.data();
    auto doc = Document::parse(std::move(text), "");
    REQUIRE(doc.has_value());
    const dstd::StringView value = *doc->root().find("k")->as_string();
    CHECK(value.data() > data);
    CHECK(value.data() < data + 1010);
}

TEST_CASE("yaml: load_file reads a file and names it in an error") {
    const dstd::Path path =
        dftu_utils_test::make_unique_test_path("yaml_load_file");
    const dstd::String name(path.str());
    REQUIRE(dstd::fs::write_file(path, "k: [1\n").has_value());
    auto bad = Document::load_file(name);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().is_code(common::ErrorCode::InvalidArgument));
    CHECK(dstd::String(bad.error().message()).find(name) == 0);

    REQUIRE(dstd::fs::write_file(path, "k: v\n").has_value());
    auto good = Document::load_file(name);
    REQUIRE(good.has_value());
    CHECK(good->root().find("k")->raw() == "v");
    REQUIRE(dstd::fs::remove(path).has_value());

    CHECK_FALSE(Document::load_file(name).has_value());
}
