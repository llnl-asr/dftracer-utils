// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

using namespace dftracer::utils;

using json::Document;
using json::Value;

static_assert(!std::is_copy_constructible_v<Value>);
static_assert(std::is_nothrow_move_constructible_v<Value>);
static_assert(!std::is_copy_constructible_v<Document>);
static_assert(std::is_nothrow_move_constructible_v<Document>);

namespace {

Value root(Document& doc, dstd::StringView line) {
    auto parsed = doc.parse(line);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

Value field(Value& parent, dstd::StringView key) {
    auto found = parent.find(key);
    REQUIRE(found.has_value());
    return std::move(*found);
}

}  // namespace

TEST_CASE("every type reports itself") {
    Document doc;
    Value v = root(doc, R"({"o":{},"a":[],"s":"x","i":-3,"d":1.5,"t":true,)"
                        R"("f":false,"n":null,"big":18446744073709551615})");
    CHECK(v.type() == Value::Type::Object);
    CHECK(field(v, "o").type() == Value::Type::Object);
    CHECK(field(v, "a").type() == Value::Type::Array);
    CHECK(field(v, "s").type() == Value::Type::String);
    CHECK(field(v, "i").type() == Value::Type::Int);
    CHECK(field(v, "d").type() == Value::Type::Double);
    CHECK(field(v, "t").type() == Value::Type::Bool);
    CHECK(field(v, "f").type() == Value::Type::Bool);
    CHECK(field(v, "n").type() == Value::Type::Null);
    CHECK(field(v, "big").type() == Value::Type::Double);
}

TEST_CASE("a read of the wrong type is empty and leaves the value readable") {
    Document doc;
    Value v =
        root(doc, R"({"o":{"k":1},"a":[1],"s":"x","i":7,"d":1.5,"t":true,)"
                  R"("n":null})");
    {
        Value o = field(v, "o");
        CHECK_FALSE(o.as_int());
        CHECK_FALSE(o.as_double());
        CHECK_FALSE(o.as_string());
        CHECK_FALSE(o.as_bool());
        auto k = o.find("k");
        REQUIRE(k.has_value());
        CHECK(k->as_int() == 1);
    }
    {
        Value a = field(v, "a");
        CHECK_FALSE(a.as_int());
        CHECK_FALSE(a.as_string());
        CHECK_FALSE(a.find("k"));
        std::int64_t sum = 0;
        a.for_each_element([&](Value e) { sum += e.as_int().value_or(0); });
        CHECK(sum == 1);
    }
    {
        Value s = field(v, "s");
        CHECK_FALSE(s.as_int());
        CHECK_FALSE(s.as_double());
        CHECK_FALSE(s.as_bool());
        CHECK_FALSE(s.find("k"));
        CHECK(s.as_string() == "x");
    }
    {
        Value i = field(v, "i");
        CHECK_FALSE(i.as_string());
        CHECK_FALSE(i.as_bool());
        CHECK_FALSE(i.find("k"));
        CHECK(i.as_int() == 7);
    }
    {
        Value d = field(v, "d");
        CHECK_FALSE(d.as_int());
        CHECK_FALSE(d.as_string());
        CHECK_FALSE(d.as_bool());
        CHECK(d.as_double() == 1.5);
    }
    {
        Value t = field(v, "t");
        CHECK_FALSE(t.as_int());
        CHECK_FALSE(t.as_double());
        CHECK_FALSE(t.as_string());
        CHECK(t.as_bool() == true);
    }
    {
        Value n = field(v, "n");
        CHECK_FALSE(n.as_int());
        CHECK_FALSE(n.as_double());
        CHECK_FALSE(n.as_string());
        CHECK_FALSE(n.as_bool());
    }
}

TEST_CASE("a value is read once") {
    Document doc;
    Value v = root(doc, R"({"a":5,"b":6})");
    Value a = field(v, "a");
    CHECK(a.as_int() == 5);
    CHECK_FALSE(a.as_int());
    CHECK_FALSE(a.as_double());
    CHECK(a.type() == Value::Type::Int);
    CHECK(a.raw() == "5");
    CHECK(field(v, "b").as_int() == 6);
}

TEST_CASE("nested objects and arrays") {
    Document doc;
    Value v = root(doc, R"({"args":{"fd":3,"path":"/a","tags":["x","y"],)"
                        R"("inner":{"deep":[1,[2,3],{"k":4}]}}})");
    Value args = field(v, "args");
    Value inner = field(args, "inner");
    Value deep = field(inner, "deep");
    dstd::Vector<std::int64_t> flat;
    int arrays = 0;
    int objects = 0;
    deep.for_each_element([&](Value e) {
        if (e.type() == Value::Type::Array) {
            ++arrays;
            e.for_each_element(
                [&](Value x) { flat.push_back(x.as_int().value_or(-1)); });
        } else if (e.type() == Value::Type::Object) {
            ++objects;
            flat.push_back(field(e, "k").as_int().value_or(-1));
        } else {
            flat.push_back(e.as_int().value_or(-1));
        }
    });
    CHECK(arrays == 1);
    CHECK(objects == 1);
    REQUIRE(flat.size() == 4);
    CHECK(flat[0] == 1);
    CHECK(flat[1] == 2);
    CHECK(flat[2] == 3);
    CHECK(flat[3] == 4);
}

TEST_CASE("find before and after the current position") {
    Document doc;
    Value v = root(doc, R"({"a":1,"b":2,"c":3})");
    CHECK(field(v, "c").as_int() == 3);
    CHECK(field(v, "a").as_int() == 1);
    CHECK(field(v, "b").as_int() == 2);
    CHECK(field(v, "c").as_int() == 3);
}

TEST_CASE("find of a missing key is empty and later finds still work") {
    Document doc;
    Value v = root(doc, R"({"a":1,"b":2})");
    CHECK_FALSE(v.find("zz"));
    CHECK(field(v, "b").as_int() == 2);
    CHECK_FALSE(v.find("zz"));
    CHECK(field(v, "a").as_int() == 1);
}

TEST_CASE("find on an empty object is empty") {
    Document doc;
    Value v = root(doc, "{}");
    CHECK(v.type() == Value::Type::Object);
    CHECK_FALSE(v.find("a"));
    int members = 0;
    v.for_each_member([&](dstd::StringView, Value) { ++members; });
    CHECK(members == 0);
}

TEST_CASE("empty array has no elements") {
    Document doc;
    Value v = root(doc, "[]");
    CHECK(v.type() == Value::Type::Array);
    int elements = 0;
    v.for_each_element([&](Value) { ++elements; });
    CHECK(elements == 0);
}

TEST_CASE("for_each_member visits names and values in order") {
    Document doc;
    Value v = root(doc, R"({"b":2,"a":"x","c":[1]})");
    dstd::String seen;
    v.for_each_member([&](dstd::StringView name, Value member) {
        seen += name;
        seen += ':';
        if (member.type() == Value::Type::Int)
            seen += dstd::to_string(*member.as_int());
        else if (member.type() == Value::Type::String)
            seen += *member.as_string();
        else
            seen += '?';
        seen += ';';
    });
    CHECK(seen == "b:2;a:x;c:?;");
}

TEST_CASE("the root restarts for a second visit and for find after a visit") {
    Document doc;
    Value v = root(doc, R"({"a":1,"b":2})");
    int visits = 0;
    v.for_each_member([&](dstd::StringView, Value) { ++visits; });
    v.for_each_member([&](dstd::StringView, Value) { ++visits; });
    CHECK(visits == 4);
    CHECK(field(v, "b").as_int() == 2);
}

TEST_CASE("a child is visited once") {
    Document doc;
    Value v = root(doc, R"({"o":{"a":1,"b":2}})");
    Value o = field(v, "o");
    int visits = 0;
    o.for_each_member([&](dstd::StringView, Value) { ++visits; });
    o.for_each_member([&](dstd::StringView, Value) { ++visits; });
    CHECK(visits == 2);
    CHECK_FALSE(o.find("a"));
}

TEST_CASE("for_each_member after find: the root visits all, a child none") {
    Document doc;
    Value v = root(doc, R"({"a":1,"b":2,"o":{"x":1,"y":2}})");
    CHECK(field(v, "b").as_int() == 2);
    int visits = 0;
    v.for_each_member([&](dstd::StringView, Value) { ++visits; });
    CHECK(visits == 3);
    Value o = field(v, "o");
    CHECK(field(o, "y").as_int() == 2);
    int inner = 0;
    o.for_each_member([&](dstd::StringView, Value) { ++inner; });
    CHECK(inner == 0);
}

TEST_CASE("a missed find then for_each_member and then find") {
    Document doc;
    Value v = root(doc, R"({"a":1,"b":2,"c":3,"o":{"x":1,"y":2}})");
    CHECK_FALSE(v.find("zz"));
    int visits = 0;
    v.for_each_member([&](dstd::StringView, Value) { ++visits; });
    CHECK(visits == 4);
    CHECK_FALSE(v.find("zz"));
    CHECK(field(v, "c").as_int() == 3);
    Value o = field(v, "o");
    CHECK_FALSE(o.find("zz"));
    CHECK(field(o, "x").as_int() == 1);
    int inner = 0;
    o.for_each_member([&](dstd::StringView, Value) { ++inner; });
    CHECK(inner == 0);
}

TEST_CASE("the root reads again after any read") {
    Document doc;
    Value v = root(doc, R"([1,2,3])");
    std::int64_t sum = 0;
    for (int i = 0; i < 3; ++i)
        v.for_each_element([&](Value e) { sum += e.as_int().value_or(0); });
    CHECK(sum == 18);
}

TEST_CASE("type and raw work on a value that was read") {
    Document doc;
    Value v = root(doc, R"({"i":5,"s":"x","t":true,"o":{"a":1},"l":[1]})");
    Value i = field(v, "i");
    CHECK(i.as_int() == 5);
    CHECK(i.type() == Value::Type::Int);
    CHECK(i.raw() == "5");
    Value t = field(v, "t");
    CHECK(t.as_bool() == true);
    CHECK(t.type() == Value::Type::Bool);
    Value o = field(v, "o");
    o.for_each_member([](dstd::StringView, Value) {});
    CHECK(o.type() == Value::Type::Object);
    CHECK(o.raw() == R"({"a":1})");
    Value l = field(v, "l");
    CHECK(l.find("a") == dstd::nullopt);
    CHECK(l.raw() == "[1]");
}

TEST_CASE("raw excludes the whitespace around a value") {
    Document doc;
    Value v = root(doc, R"(  { "a" :  12  , "b" : "s p" ,"c": [ 1 , 2 ] ,)"
                        R"( "d" : { "k" : 1 } , "e" : true  }  )");
    CHECK(v.raw().front() == '{');
    CHECK(v.raw().back() == '}');
    CHECK(field(v, "a").raw() == "12");
    CHECK(field(v, "b").raw() == R"("s p")");
    CHECK(field(v, "c").raw() == "[ 1 , 2 ]");
    CHECK(field(v, "d").raw() == R"({ "k" : 1 })");
    CHECK(field(v, "e").raw() == "true");
    Value arr = root(doc, "[12 ,  3 ]");
    int seen = 0;
    arr.for_each_element([&](Value e) {
        CHECK(e.raw() == (seen == 0 ? "12" : "3"));
        ++seen;
    });
    CHECK(seen == 2);
}

TEST_CASE("one field read many times, plain and escaped") {
    Document doc;
    Value v =
        root(doc, "{\"p\":\"plain text\",\"e\":\"line\\none\\u00e9\",\"k\":1}");
    dstd::StringView first_plain;
    dstd::StringView first_escaped;
    for (int i = 0; i < 10000; ++i) {
        auto p = v.find("p");
        REQUIRE(p.has_value());
        auto ps = p->as_string();
        REQUIRE(ps.has_value());
        CHECK(*ps == "plain text");
        auto e = v.find("e");
        REQUIRE(e.has_value());
        auto es = e->as_string();
        REQUIRE(es.has_value());
        CHECK(*es == "line\none\xc3\xa9");
        if (i == 0) {
            first_plain = *ps;
            first_escaped = *es;
        }
        CHECK(p->as_string() == "plain text");
    }
    CHECK(first_plain == "plain text");
    CHECK(first_escaped == "line\none\xc3\xa9");
}

TEST_CASE("decoded views stay valid while later strings are decoded") {
    Document doc;
    dstd::String line = "{";
    for (int i = 0; i < 400; ++i) {
        if (i) line += ',';
        line += "\"k" + dstd::to_string(i) + "\":\"v\\n" + dstd::to_string(i) +
                dstd::String(500, 'x') + "\"";
    }
    line += "}";
    Value v = root(doc, line);
    dstd::Vector<dstd::StringView> views;
    for (int i = 0; i < 400; ++i) {
        auto s = field(v, "k" + dstd::to_string(i)).as_string();
        REQUIRE(s.has_value());
        views.push_back(*s);
    }
    for (int i = 0; i < 400; ++i) {
        const dstd::String want =
            "v\n" + dstd::to_string(i) + dstd::String(500, 'x');
        CHECK(views[static_cast<std::size_t>(i)] == want);
    }
}

TEST_CASE("member names decode and are read many times") {
    Document doc;
    Value v = root(doc, R"({"a\tb":1,"c":2})");
    for (int i = 0; i < 2000; ++i) {
        dstd::String names;
        v.for_each_member([&](dstd::StringView n, Value) {
            names += n;
            names += ';';
        });
        CHECK(names == "a\tb;c;");
    }
}

TEST_CASE("escaped strings decode") {
    Document doc;
    Value v = root(
        doc, "{\"s\":\"a\\\"b\\\\c\\nd\\u00e9\",\"k\\\"ey\":1,\"\\u0041\":2}");
    CHECK(field(v, "s").as_string() == "a\"b\\c\nd\xc3\xa9");
    CHECK(field(v, R"(k\"ey)").as_int() == 1);
    CHECK(field(v, "\\u0041").as_int() == 2);
    dstd::String names;
    Value again = root(doc, "{\"k\\\"ey\":1,\"\\u0041\":2}");
    again.for_each_member([&](dstd::StringView name, Value) { names += name; });
    CHECK(names == "k\"eyA");
}

TEST_CASE("numbers at the int64 and uint64 limits") {
    Document doc;
    Value v =
        root(doc, R"({"max":9223372036854775807,"min":-9223372036854775808,)"
                  R"("past":9223372036854775808,"umax":18446744073709551615,)"
                  R"("exp":1e3,"frac":-0.25})");
    CHECK(field(v, "max").as_int() == std::numeric_limits<std::int64_t>::max());
    CHECK(field(v, "min").as_int() == std::numeric_limits<std::int64_t>::min());
    {
        Value past = field(v, "past");
        CHECK(past.type() == Value::Type::Double);
        CHECK_FALSE(past.as_int());
        CHECK(past.as_double() == 9223372036854775808.0);
    }
    {
        Value umax = field(v, "umax");
        CHECK_FALSE(umax.as_int());
        CHECK(umax.as_double() == 18446744073709551615.0);
    }
    CHECK(field(v, "exp").as_double() == 1000.0);
    CHECK(field(v, "frac").as_double() == -0.25);
    CHECK(field(v, "max").as_double() ==
          static_cast<double>(std::numeric_limits<std::int64_t>::max()));
}

TEST_CASE("invalid JSON is a parse error") {
    Document doc;
    for (const char* bad :
         {"", "not json", "{", R"({"a":)", R"({"a":"x)", "[1,", "}"}) {
        auto parsed = doc.parse(bad);
        CHECK_MESSAGE(!parsed.has_value(), bad);
    }
    auto good = doc.parse(R"({"a":1})");
    REQUIRE(good.has_value());
    CHECK(field(*good, "a").as_int() == 1);
}

TEST_CASE("a bare scalar line is a parse error and an array is a value") {
    Document doc;
    for (const char* scalar : {"42", R"("text")", "null", "true", "-1.5"})
        CHECK_MESSAGE(!doc.parse(scalar).has_value(), scalar);
    Value arr = root(doc, "[1,2]");
    CHECK(arr.type() == Value::Type::Array);
    CHECK_FALSE(arr.find("a"));
    std::int64_t sum = 0;
    arr.for_each_element([&](Value e) { sum += e.as_int().value_or(0); });
    CHECK(sum == 3);
}

TEST_CASE("a line longer than the first buffer parses") {
    Document doc;
    Value small = root(doc, R"({"a":1})");
    CHECK(field(small, "a").as_int() == 1);
    dstd::String big = R"({"pad":")";
    big.append(300000, 'x');
    big += R"(","tail":9})";
    Value v = root(doc, big);
    CHECK(field(v, "tail").as_int() == 9);
    Value again = root(doc, big);
    CHECK(field(again, "pad").as_string()->size() == 300000);
    Value last = root(doc, R"({"a":2})");
    CHECK(field(last, "a").as_int() == 2);
}

