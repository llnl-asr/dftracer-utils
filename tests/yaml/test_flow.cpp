// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/yaml/document.h>
#include <doctest/doctest.h>
#include <yaml/yaml_dump.h>

#include <cstddef>

using namespace dftracer::utils;
using dftu_yaml_test::dump;
using yaml::Document;
using yaml::Value;

namespace {

Document must_parse(dstd::StringView text) {
    auto doc = Document::parse(dstd::String(text), "");
    REQUIRE(doc.has_value());
    return std::move(*doc);
}

dstd::String error_of(dstd::StringView text) {
    auto doc = Document::parse(dstd::String(text), "t.yaml");
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error().is_code(common::ErrorCode::InvalidArgument));
    return dstd::String(doc.error().message());
}

bool has(const dstd::String& message, dstd::StringView part) {
    return message.find(part) != dstd::String::npos;
}

}  // namespace

TEST_CASE("yaml flow: a deep nesting skips and reads right") {
    dstd::String text;
    constexpr int DEPTH = 100;
    for (int i = 0; i < DEPTH; ++i) {
        text += dstd::String(static_cast<std::size_t>(i) * 2, ' ');
        text += dstd::fmt<"k{}:\n">(i);
    }
    text += dstd::String(static_cast<std::size_t>(DEPTH) * 2, ' ');
    text += "leaf: end\n";
    text += "after: 1\n";
    const Document doc = must_parse(text);
    Value node = doc.root();
    for (int i = 0; i < DEPTH; ++i) {
        REQUIRE(node.is_object());
        const dstd::String key = dstd::fmt<"k{}">(i);
        const auto next = node.find(key);
        REQUIRE(next);
        node = *next;
    }
    CHECK(node.find("leaf")->raw() == "end");
    CHECK(doc.root().find("after")->as_uint() == 1);
}

TEST_CASE("yaml flow: a sibling after a subtree is found past it") {
    const Document doc = must_parse(
        "a:\n  - {x: [1, [2, [3]]], y: {z: {w: 1}}}\n  - [[], {}]\n"
        "b: 2\nc:\n  d:\n    e: 3\nf: 4\n");
    CHECK(doc.root().find("b")->as_uint() == 2);
    CHECK(doc.root().find("c")->find("d")->find("e")->as_uint() == 3);
    CHECK(doc.root().find("f")->as_uint() == 4);
    CHECK(dump(doc.root()) ==
          "{a:[{x:['1',['2',['3',],],],y:{z:{w:'1',},},},[[],{},],],b:'2',"
          "c:{d:{e:'3',},},f:'4',}");
}

TEST_CASE("yaml flow: a flow value goes over lines, as a value and as a root") {
    const Document doc = must_parse(
        "a: [1,\n  2, {k: v,\n  w: [x,\n y]}]\nb: {p: 1,\n  q: 2}\nc: 3\n");
    CHECK(dump(doc.root()) ==
          "{a:['1','2',{k:'v',w:['x','y',],},],b:{p:'1',q:'2',},c:'3',}");
    const Document root = must_parse("{a: 1,\n b: [2,\n 3]}\n");
    CHECK(dump(root.root()) == "{a:'1',b:['2','3',],}");
    const Document seq = must_parse("- [a,\n  b]\n- {c: d}\n- e\n");
    CHECK(dump(seq.root()) == "[['a','b',],{c:'d',},'e',]");
}

TEST_CASE("yaml flow: quoted and plain scalars in flow collections") {
    const Document doc = must_parse(
        "[\"a\\tb\", 'it''s', plain text, \"two\n  lines\", a:b, {x: 'y'}]\n");
    CHECK(dump(doc.root()) ==
          "['a\tb','it's','plain text','two lines','a:b',{x:'y',},]");
}

TEST_CASE("yaml flow: anchors, aliases and tags are refused") {
    CHECK(has(error_of("a: &x 1\nb: *x\n"), "anchors are not supported"));
    CHECK(has(error_of("a: [1, *x]\n"), "aliases are not supported"));
    CHECK(has(error_of("a: !!str 5\n"), "tags are not supported"));
    CHECK(has(error_of("a: {k: !t v}\n"), "tags are not supported"));
    CHECK(has(error_of("%YAML 1.2\n---\na: 1\n"),
              "directives are not supported"));
}

