// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/escape.h>
#include <dftracer/utils/yaml/writer.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>

using namespace dftracer::utils;
using yaml::Document;
using yaml::Value;
using yaml::Writer;

namespace {

struct Trap {
    const char* text;     // the string to write
    const char* written;  // exactly how the writer spells it
};

const Trap TRAPS[] = {
    {"true", "\"true\""},
    {"false", "\"false\""},
    {"True", "\"True\""},
    {"FALSE", "\"FALSE\""},
    {"Yes", "\"Yes\""},
    {"no", "\"no\""},
    {"ON", "\"ON\""},
    {"off", "\"off\""},
    {"y", "\"y\""},
    {"n", "\"n\""},
    {"Y", "\"Y\""},
    {"N", "\"N\""},
    {"~", "\"~\""},
    {"null", "\"null\""},
    {"Null", "\"Null\""},
    {"NULL", "\"NULL\""},
    {"", "\"\""},
    {"123", "\"123\""},
    {"-5", "\"-5\""},
    {"+5", "\"+5\""},
    {"0x1F", "\"0x1F\""},
    {"0o17", "\"0o17\""},
    {"017", "\"017\""},
    {"1_000", "\"1_000\""},
    {"1:20", "\"1:20\""},
    {"1e3", "\"1e3\""},
    {"1.5", "\"1.5\""},
    {".5", "\".5\""},
    {"-.5", "\"-.5\""},
    {"1.", "\"1.\""},
    {"2001-12-14", "\"2001-12-14\""},
    {"2001-12-14 10:00:00", "\"2001-12-14 10:00:00\""},
    {"<<", "\"<<\""},
    {"=", "\"=\""},
    {"---", "\"---\""},
    {"...", "\"...\""},
    {"--- x", "\"--- x\""},
    {"- x", "\"- x\""},
    {": x", "\": x\""},
    {"a: b", "\"a: b\""},
    {"a:", "\"a:\""},
    {"a #b", "\"a #b\""},
    {" lead", "\" lead\""},
    {"trail ", "\"trail \""},
    {"[x]", "\"[x]\""},
    {"{x}", "\"{x}\""},
    {"&a", "\"&a\""},
    {"*a", "\"*a\""},
    {"!t", "\"!t\""},
    {"|", "\"|\""},
    {">", "\">\""},
    {"'q'", "\"'q'\""},
    {"%p", "\"%p\""},
    {"@a", "\"@a\""},
    {"`b`", "\"`b`\""},
    {"#c", "\"#c\""},
    {".inf", "\".inf\""},
    {"-.inf", "\"-.inf\""},
    {".NaN", "\".NaN\""},
    {"0b101", "\"0b101\""},
    {"tab\there", "\"tab\\there\""},
    {"new\nline", "\"new\\nline\""},
    {"cr\rx", "\"cr\\rx\""},
    {"\x7f", "\"\\u007f\""},
    {"quote\"d", "\"quote\\\"d\""},
    {"back\\slash", "\"back\\\\slash\""},
    {"\xc2\x85", "\"\\u0085\""},
    {"\xc2\xa0", "\"\\u00a0\""},
    {"\xe2\x80\xa8", "\"\\u2028\""},
    {"\xef\xbb\xbf", "\"\\ufeff\""},
    {"bad\xff"
     "utf",
     "\"bad\\ufffdutf\""},
    {"plain words ok", "plain words ok"},
    {"a-b_c.d", "a-b_c.d"},
    {"1st", "1st"},
    {"e5", "e5"},
    {"x,y", "x,y"},
    {"x;y", "x;y"},
    {"k=v", "k=v"},
    {"p/q/r", "p/q/r"},
    {"emoji \xf0\x9f\x98\x80", "emoji \xf0\x9f\x98\x80"},
    {"caf\xc3\xa9", "caf\xc3\xa9"},
};

constexpr std::size_t TRAP_COUNT = sizeof(TRAPS) / sizeof(TRAPS[0]);

// What the text must read back as: the string itself, except that a byte
// that is not valid UTF-8 is repaired.
dstd::String expected_text(dstd::StringView s) {
    if (s == "bad\xffutf") return "bad\xef\xbf\xbdutf";
    return dstd::String(s);
}

}  // namespace

TEST_CASE("writer: a flat object") {
    dstd::String out;
    Writer w(out);
    w.begin_object();
    w.member("name", "demo");
    w.member("count", 12);
    w.member("ratio", 0.5);
    w.member("on", true);
    w.key("nothing");
    w.null();
    w.end_object();
    CHECK(out ==
          "name: demo\n"
          "count: 12\n"
          "ratio: 0.5\n"
          "\"on\": true\n"
          "nothing: null");
}