TEST_CASE("reuse across many parses gives independent answers") {
    Document doc;
    for (int i = 0; i < 2000; ++i) {
        const dstd::String line = R"({"id":)" + dstd::to_string(i) +
                                  R"(,"name":"n)" + dstd::to_string(i) +
                                  R"(","args":{"v":)" + dstd::to_string(i * 2) +
                                  "}}";
        Value v = root(doc, line);
        CHECK(field(v, "id").as_int() == i);
        Value args = field(v, "args");
        CHECK(field(args, "v").as_int() == i * 2);
        CHECK(field(v, "name").as_string() == "n" + dstd::to_string(i));
    }
}

TEST_CASE("raw gives the JSON text") {
    Document doc;
    const dstd::String line = R"({"a":{"b":[1, 2]},"c":"x"})";
    Value v = root(doc, line);
    CHECK(v.raw() == line);
    CHECK(v.raw() == line);
    CHECK(field(v, "c").as_string() == "x");
    Value a = field(v, "a");
    CHECK(a.raw() == R"({"b":[1, 2]})");
    CHECK(field(v, "c").raw() == R"("x")");
}

TEST_CASE("a Document moves with its values") {
    Document first;
    Value v = root(first, R"({"a":1})");
    Document second = std::move(first);
    CHECK(field(v, "a").as_int() == 1);
    Value w = root(second, R"({"a":2})");
    CHECK(field(w, "a").as_int() == 2);
}