TEST_CASE("yaml flow: scalars that grow are held in the arena") {
    dstd::String text;
    // \L and \P are one code point of three bytes in two characters.
    for (int i = 0; i < 300; ++i)
        text += dstd::fmt<"k{}: \"v\\L\\P\\L{}\"\n">(i, i);
    const Document doc = must_parse(text);
    CHECK(doc.root().find("k0")->raw() ==
          "v\xE2\x80\xA8\xE2\x80\xA9\xE2\x80\xA8"
          "0");
    CHECK(doc.root().find("k299")->raw() ==
          "v\xE2\x80\xA8\xE2\x80\xA9\xE2\x80\xA8"
          "299");
    int count = 0;
    doc.root().for_each_member([&](dstd::StringView key, Value value) {
        ++count;
        CHECK(value.raw().size() == 3 + 3 + 3 + 1 + (key.size() - 1));
    });
    CHECK(count == 300);
}

TEST_CASE("yaml flow: a grown key is a key, and its duplicate is found") {
    dstd::String text;
    for (int i = 0; i < 80; ++i) text += dstd::fmt<"\"key\\L{}\": {}\n">(i, i);
    CHECK(Document::parse(text, "").has_value());
    text += "\"key\\L7\": again\n";
    auto doc = Document::parse(text, "grow.yaml");
    REQUIRE_FALSE(doc.has_value());
    const dstd::String message(doc.error().message());
    CHECK(message.find("grow.yaml:81:") == 0);
    CHECK(has(message,
              "duplicate key 'key\xE2\x80\xA8"
              "7'"));
}

TEST_CASE("yaml flow: a duplicate key in a flow map is found at any size") {
    CHECK(has(error_of("{a: 1, b: 2, a: 3}\n"), "duplicate key 'a'"));
    dstd::String text = "{";
    for (int i = 0; i < 100; ++i) text += dstd::fmt<"k{}: {}, ">(i, i);
    CHECK(Document::parse(text + "}\n", "").has_value());
    CHECK(has(error_of(text + "k42: x}\n"), "duplicate key 'k42'"));
}

TEST_CASE("yaml flow: folded and block scalars read as text") {
    const Document doc = must_parse(
        "a: >\n  one\n  two\n\n  three\n"
        "b: |\n  l1\n  l2\n"
        "c: \"x\\ty\"\n"
        "d: 'it''s'\n"
        "e: multi\n  line plain\n");
    CHECK(doc.root().find("a")->raw() == "one two\nthree\n");
    CHECK(doc.root().find("b")->raw() == "l1\nl2\n");
    CHECK(doc.root().find("c")->raw() == "x\ty");
    CHECK(doc.root().find("d")->raw() == "it's");
    CHECK(doc.root().find("e")->raw() == "multi line plain");
}

TEST_CASE("yaml flow: a null among block scalars is a string") {
    const Document doc = must_parse("a: |-\n  null\nb: null\nc: \"\"\n");
    CHECK(doc.root().find("a")->is_string());
    CHECK(doc.root().find("b")->is_null());
    CHECK(doc.root().find("c")->is_string());
    CHECK(doc.root().find("c")->raw().empty());
}

TEST_CASE("yaml flow: a flow sequence of single pairs reads as maps") {
    const Document doc = must_parse("[a: 1, {b: 2}, c]\n");
    CHECK(dump(doc.root()) == "[{a:'1',},{b:'2',},'c',]");
}

TEST_CASE("yaml flow: a container or a null as a key is refused") {
    CHECK(has(error_of("? [a, b]\n: v\n"), "complex keys are not supported"));
    CHECK(has(error_of("[a, b]: v\n"), "a collection cannot be a key"));
    CHECK(has(error_of("{a: 1}: v\n"), "a collection cannot be a key"));
    CHECK(has(error_of("{[a]: v}\n"), "a collection cannot be a key"));
    CHECK(has(error_of("~: 1\n"), "a null key is not supported"));
    CHECK(has(error_of("{null: 1}\n"), "a null key is not supported"));
}