TEST_CASE("writer: nesting and arrays of objects") {
    dstd::String out;
    Writer w(out);
    w.begin_object();
    w.key("a");
    w.begin_object();
    w.member("b", 1);
    w.key("c");
    w.begin_array();
    w.value("x");
    w.value(std::int64_t{2});
    w.begin_object();
    w.member("k", "v");
    w.member("l", "w");
    w.end_object();
    w.begin_array();
    w.value("n1");
    w.value("n2");
    w.end_array();
    w.end_array();
    w.end_object();
    w.member("z", 3);
    w.end_object();
    CHECK(out ==
          "a:\n"
          "  b: 1\n"
          "  c:\n"
          "    - x\n"
          "    - 2\n"
          "    - k: v\n"
          "      l: w\n"
          "    - - n1\n"
          "      - n2\n"
          "z: 3");
}

TEST_CASE("writer: empty containers and a root array") {
    dstd::String out;
    Writer w(out);
    w.begin_object();
    w.key("o");
    w.begin_object();
    w.end_object();
    w.key("a");
    w.begin_array();
    w.end_array();
    w.key("items");
    w.begin_array();
    w.begin_object();
    w.end_object();
    w.begin_array();
    w.end_array();
    w.end_array();
    w.end_object();
    CHECK(out ==
          "o: {}\n"
          "a: []\n"
          "items:\n"
          "  - {}\n"
          "  - []");

    dstd::String root;
    Writer r(root);
    r.begin_array();
    r.begin_object();
    r.member("k", 1);
    r.end_object();
    r.value("s");
    r.end_array();
    CHECK(root == "- k: 1\n- s");

    dstd::String empty;
    Writer e(empty);
    e.begin_object();
    e.end_object();
    CHECK(empty == "{}");

    dstd::String scalar;
    Writer s(scalar);
    s.value("just text");
    CHECK(scalar == "just text");
}

TEST_CASE("writer: the indent option and an existing prefix") {
    dstd::String out = "# head\n";
    yaml::WriterOptions options;
    options.indent = 4;
    Writer w(out, options);
    w.begin_object();
    w.key("a");
    w.begin_object();
    w.member("b", 1);
    w.end_object();
    w.end_object();
    CHECK(out == "# head\na:\n    b: 1");
}

TEST_CASE("writer: integer overloads are unambiguous") {
    dstd::String out;
    Writer w(out);
    w.begin_array();
    w.value(1);
    w.value(1L);
    w.value(1LL);
    w.value(1u);
    w.value(1UL);
    w.value(1ULL);
    w.value(std::size_t{7});
    w.value(std::int8_t{-3});
    w.value(std::uint16_t{9});
    w.value(std::numeric_limits<std::int64_t>::min());
    w.value(std::numeric_limits<std::int64_t>::max());
    w.value(std::numeric_limits<std::uint64_t>::max());
    w.value(true);
    w.value(false);
    w.value(1.5);
    w.value("literal");
    w.value(dstd::String("owned"));
    w.value(dstd::StringView("view"));
    w.end_array();
    CHECK(out ==
          "- 1\n- 1\n- 1\n- 1\n- 1\n- 1\n- 7\n- -3\n- 9\n"
          "- -9223372036854775808\n- 9223372036854775807\n"
          "- 18446744073709551615\n- true\n- false\n- 1.5\n- literal\n"
          "- owned\n- view");
}

TEST_CASE("writer: doubles always have a decimal point") {
    struct Case {
        double value;
        const char* text;
    };
    const Case cases[] = {
        {1.0, "1.0"},
        {0.1, "0.1"},
        {-0.0, "-0.0"},
        {0.0, "0.0"},
        {100.0, "100.0"},
        {1e5, "1.0e+05"},
        {1e20, "1.0e+20"},
        {1.5e-7, "1.5e-07"},
        {5e-324, "5.0e-324"},
        {1e-4, "1.0e-04"},
        {123456.789, "123456.789"},
        {-2.5, "-2.5"},
        {1.7976931348623157e308, "1.7976931348623157e+308"},
        {std::nan(""), ".nan"},
        {std::numeric_limits<double>::infinity(), ".inf"},
        {-std::numeric_limits<double>::infinity(), "-.inf"},
    };
    for (const Case& c : cases) {
        dstd::String out;
        Writer w(out);
        w.value(c.value);
        CAPTURE(c.text);
        CHECK(out == c.text);
    }
}

