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

}  // namespace

TEST_CASE("yaml tape: a deep nesting skips and reads right") {
    dstd::String text;
    constexpr int DEPTH = 200;
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

TEST_CASE("yaml tape: a sibling after a subtree is found past it") {
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

TEST_CASE("yaml tape: an alias names an earlier node, a map included") {
    const Document doc = must_parse(
        "base: &b {k: 1, list: [1, 2]}\n"
        "copy: *b\n"
        "items:\n  - *b\n  - &s one\n  - *s\n");
    CHECK(dump(doc.root().find("copy")->find("list").value()) == "['1','2',]");
    CHECK(doc.root().find("copy")->find("k")->as_uint() == 1);
    int elements = 0;
    doc.root().find("items")->for_each_element([&](Value v) {
        ++elements;
        if (elements == 1) CHECK(v.is_object());
        if (elements == 3) CHECK(v.raw() == "one");
    });
    CHECK(elements == 3);
}

TEST_CASE("yaml tape: a merge key reads its maps, and an own key wins") {
    const Document doc = must_parse(
        "a: &a {x: 1, y: 2}\n"
        "b: &b {y: 20, z: 30}\n"
        "one:\n  <<: *a\n  x: 100\n"
        "two:\n  <<: [*a, *b]\n  w: 0\n"
        "three:\n  w: 0\n  <<: *a\n");
    const Value one = *doc.root().find("one");
    CHECK(one.find("x")->as_uint() == 100);
    CHECK(one.find("y")->as_uint() == 2);
    CHECK_FALSE(one.find("<<"));
    CHECK(dump(one) == "{y:'2',x:'100',}");
    const Value two = *doc.root().find("two");
    CHECK(two.find("y")->as_uint() == 2);
    CHECK(two.find("z")->as_uint() == 30);
    CHECK(two.find("w")->as_uint() == 0);
    CHECK(dump(two) == "{x:'1',y:'2',z:'30',w:'0',}");
    CHECK(doc.root().find("three")->find("x")->as_uint() == 1);
}

TEST_CASE("yaml tape: a merge key that names a merge key") {
    const Document doc = must_parse(
        "a: &a {p: 1}\n"
        "b: &b\n  <<: *a\n  q: 2\n"
        "c:\n  <<: *b\n");
    CHECK(doc.root().find("c")->find("p")->as_uint() == 1);
    CHECK(doc.root().find("c")->find("q")->as_uint() == 2);
}

TEST_CASE("yaml tape: an alias to a node that holds it is refused") {
    CHECK(error_of("a: &x [1, *x]\n").find("t.yaml:") == 0);
    CHECK(error_of("a: &x {k: *x}\n").find("contains it") !=
          dstd::String::npos);
    CHECK(error_of("- &x\n  - *x\n").find("contains it") != dstd::String::npos);
}

TEST_CASE("yaml tape: an alias that expands too far is refused") {
    dstd::String text = "l0: &a0 [x, x, x, x, x, x, x, x, x, x]\n";
    for (int i = 1; i < 12; ++i) {
        text += dstd::fmt<"l{}: &a{} [">(i, i);
        for (int j = 0; j < 10; ++j) text += dstd::fmt<"*a{}, ">(i - 1);
        text += "]\n";
    }
    const dstd::String message = error_of(text);
    CHECK(message.find("expand") != dstd::String::npos);
}

TEST_CASE("yaml tape: a long alias chain that expands little is fine") {
    dstd::String text = "a0: &a0 [1]\n";
    for (int i = 1; i < 500; ++i)
        text += dstd::fmt<"a{}: &a{} [*a{}]\n">(i, i, i - 1);
    const Document doc = must_parse(text);
    CHECK(doc.root().find("a499").has_value());
}

TEST_CASE("yaml tape: an undefined alias and a later anchor are refused") {
    CHECK(error_of("a: *x\nb: &x 1\n").find("undefined alias") !=
          dstd::String::npos);
}

TEST_CASE("yaml tape: scalars that grow are held in the arena") {
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

TEST_CASE("yaml tape: a grown key is a key, and its duplicate is found") {
    dstd::String text;
    for (int i = 0; i < 80; ++i) text += dstd::fmt<"\"key\\L{}\": {}\n">(i, i);
    CHECK(Document::parse(text, "").has_value());
    text += "\"key\\L7\": again\n";
    auto doc = Document::parse(text, "grow.yaml");
    REQUIRE_FALSE(doc.has_value());
    const dstd::String message(doc.error().message());
    CHECK(message.find("grow.yaml:81:") == 0);
    CHECK(message.find("duplicate key 'key\xE2\x80\xA8"
                       "7'") != dstd::String::npos);
}

TEST_CASE("yaml tape: folded and block scalars read as text") {
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

TEST_CASE("yaml tape: a null among block scalars is a string") {
    const Document doc = must_parse("a: |-\n  null\nb: null\nc: \"\"\n");
    CHECK(doc.root().find("a")->is_string());
    CHECK(doc.root().find("b")->is_null());
    CHECK(doc.root().find("c")->is_string());
    CHECK(doc.root().find("c")->raw().empty());
}

TEST_CASE("yaml tape: a flow sequence of single pairs reads as maps") {
    const Document doc = must_parse("[a: 1, {b: 2}, c]\n");
    CHECK(dump(doc.root()) == "[{a:'1',},{b:'2',},'c',]");
}

TEST_CASE("yaml tape: a container as a key is refused") {
    CHECK(error_of("? [a, b]\n: v\n").find("t.yaml:") == 0);
    CHECK(error_of("[a, b]: v\n").find("complex keys") != dstd::String::npos);
    CHECK(error_of("{a: 1}: v\n").find("complex keys") != dstd::String::npos);
}

TEST_CASE("yaml tape: a stream gives its first document as the root") {
    const Document doc = must_parse("a: 1\n---\nb: 2\n---\n- c\n");
    CHECK(dump(doc.root()) == "{a:'1',}");
    const Document empty_first = must_parse("---\n---\nb: 2\n");
    CHECK(empty_first.root().is_null());
}

TEST_CASE("yaml tape: a second document that is wrong fails the parse") {
    CHECK(error_of("a: 1\n---\nb: [\n").find("t.yaml:") == 0);
    CHECK(error_of("a: 1\n---\nk: 1\nk: 2\n").find("duplicate key 'k'") !=
          dstd::String::npos);
}

TEST_CASE("yaml tape: a keyless value and a valueless key") {
    const Document doc = must_parse("{a, b: , c: 1}\n");
    CHECK(dump(doc.root()) == "{a:~,b:~,c:'1',}");
    const Document doc2 = must_parse(": v\n");
    CHECK(doc2.root().find("")->raw() == "v");
}

TEST_CASE("yaml tape: tags and directives are ignored") {
    const Document doc = must_parse(
        "%YAML 1.2\n---\na: !!str 5\nb: !custom {x: 1}\nc: !!int \"7\"\n");
    CHECK(doc.root().find("a")->raw() == "5");
    CHECK(doc.root().find("b")->find("x")->as_uint() == 1);
}

TEST_CASE("yaml tape: a failed parse leaves the next one clean") {
    for (int round = 0; round < 5; ++round) {
        CHECK_FALSE(Document::parse("a: &x 1\nb: *y\n", "").has_value());
        CHECK_FALSE(Document::parse("{\"k\": 1, \"k\": 2}\n", "").has_value());
        const Document ok = must_parse("a: &x 1\nb: *x\n");
        CHECK(ok.root().find("b")->as_uint() == 1);
    }
}

TEST_CASE("yaml tape: CRLF line ends read like LF") {
    const Document lf = must_parse("a: 1\nb:\n  - x\n  - y\nc: |\n  t\n");
    const Document crlf =
        must_parse("a: 1\r\nb:\r\n  - x\r\n  - y\r\nc: |\r\n  t\r\n");
    CHECK(dump(lf.root()).size() > 0);
    CHECK(lf.root().find("b")->find("zz").has_value() ==
          crlf.root().find("b")->find("zz").has_value());
    CHECK(dump(crlf.root().find("a").value()) == "'1'");
    CHECK(crlf.root().find("c")->raw() == "t\n");
}

TEST_CASE("yaml tape: null and empty keys are one key at every map size") {
    CHECK(error_of("~: 1\nnull: 2\n").find("duplicate key") !=
          dstd::String::npos);
    CHECK(error_of("~: 1\n\"\": 2\n").find("duplicate key") !=
          dstd::String::npos);
    for (const char* second : {"~", "null"}) {
        dstd::String text = "~: 0\n";
        for (int i = 0; i < 100; ++i) text += dstd::fmt<"k{}: 1\n">(i);
        text += dstd::fmt<"{}: 2\n">(second);
        CHECK(error_of(text).find("duplicate key") != dstd::String::npos);
    }
}

TEST_CASE("yaml tape: an anchor does not outlive its document") {
    CHECK(error_of("--- &x a\n--- *x\n").find("x") != dstd::String::npos);
    CHECK_FALSE(Document::parse("--- &x a\n...\n--- *x\n", "").has_value());
    CHECK(Document::parse("--- &x a\n--- &x b\n", "").has_value());
}

TEST_CASE("yaml tape: a merge key inside a flow sequence still merges") {
    const Document doc = must_parse("[<<: {a: 1}]\n");
    int seen = 0;
    doc.root().for_each_element([&](Value element) {
        ++seen;
        const auto a = element.find("a");
        REQUIRE(a.has_value());
        CHECK(a->as_uint() == 1);
    });
    CHECK(seen == 1);
}

TEST_CASE("yaml tape: blank and comment lines do not reserve a huge tape") {
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
        // The text, and a tape of about four times its size at most; the old
        // reserve held sixteen times.
        CHECK(held <= 6 * size + (std::size_t{1} << 16));
    }
}