TEST_CASE(
    "a syntax error in a read part empties later reads and never crashes") {
    Document doc;
    Value v = root(doc, R"({"dur":,"name":"read"})");
    CHECK_FALSE(v.find("name").has_value());
    CHECK_FALSE(v.find("name").has_value());
    int members = 0;
    v.for_each_member([&](dstd::StringView, Value) { ++members; });
    CHECK(members == 0);

    Value arr = root(doc, R"([1,2,,3])");
    std::int64_t sum = 0;
    arr.for_each_element([&](Value e) { sum += e.as_int().value_or(0); });
    arr.for_each_element([&](Value e) { sum += e.as_int().value_or(0); });
    CHECK(sum <= 3);

    Value good = root(doc, R"({"name":"read"})");
    CHECK(field(good, "name").as_string() == "read");
}

TEST_CASE("a syntax error after the field that is read does not matter") {
    Document doc;
    Value v = root(doc, R"({"name":"read","dur":})");
    CHECK(field(v, "name").as_string() == "read");
    auto dur = v.find("dur");
    REQUIRE(dur.has_value());
    CHECK_FALSE(dur->as_int().has_value());
}

TEST_CASE("a find on the root after a child walked a malformed part is safe") {
    Document doc;
    const char* const line =
        R"({"name":"read","args":{"size":4096,"tags"::"a","b"],"x":{"y":1.5}}})";
    for (int round = 0; round < 3; ++round) {
        Value v = root(doc, line);
        auto args = v.find("args");
        REQUIRE(args.has_value());
        args->for_each_member([&](dstd::StringView, Value m) {
            (void)m.as_double();
            (void)m.as_bool();
            m.for_each_element([&](Value e) { (void)e.as_int(); });
        });
        auto tags = v.find("tags");
        CHECK_FALSE(tags.has_value());
        auto name = v.find("name");
        if (name.has_value()) CHECK(name->as_string() == "read");
    }
}