TEST_CASE("writer: doubles read back as the same double") {
    const double values[] = {
        1.0, 0.1,        -0.0, 1e20,      5e-324, 1e-7,     1e300,
        0.3, 123456.789, -2.5, 1.0 / 3.0, 2e-7,   4.9e-324, 9007199254740993.0};
    for (const double v : values) {
        dstd::String out;
        Writer w(out);
        w.begin_object();
        w.member("v", v);
        w.end_object();
        auto doc = Document::parse(out, "");
        REQUIRE(doc.has_value());
        const auto read = doc->root().find("v")->as_double();
        REQUIRE(read.has_value());
        CAPTURE(out);
        CHECK(*read == v);
        CHECK(std::signbit(*read) == std::signbit(v));
        CHECK_FALSE(doc->root().find("v")->as_int());
    }
}

TEST_CASE("writer: every trap string is spelled exactly as expected") {
    for (const Trap& trap : TRAPS) {
        dstd::String as_value;
        Writer v(as_value);
        v.value(trap.text);
        CAPTURE(trap.text);
        CHECK(as_value == trap.written);

        dstd::String as_key;
        Writer k(as_key);
        k.begin_object();
        k.member(trap.text, 1);
        k.end_object();
        CHECK(as_key == dstd::String(trap.written) + ": 1");
    }
}

TEST_CASE("writer: every trap string round-trips as a string value") {
    for (const Trap& trap : TRAPS) {
        const dstd::String text(trap.text);
        dstd::String out;
        Writer w(out);
        w.begin_object();
        w.member("k", text);
        w.end_object();
        CAPTURE(out);
        auto doc = Document::parse(out, "");
        REQUIRE(doc.has_value());
        const auto v = doc->root().find("k");
        REQUIRE(v.has_value());
        CHECK(v->is_string());
        CHECK(v->raw() == expected_text(text));
    }
}

TEST_CASE("writer: every trap string round-trips as a key") {
    for (const Trap& trap : TRAPS) {
        const dstd::String text(trap.text);
        dstd::String out;
        Writer w(out);
        w.begin_object();
        w.member(text, 1);
        w.end_object();
        CAPTURE(out);
        auto doc = Document::parse(out, "");
        REQUIRE(doc.has_value());
        dstd::String key;
        int members = 0;
        doc->root().for_each_member([&](dstd::StringView k, Value v) {
            key = dstd::String(k);
            ++members;
            CHECK(v.as_int() == 1);
        });
        CHECK(members == 1);
        CHECK(key == expected_text(text));
    }
}

TEST_CASE("writer: every trap string round-trips as an array element") {
    dstd::String out;
    Writer w(out);
    w.begin_array();
    for (const Trap& trap : TRAPS) w.value(trap.text);
    w.end_array();
    auto doc = Document::parse(out, "");
    REQUIRE(doc.has_value());
    std::size_t i = 0;
    doc->root().for_each_element([&](Value v) {
        CHECK(v.is_string());
        CHECK(v.raw() == expected_text(TRAPS[i].text));
        ++i;
    });
    CHECK(i == TRAP_COUNT);
}

TEST_CASE("writer: a key over 1024 bytes is an explicit key") {
    const dstd::String long_key(1100, 'k');
    const dstd::String edge_key(1024, 'e');
    {
        dstd::String out;
        Writer w(out);
        w.begin_object();
        w.member(edge_key, 1);
        w.member(long_key, "v");
        w.key(long_key + "2");
        w.begin_object();
        w.member("a", 1);
        w.member("b", 2);
        w.end_object();
        w.key(long_key + "3");
        w.begin_object();
        w.end_object();
        w.key(long_key + "4");
        w.begin_array();
        w.value("x");
        w.end_array();
        w.member("after", true);
        w.end_object();
        CHECK(out.find(edge_key + ": 1") == 0);
        CHECK(out.find("\n? " + long_key + "\n: v\n") != dstd::String::npos);
        CHECK(out.find("\n? " + long_key + "2\n:\n  a: 1\n  b: 2\n") !=
              dstd::String::npos);
        CHECK(out.find("\n? " + long_key + "3\n: {}\n") != dstd::String::npos);
        CHECK(out.find("\n? " + long_key + "4\n:\n  - x\n") !=
              dstd::String::npos);
        auto doc = Document::parse(out, "");
        REQUIRE(doc.has_value());
        CHECK(doc->root().find(edge_key)->as_int() == 1);
        CHECK(doc->root().find(long_key)->raw() == "v");
        CHECK(doc->root().find(long_key + "2")->find("b")->as_int() == 2);
        CHECK(doc->root().find(long_key + "3")->is_object());
        CHECK(doc->root().find(long_key + "4")->is_array());
        CHECK(doc->root().find("after")->as_bool() == true);
    }
    {
        // A key that needs quotes counts its written form.
        const dstd::String quoted_key(1030, '\n');
        dstd::String out;
        Writer w(out);
        w.begin_array();
        w.begin_object();
        w.member(quoted_key, "v");
        w.member("next", 1);
        w.end_object();
        w.end_array();
        auto doc = Document::parse(out, "");
        REQUIRE(doc.has_value());
        dstd::String key;
        doc->root().for_each_element([&](Value item) {
            item.for_each_member([&](dstd::StringView k, Value) {
                if (key.empty()) key = dstd::String(k);
            });
        });
        CHECK(key == quoted_key);
    }
}