TEST_CASE("yaml flow: a stream gives its first document as the root") {
    const Document doc = must_parse("a: 1\n---\nb: 2\n---\n- c\n");
    CHECK(dump(doc.root()) == "{a:'1',}");
    const Document empty_first = must_parse("---\n---\nb: 2\n");
    CHECK(empty_first.root().is_null());
}

TEST_CASE("yaml flow: a second document that is wrong fails the parse") {
    CHECK(error_of("a: 1\n---\nb: [\n").find("t.yaml:") == 0);
    CHECK(has(error_of("a: 1\n---\nk: 1\nk: 2\n"), "duplicate key 'k'"));
}

TEST_CASE("yaml flow: a keyless value, a valueless key and an empty key") {
    const Document doc = must_parse("{a, b: , c: 1}\n");
    CHECK(dump(doc.root()) == "{a:~,b:~,c:'1',}");
    CHECK(has(error_of(": v\n"), "a null key is not supported"));
}

TEST_CASE("yaml flow: a failed parse leaves the next one clean") {
    for (int round = 0; round < 5; ++round) {
        CHECK_FALSE(Document::parse("a: [1, 2\nb: 3\n", "").has_value());
        CHECK_FALSE(Document::parse("{\"k\": 1, \"k\": 2}\n", "").has_value());
        const Document ok = must_parse("a: [1]\nb: {c: 2}\n");
        CHECK(ok.root().find("b")->find("c")->as_uint() == 2);
    }
}

TEST_CASE("yaml flow: CRLF line ends read like LF") {
    const Document lf = must_parse("a: 1\nb:\n  - x\n  - y\nc: |\n  t\n");
    const Document crlf =
        must_parse("a: 1\r\nb:\r\n  - x\r\n  - y\r\nc: |\r\n  t\r\n");
    CHECK(dump(crlf.root()) == dump(lf.root()));
    const Document flow = must_parse("a: [1,\r\n  2]\r\nb: 3\r\n");
    CHECK(dump(flow.root()) == "{a:['1','2',],b:'3',}");
}

TEST_CASE("yaml flow: empty and null-like keys") {
    CHECK(has(error_of("\"\": 1\n\"\": 2\n"), "duplicate key"));
    const Document doc = must_parse("\"\": 1\n\"null\": 2\n");
    CHECK(doc.root().find("null")->as_uint() == 2);
}

TEST_CASE("yaml flow: a flow value that is not closed names its start") {
    const dstd::String seq = error_of("a: 1\nb: [1,\n  2\nc: 3\n");
    CHECK(seq.find("t.yaml:") == 0);
    CHECK(error_of("{a: 1\n").find("t.yaml:") == 0);
    CHECK(error_of("[a, b\n").find("t.yaml:") == 0);
    CHECK(error_of("[a b] c\n").find("t.yaml:") == 0);
    CHECK(error_of("{a: 1} x\n").find("t.yaml:") == 0);
}

TEST_CASE("yaml flow: a line costs no more than its index entry") {
    constexpr std::size_t SIZE = std::size_t{4} << 20;
    dstd::String blank(SIZE, '\n');
    blank += "a: 1\n";
    dstd::String comments;
    for (std::size_t i = 0; i < SIZE / 2; ++i) comments += "#\n";
    comments += "a: 1\n";
    for (dstd::String* text : {&blank, &comments}) {
        const std::size_t size = text->size();
        const std::size_t before = memory::detail::Manager::get().stats().live;
        const Document doc = must_parse(*text);
        const std::size_t held =
            memory::detail::Manager::get().stats().live - before;
        CHECK(doc.root().find("a")->as_uint() == 1);
        // The text, and 12 bytes of index for each line, at most one line for
        // each byte.
        CHECK(held <= 16 * size + (std::size_t{1} << 16));
    }
}