TEST_CASE("a root rewind after nested walks of a malformed line is safe") {
    Document doc;
    Value v = root(doc, R"({"a":["x""y"],"b":{"c":1.5},"d":2})");
    for (int round = 0; round < 3; ++round) {
        v.for_each_member([&](dstd::StringView, Value m) {
            (void)m.type();
            (void)m.as_int();
            (void)m.as_string();
            m.for_each_member(
                [&](dstd::StringView, Value w) { (void)w.as_double(); });
            m.for_each_element([&](Value e) { (void)e.as_string(); });
        });
        json::detail::ValueAccess::rewind(v);
        (void)v.find("d");
        (void)v.find("b");
    }
}

TEST_CASE("a read of the parent inside its own walk comes back empty") {
    Document doc;
    Value v = root(doc, R"({"a":1,"b":2,"c":3})");
    int members = 0;
    int parent_reads = 0;
    v.for_each_member([&](dstd::StringView, Value) {
        ++members;
        if (v.find("a").has_value()) ++parent_reads;
    });
    CHECK(members == 3);
    CHECK(parent_reads == 0);
    CHECK(field(v, "b").as_int() == 2);

    Value arr = root(doc, R"([1,2,3])");
    int elements = 0;
    arr.for_each_element([&](Value) {
        ++elements;
        arr.for_each_element([&](Value) { ++parent_reads; });
    });
    CHECK(elements == 3);
    CHECK(parent_reads == 0);
}