TEST_CASE("writer: plain strings stay plain and traps get quotes") {
    const auto text_of = [](const char* s) {
        dstd::String out;
        Writer w(out);
        w.value(s);
        return out;
    };
    CHECK(text_of("plain words ok") == "plain words ok");
    CHECK(text_of("a-b_c.d") == "a-b_c.d");
    CHECK(text_of("1st") == "1st");
    CHECK(text_of("caf\xc3\xa9") == "caf\xc3\xa9");
    CHECK(text_of("emoji \xf0\x9f\x98\x80") == "emoji \xf0\x9f\x98\x80");
    CHECK(text_of("true") == "\"true\"");
    CHECK(text_of("") == "\"\"");
    CHECK(text_of("null") == "\"null\"");
    CHECK(text_of("1e3") == "\"1e3\"");
    CHECK(text_of("1:20") == "\"1:20\"");
    CHECK(text_of("a: b") == "\"a: b\"");
    CHECK(text_of("a #b") == "\"a #b\"");
    CHECK(text_of("new\nline") == "\"new\\nline\"");
    CHECK(text_of("tab\t") == "\"tab\\t\"");
    CHECK(text_of("\x7f") == "\"\\u007f\"");
    CHECK(text_of("q\"b\\") == "\"q\\\"b\\\\\"");
    CHECK(text_of("\xc2\x85") == "\"\\u0085\"");
    CHECK(text_of("\xe2\x80\xa8") == "\"\\u2028\"");
    CHECK(text_of("\xef\xbb\xbf") == "\"\\ufeff\"");
    CHECK(text_of("a\xff") == "\"a\\ufffd\"");
    CHECK(text_of("\x01") == "\"\\u0001\"");
}

TEST_CASE("writer: raw_value and null") {
    dstd::String out;
    Writer w(out);
    w.begin_object();
    w.key("t");
    w.raw_value("2001-12-14");
    w.key("n");
    w.null();
    w.end_object();
    CHECK(out == "t: 2001-12-14\n\"n\": null");
}

TEST_CASE("escaped_yaml_size is the exact size of the written text") {
    dstd::Vector<dstd::String> samples = {"",
                                          "plain",
                                          "a\"b\\c\n",
                                          "\x01\x02\x1f\x7f",
                                          "caf\xc3\xa9",
                                          "\xf0\x9f\x98\x80",
                                          "\xc2\x85\xc2\xa0",
                                          "\xe2\x80\xa8\xe2\x80\xa9",
                                          "\xff\xfe",
                                          "\xe0\x80\x80",
                                          "\xed\xa0\x80",
                                          "\xf4\x90\x80\x80",
                                          "\xc3",
                                          "\xe2\x82"};
    dstd::String mixed;
    for (int i = 0; i < 400; ++i) mixed += static_cast<char>(i % 256);
    samples.push_back(mixed);
    for (const dstd::String& s : samples) {
        dstd::String buffer(6 * s.size() + 1, '\0');
        char* end = yaml::escape_yaml_to(buffer.data(), s);
        CHECK(static_cast<std::size_t>(end - buffer.data()) ==
              yaml::escaped_yaml_size(s));
    }
}

TEST_CASE("a long string reserves its exact escaped size") {
    const dstd::String clean(100000, 'a');
    CHECK(yaml::YamlEscape::bound(clean) == clean.size());
    dstd::String dirty;
    for (int i = 0; i < 1000; ++i) dirty += "ab\"cd\x01 ef\n";
    CHECK(yaml::YamlEscape::bound(dirty) == dirty.size() + 1000 * (1 + 5 + 1));
    const dstd::String short_dirty(63, '\x01');
    CHECK(yaml::YamlEscape::bound(short_dirty) == 6 * 63);
    const dstd::String at(yaml::YamlEscape::LONG_STRING, '\n');
    CHECK(yaml::YamlEscape::bound(at) == 2 * at.size());
    dstd::String out;
    Writer w(out);
    w.value(dirty);
    CHECK(out.size() == dirty.size() + 1000 * 7 + 2);
}