TEST_CASE("a number too large for a double reads as an infinity") {
    Document doc;
    Value v = root(doc, R"({"a":1e400,"b":2,"c":-1e400,"d":1.5E+999,)"
                        R"("e":1.7976931348623159e308,"f":"x"})");
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(field(v, "a").type() == Value::Type::Double);
    CHECK(field(v, "a").as_double() == inf);
    CHECK(field(v, "b").as_int() == 2);
    CHECK(field(v, "c").as_double() == -inf);
    CHECK(field(v, "d").as_double() == inf);
    CHECK(field(v, "e").as_double() == inf);
    CHECK(field(v, "f").as_string() == "x");
}

TEST_CASE("a member walk goes past numbers too large for a double") {
    Document doc;
    Value v = root(doc, R"({"a":1e400,"b":-1e400,"c":3,"d":1e-400})");
    dstd::Vector<double> seen;
    v.for_each_member([&](dstd::StringView, Value m) {
        if (auto d = m.as_double()) seen.push_back(*d);
    });
    REQUIRE(seen.size() == 4);
    CHECK(seen[0] == std::numeric_limits<double>::infinity());
    CHECK(seen[1] == -std::numeric_limits<double>::infinity());
    CHECK(seen[2] == 3.0);
    CHECK(seen[3] == 0.0);
}

TEST_CASE("a number with a bad grammar stays a read error") {
    Document doc;
    for (const char* bad : {"tru", "01", "1x", "-", "1.", "+1", ".5", "NaN",
                            "1e", "1e+", "0x1"}) {
        const dstd::String line = dstd::String(R"({"a":)") + bad + R"(,"b":2})";
        auto parsed = doc.parse(line);
        if (!parsed) continue;
        CAPTURE(bad);
        auto a = parsed->find("a");
        REQUIRE(a.has_value());
        CHECK_FALSE(a->as_double().has_value());
        CHECK_FALSE(a->as_int().has_value());
        const bool reaches_parser =
            bad[0] == '-' || (bad[0] >= '0' && bad[0] <= '9');
        if (reaches_parser) CHECK_FALSE(parsed->find("b").has_value());
    }
}

TEST_CASE("refused_number gives the double of a number simdjson refuses") {
    using json::detail::refused_number;
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(refused_number("1e400") == inf);
    CHECK(refused_number("-1e400") == -inf);
    CHECK(refused_number("1.5E+999") == inf);
    CHECK(refused_number("-0.5e400") == -inf);
    CHECK(refused_number("1e99999999999999999999") == inf);
    CHECK(refused_number("1.7976931348623159e308") == inf);
    CHECK(refused_number("1e-99999999999999999999") == 0.0);
    const auto negative_zero = refused_number("-1e-99999999999999999999");
    REQUIRE(negative_zero.has_value());
    CHECK(*negative_zero == 0.0);
    CHECK(std::signbit(*negative_zero));
    for (const char* bad :
         {"", "tru", "01", "1x", "-", "1.", "+1", ".5", "NaN", "1e", "1e+",
          "1e400x", "--1e400", "1e4.0", "0x1", "1.5", "42"}) {
        CAPTURE(bad);
        CHECK_FALSE(refused_number(bad).has_value());
    }
}

TEST_CASE("a string with an invalid escape reads as its raw text") {
    Document doc;
    Value v = root(doc, R"({"a":"x\qy","b":2,"c":"\ud800","d":"\udc00",)"
                        R"("e":"\u12","f":"ok"})");
    CHECK(field(v, "a").as_string() == R"(x\qy)");
    CHECK(field(v, "b").as_int() == 2);
    CHECK(field(v, "c").as_string() == R"(\ud800)");
    CHECK(field(v, "d").as_string() == R"(\udc00)");
    CHECK(field(v, "e").as_string() == R"(\u12)");
    CHECK(field(v, "f").as_string() == "ok");
}

TEST_CASE("a name with an invalid escape is its raw text") {
    Document doc;
    Value v = root(doc, R"({"k\q":1,"l\ud800":2,"m":3})");
    dstd::String names;
    int sum = 0;
    v.for_each_member([&](dstd::StringView name, Value m) {
        names += name;
        names += '|';
        sum += static_cast<int>(m.as_int().value_or(0));
    });
    CHECK(names == R"(k\q|l\ud800|m|)");
    CHECK(sum == 6);
    Value again = root(doc, R"({"k\q":1,"m":3})");
    CHECK(field(again, R"(k\q)").as_int() == 1);
    CHECK(field(again, "m").as_int() == 3);
}

TEST_CASE("an unterminated string or a cropped line is still an error") {
    Document doc;
    for (const char* bad :
         {R"({"a":"x)", R"({"a":"x\q)", R"({"a":"x\)", R"({"a\q":1,"b":"y)"}) {
        CAPTURE(bad);
        CHECK_FALSE(doc.parse(bad).has_value());
    }
    for (const char* cropped : {R"({"a":)", R"({"a\q":1,"b":)"}) {
        CAPTURE(cropped);
        auto parsed = doc.parse(cropped);
        if (!parsed) continue;
        auto b = parsed->find("b");
        if (b) CHECK_FALSE(b->as_int().has_value());
    }
}

TEST_CASE(
    "an integer wider than 64 bits reads as a double, +inf past 308 digits") {
    Document doc;
    const dstd::String d308(308, '9');
    const dstd::String d309(309, '9');
    const dstd::String line =
        R"({"a":18446744073709551616,"b":99999999999999999999,)"
        R"("c":)" +
        d308 + R"(,"d":)" + d309 + R"(,"e":-)" + d309 + R"(,"f":2})";
    Value v = root(doc, line);
    CHECK(field(v, "a").as_double() == 18446744073709551616.0);
    CHECK(field(v, "b").as_double() == 1e20);
    CHECK(field(v, "c").as_double() == doctest::Approx(1e308).epsilon(1e-9));
    CHECK(field(v, "d").as_double() == std::numeric_limits<double>::infinity());
    CHECK(field(v, "e").as_double() ==
          -std::numeric_limits<double>::infinity());
    CHECK(field(v, "f").as_int() == 2);
    CHECK(field(v, "a").type() == Value::Type::Double);
}

TEST_CASE("get_double_or_refused agrees with as_double on wide integers") {
    const dstd::String d309(309, '9');
    for (const dstd::String& token :
         {dstd::String("18446744073709551616"),
          dstd::String("99999999999999999999"), d309, "-" + d309,
          dstd::String("1e400")}) {
        const dstd::String line = R"({"a":)" + token + "}";
        Document doc;
        Value v = root(doc, line);
        const auto expected = field(v, "a").as_double();
        simdjson::ondemand::parser parser;
        const simdjson::padded_string padded(line);
        auto document = parser.iterate(padded);
        auto object = document.get_object();
        auto value = object.find_field_unordered("a");
        auto inner = value.value_unsafe();
        const auto got = json::detail::get_double_or_refused(inner);
        REQUIRE(expected.has_value());
        REQUIRE(got.has_value());
        CHECK(*got == *expected);
    }
}

TEST_CASE(
    "a line with bad UTF-8 or a raw control byte in a string keeps its "
    "fields") {
    Document doc;
    struct Case {
        dstd::String line;
        dstd::String a;
    };
    const Case cases[] = {
        {dstd::String("{\"a\":\"\xff\",\"b\":1}"), "?"},
        {dstd::String("{\"a\":\"x\xff\xfey\",\"b\":1}"), "x??y"},
        {dstd::String("{\"a\":\"ab\xff\",\"b\":1}"), "ab?"},
        {dstd::String("{\"a\":\"\xe2\x82\",\"b\":1}"), "??"},
        {dstd::String("{\"a\":\"\xe2\x82\xac\xe2\x82\",\"b\":1}"),
         dstd::String("\xe2\x82\xac??")},
        {dstd::String("{\"a\":\"\xc0\xaf\",\"b\":1}"), "??"},
        {dstd::String("{\"a\":\"\xed\xa0\x80\",\"b\":1}"), "???"},
        {dstd::String("{\"a\":\"x\ty\",\"b\":1}"), "x?y"},
        {dstd::String("{\"a\":\"x\ny\",\"b\":1}"), "x?y"},
        {dstd::String("{\"a\":\"\x01\",\"b\":1}"), "?"},
        {dstd::String("{\"a\":\"x\0y\",\"b\":1}", 17), dstd::String("x?y")},
        {dstd::String("{\"a\":\"ok\xc3\xa9\",\"b\":1}"),
         dstd::String("ok\xc3\xa9")},
    };
    for (const Case& c : cases) {
        INFO(c.line);
        Value v = root(doc, c.line);
        CHECK(field(v, "a").as_string() == c.a);
        CHECK(field(v, "b").as_int() == 1);
    }
}

TEST_CASE("a bad byte after a backslash replaces the backslash too") {
    Document doc;
    struct Case {
        dstd::String line;
        dstd::String a;
    };
    const Case cases[] = {
        {dstd::String("{\"a\":\"x\\\xffy\",\"b\":1}"), "x??y"},
        {dstd::String("{\"a\":\"x\\\ty\",\"b\":1}"), "x??y"},
        {dstd::String("{\"a\":\"x\\\x01y\",\"b\":1}"), "x??y"},
        {dstd::String("{\"a\":\"x\\\xe2\x82\",\"b\":1}"), "x???"},
        {dstd::String("{\"a\":\"x\\\\\xffy\",\"b\":1}"), dstd::String("x\\?y")},
        {dstd::String("{\"a\":\"x\\\\\ty\",\"b\":1}"), dstd::String("x\\?y")},
        {dstd::String("{\"a\":\"x\\\\\\\xffy\",\"b\":1}"),
         dstd::String("x\\??y")},
        {dstd::String("{\"a\":\"x\\ny\\\xff\",\"b\":1}"),
         dstd::String("x\ny??")},
    };
    for (const Case& c : cases) {
        INFO(c.line);
        Value v = root(doc, c.line);
        CHECK(field(v, "a").as_string() == c.a);
        CHECK(field(v, "b").as_int() == 1);
    }
}

TEST_CASE("bad bytes at the start, middle and end of a line are repaired") {
    Document doc;
    const dstd::String lines[] = {
        dstd::String("{\"a\":\"\xff\"}"),
        dstd::String("{\"p\":1,\"a\":\"\xff\",\"q\":2}"),
        dstd::String("{\"p\":1,\"q\":2,\"a\":\"\xff\"}"),
        dstd::String("{\"a\":\"\t\"}"),
        dstd::String("{\"p\":1,\"a\":\"\t\",\"q\":2}"),
        dstd::String("{\"p\":1,\"q\":2,\"a\":\"\t\"}"),
    };
    for (const dstd::String& line : lines) {
        INFO(line);
        Value v = root(doc, line);
        CHECK(field(v, "a").as_string() == "?");
        auto again = root(doc, line);
        int members = 0;
        again.for_each_member([&](dstd::StringView, Value) { ++members; });
        CHECK(members >= 1);
    }
}

TEST_CASE(
    "a name with a raw control byte or bad UTF-8 reads as its repaired text") {
    Document doc;
    Value v = root(doc, dstd::String("{\"a\tb\":1,\"c\xff\":2,\"d\":3}"));
    dstd::String names;
    v.for_each_member([&](dstd::StringView name, Value m) {
        names += name;
        names += '|';
        (void)m.as_int();
    });
    CHECK(names == "a?b|c?|d|");
    Value again = root(doc, dstd::String("{\"a\tb\":1,\"d\":3}"));
    CHECK(field(again, "a?b").as_int() == 1);
    CHECK(field(again, "d").as_int() == 3);
}

TEST_CASE(
    "a control byte outside a string, or bad UTF-8 outside one, stays broken") {
    Document doc;
    struct Case {
        dstd::String line;
        bool parses;
    };
    // A line that parses has a document but no readable "b" after the bad
    // byte; the others are refused by the parser.
    const Case cases[] = {
        {dstd::String("{\"a\":1,\x01\"b\":2}"), true},
        {dstd::String("{\"a\":\x01 1,\"b\":2}"), true},
        {dstd::String("{\"a\":1,\xff\"b\":2}"), false},
        {dstd::String("{\"a\":1\xc3,\"b\":2}"), false},
        {dstd::String("{\"a\":\"x\ty"), false},
        {dstd::String("{\"a\":\"\xff"), false},
        {dstd::String("{\"a\":\"x\ty\",\"b\":"), false},
    };
    for (const Case& c : cases) {
        INFO(c.line);
        auto parsed = doc.parse(c.line);
        REQUIRE(parsed.has_value() == c.parses);
        if (!parsed) continue;
        CHECK_FALSE(parsed->find("b").has_value());
    }
}

TEST_CASE("a repaired line leaves the next parse and the views valid") {
    Document doc;
    Value first = root(doc, dstd::String("{\"a\":\"x\ty\",\"b\":\"z\"}"));
    const auto a = field(first, "a").as_string();
    REQUIRE(a.has_value());
    const auto b = field(first, "b").as_string();
    REQUIRE(b.has_value());
    CHECK(*a == "x?y");
    CHECK(*b == "z");
    Value second = root(doc, R"({"a":"plain","b":1})");
    CHECK(field(second, "a").as_string() == "plain");
    Value third = root(doc, dstd::String("{\"a\":\"\xff\"}"));
    CHECK(field(third, "a").as_string() == "?");
    Value fourth = root(doc, R"({"a":"plain"})");
    CHECK(field(fourth, "a").as_string() == "plain");
}
