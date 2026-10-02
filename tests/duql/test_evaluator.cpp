#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/numbers.h>
#include <dftracer/utils/duql/parser.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace dftracer::utils::duql;
using dftracer::utils::json::JsonValue;

namespace {

struct JsonDoc {
    simdjson::dom::parser parser;
    simdjson::dom::element elem;
    bool valid = false;

    JsonDoc(const char* json) {
        auto result = parser.parse(json, std::strlen(json));
        if (!result.error()) {
            elem = result.value_unsafe();
            valid = true;
        }
    }
    JsonValue root() { return valid ? JsonValue(elem) : JsonValue(); }
};

bool eval(const char* query_str, const char* json_str) {
    auto ast = parse(query_str);
    REQUIRE(ast.has_value());
    JsonDoc doc(json_str);
    REQUIRE(doc.valid);
    return evaluate(**ast, doc.root());
}

Truth truth(const char* query_str, const char* json_str) {
    auto ast = parse(query_str);
    REQUIRE(ast.has_value());
    JsonDoc doc(json_str);
    REQUIRE(doc.valid);
    return evaluate_truth(**ast, doc.root());
}

Truth truth(const char* query_str, const ValueMap& fields) {
    auto ast = parse(query_str);
    REQUIRE(ast.has_value());
    return evaluate_truth(**ast, fields);
}

bool eval(const char* query_str, const ValueMap& fields) {
    return truth(query_str, fields) == Truth::YES;
}

}  // namespace

TEST_CASE("evaluate - string equality") {
    CHECK(eval(R"(cat == "POSIX")", R"({"cat":"POSIX","dur":100})"));
    CHECK_FALSE(eval(R"(cat == "STDIO")", R"({"cat":"POSIX","dur":100})"));
}

TEST_CASE("evaluate - string inequality") {
    CHECK(eval(R"(cat != "STDIO")", R"({"cat":"POSIX"})"));
    CHECK_FALSE(eval(R"(cat != "POSIX")", R"({"cat":"POSIX"})"));
}

TEST_CASE("evaluate - integer comparison") {
    CHECK(eval("dur > 50", R"({"dur":100})"));
    CHECK_FALSE(eval("dur > 200", R"({"dur":100})"));
    CHECK(eval("dur >= 100", R"({"dur":100})"));
    CHECK(eval("dur < 200", R"({"dur":100})"));
    CHECK(eval("dur <= 100", R"({"dur":100})"));
    CHECK(eval("dur == 100", R"({"dur":100})"));
}

TEST_CASE("evaluate - float comparison") {
    CHECK(eval("score > 3.0", R"({"score":3.14})"));
    CHECK_FALSE(eval("score > 4.0", R"({"score":3.14})"));
}

TEST_CASE("evaluate - boolean comparison") {
    CHECK(eval("active == true", R"({"active":true})"));
    CHECK_FALSE(eval("active == false", R"({"active":true})"));
}

TEST_CASE("evaluate - dotted field path") {
    CHECK(eval(R"(args.level == "DEBUG")", R"({"args":{"level":"DEBUG"}})"));
    CHECK_FALSE(
        eval(R"(args.level == "INFO")", R"({"args":{"level":"DEBUG"}})"));
}

TEST_CASE("evaluate - flat arg key that itself contains dots") {
    // "cqe.raw_ns" is one flat arg member, not a nested {cqe:{raw_ns}}. It
    // resolves both bare and with the explicit args. prefix.
    const char* json = R"({"args":{"cqe.raw_ns":42,"mlx5.op":"SEND"}})";
    CHECK(eval("cqe.raw_ns == 42", json));
    CHECK(eval("args.cqe.raw_ns == 42", json));
    CHECK(eval(R"(mlx5.op == "SEND")", json));
    CHECK(eval(R"(args.mlx5.op == "SEND")", json));
    CHECK_FALSE(eval("cqe.raw_ns == 7", json));
    // A genuinely nested arg still resolves segment by segment.
    CHECK(eval("a.b == 5", R"({"args":{"a":{"b":5}}})"));
}

TEST_CASE("evaluate - missing field returns false") {
    CHECK_FALSE(eval(R"(missing == "x")", R"({"cat":"POSIX"})"));
    CHECK_FALSE(eval("missing > 0", R"({"cat":"POSIX"})"));
}

TEST_CASE("evaluate - type mismatch returns false") {
    CHECK_FALSE(eval("cat > 100", R"({"cat":"POSIX"})"));
    CHECK_FALSE(eval(R"(dur == "hello")", R"({"dur":100})"));
}

TEST_CASE("evaluate - in operator") {
    CHECK(eval(R"(cat in ["POSIX", "STDIO"])", R"({"cat":"POSIX"})"));
    CHECK(eval(R"(cat in ["POSIX", "STDIO"])", R"({"cat":"STDIO"})"));
    CHECK_FALSE(eval(R"(cat in ["POSIX", "STDIO"])", R"({"cat":"MPI"})"));
}

TEST_CASE("evaluate - not in operator") {
    CHECK(eval(R"(cat not in ["POSIX", "STDIO"])", R"({"cat":"MPI"})"));
    CHECK_FALSE(eval(R"(cat not in ["POSIX", "STDIO"])", R"({"cat":"POSIX"})"));
}

TEST_CASE("evaluate - and") {
    CHECK(
        eval(R"(cat == "POSIX" and dur > 50)", R"({"cat":"POSIX","dur":100})"));
    CHECK_FALSE(eval(R"(cat == "POSIX" and dur > 200)",
                     R"({"cat":"POSIX","dur":100})"));
}

TEST_CASE("evaluate - or") {
    CHECK(eval(R"(cat == "POSIX" or cat == "STDIO")", R"({"cat":"POSIX"})"));
    CHECK(eval(R"(cat == "POSIX" or cat == "STDIO")", R"({"cat":"STDIO"})"));
    CHECK_FALSE(
        eval(R"(cat == "POSIX" or cat == "STDIO")", R"({"cat":"MPI"})"));
}

TEST_CASE("evaluate - not") {
    CHECK(eval(R"(not cat == "POSIX")", R"({"cat":"STDIO"})"));
    CHECK_FALSE(eval(R"(not cat == "POSIX")", R"({"cat":"POSIX"})"));
}

TEST_CASE("evaluate - complex nested query") {
    const char* json = R"({"cat":"POSIX","dur":500,"name":"read"})";
    CHECK(eval(R"((cat == "POSIX" and dur > 100) or name == "write")", json));
    CHECK_FALSE(
        eval(R"((cat == "STDIO" and dur > 100) or name == "write")", json));
}

TEST_CASE("evaluate - integer cross-type comparison") {
    // JSON uint vs query int64_t
    CHECK(eval("pid == 1234", R"({"pid":1234})"));
    // Negative query literal vs positive JSON value
    CHECK_FALSE(eval("pid == -1", R"({"pid":1234})"));
}

TEST_CASE("evaluate - any over array elements") {
    const char* rec = R"({"tags":["a","b"],"sizes":[10,4096],"none":[]})";
    CHECK(eval(R"(any(tags) == "a")", rec));
    CHECK_FALSE(eval(R"(any(tags) == "c")", rec));
    CHECK(eval(R"(any(tags) != "a")", rec));
    CHECK_FALSE(eval(R"(not any(tags) == "a")", rec));
    CHECK(eval("any(sizes) > 1000", rec));
    CHECK_FALSE(eval("any(sizes) > 5000", rec));
    CHECK(eval(R"(any(tags) in ["x", "b"])", rec));
    CHECK(eval(R"(any(tags) not in ["a"])", rec));
    CHECK(eval(R"(any(tags) like "b%")", rec));
    CHECK_FALSE(eval(R"(any(none) == "a")", rec));
    CHECK_FALSE(eval(R"(any(missing) == "a")", rec));
    // A value that is not an array, and object elements, match nothing.
    CHECK_FALSE(eval(R"(any(tags) == "a")", R"({"tags":"a"})"));
    CHECK_FALSE(eval("any(items) == 1", R"({"items":[{"id":1}]})"));
}

TEST_CASE("evaluate - any over flattened positions in a value map") {
    auto ast = parse(R"(any(tags) == "b")");
    REQUIRE(ast.has_value());
    ValueMap m;
    m["tags.0"] = std::string("a");
    m["tags.1"] = std::string("b");
    m["tagsx.0"] = std::string("b");
    CHECK(evaluate(**ast, m));
    ValueMap other;
    other["tagsx.0"] = std::string("b");
    other["tags.name"] = std::string("b");
    CHECK_FALSE(evaluate(**ast, other));
    auto args = parse("any(args.sizes) > 100");
    REQUIRE(args.has_value());
    ValueMap bare;
    bare["sizes.3"] = std::int64_t{4096};
    CHECK(evaluate(**args, bare));
}

TEST_CASE("spec - empty string is a value") {
    CHECK(eval(R"(x == "")", R"({"x":""})"));
    CHECK(truth(R"(x != "")", R"({"x":""})") == Truth::NO);
    ValueMap m;
    m["x"] = std::string();
    CHECK(eval(R"(x == "")", m));
    CHECK(truth(R"(x != "")", m) == Truth::NO);
}

TEST_CASE("spec - negation does not select missing or null fields") {
    const char* records[] = {R"({"x":1})", R"({"x":2})", "{}", R"({"x":null})"};
    const bool expected[] = {false, true, false, false};
    for (const char* q : {"not (x == 1)", "x != 1"}) {
        CAPTURE(q);
        for (int i = 0; i < 4; ++i) {
            CAPTURE(records[i]);
            CHECK(eval(q, records[i]) == expected[i]);
        }
        CHECK(truth(q, "{}") == Truth::UNKNOWN);
        CHECK(truth(q, R"({"x":null})") == Truth::UNKNOWN);

        ValueMap one, two, absent;
        one["x"] = std::int64_t{1};
        two["x"] = std::int64_t{2};
        CHECK_FALSE(eval(q, one));
        CHECK(eval(q, two));
        CHECK(truth(q, absent) == Truth::UNKNOWN);
    }
}

TEST_CASE("spec - unknown under or") {
    CHECK(eval("x == 1 or y == 5", R"({"y":5})"));
    CHECK(truth("x == 1 or y == 6", R"({"y":5})") == Truth::UNKNOWN);
    CHECK(truth("x == 1 and y == 6", R"({"y":5})") == Truth::NO);
    ValueMap m;
    m["y"] = std::int64_t{5};
    CHECK(eval("x == 1 or y == 5", m));
    CHECK(truth("x == 1 or y == 6", m) == Truth::UNKNOWN);
    CHECK(truth("x == 1 and y == 6", m) == Truth::NO);
}

TEST_CASE("spec - type mismatch fails a comparison and its negation") {
    const char* rec = R"({"cat":"POSIX"})";
    CHECK(truth("cat > 100", rec) == Truth::UNKNOWN);
    CHECK(truth("not (cat > 100)", rec) == Truth::UNKNOWN);
    CHECK_FALSE(eval("cat > 100", rec));
    CHECK_FALSE(eval("not (cat > 100)", rec));
    ValueMap m;
    m["cat"] = std::string("POSIX");
    CHECK(truth("cat > 100", m) == Truth::UNKNOWN);
    CHECK(truth("not (cat > 100)", m) == Truth::UNKNOWN);
}

TEST_CASE("spec - any() over a missing, non-array or non-matching array") {
    CHECK(truth(R"(any(tags) == "a")", "{}") == Truth::UNKNOWN);
    CHECK(truth(R"(any(tags) == "a")", R"({"tags":null})") == Truth::UNKNOWN);
    CHECK(truth(R"(any(tags) == "a")", R"({"tags":"a"})") == Truth::UNKNOWN);
    CHECK(truth(R"(any(tags) == "a")", R"({"tags":["b"]})") == Truth::NO);
    CHECK(truth(R"(any(tags) == "a")", R"({"tags":[]})") == Truth::NO);
    CHECK_FALSE(eval(R"(not (any(tags) == "a"))", "{}"));
    CHECK(eval(R"(not (any(tags) == "a"))", R"({"tags":["b"]})"));

    ValueMap absent, b;
    b["tags.0"] = std::string("b");
    CHECK(truth(R"(any(tags) == "a")", absent) == Truth::UNKNOWN);
    CHECK_FALSE(eval(R"(not (any(tags) == "a"))", absent));
    CHECK(truth(R"(any(tags) == "a")", b) == Truth::NO);
    CHECK(eval(R"(not (any(tags) == "a"))", b));
}

TEST_CASE("spec - exact numeric comparison across int and double") {
    const char* big = R"({"x":9007199254740993})";
    CHECK_FALSE(eval("x == 9007199254740992.0", big));
    CHECK(eval("x == 9007199254740993", big));
    CHECK(eval("x > 9007199254740992.0", big));
    CHECK(eval("x == 1.0", R"({"x":1})"));
    CHECK(eval("x == 1", R"({"x":1.0})"));
    CHECK_FALSE(eval("x == 1", R"({"x":1.5})"));

    ValueMap m;
    m["x"] = std::int64_t{9007199254740993};
    CHECK_FALSE(eval("x == 9007199254740992.0", m));
    CHECK(eval("x == 9007199254740993", m));
    ValueMap one;
    one["x"] = std::int64_t{1};
    CHECK(eval("x == 1.0", one));
    ValueMap one_d;
    one_d["x"] = 1.0;
    CHECK(eval("x == 1", one_d));
}

TEST_CASE("spec - args fallback only when the bare name is missing") {
    CHECK(truth("ret == 5", R"({"ret":null,"args":{"ret":5}})") ==
          Truth::UNKNOWN);
    CHECK(eval("ret == 5", R"({"args":{"ret":5}})"));
    CHECK(eval("args.ret == 5", R"({"ret":null,"args":{"ret":5}})"));
}

TEST_CASE("spec - in and not in") {
    CHECK(truth(R"(x in ["a", "b"])", R"({"x":1})") == Truth::UNKNOWN);
    CHECK(truth(R"(x not in ["a", "b"])", R"({"x":1})") == Truth::UNKNOWN);
    CHECK(truth("x in [1, 2]", R"({"x":"1"})") == Truth::UNKNOWN);
    CHECK(truth("x not in [1, 2]", R"({"x":"1"})") == Truth::UNKNOWN);
    CHECK(truth("x not in [1, 2]", "{}") == Truth::UNKNOWN);
    CHECK(truth("x not in [1, 2]", R"({"x":null})") == Truth::UNKNOWN);
    CHECK_FALSE(eval("x not in [1, 2]", "{}"));
    CHECK(eval("x not in [1, 2]", R"({"x":3})"));
    CHECK(truth("x in [1, 2]", R"({"x":3})") == Truth::NO);

    ValueMap i, s, absent;
    i["x"] = std::int64_t{1};
    s["x"] = std::string("1");
    CHECK(truth(R"(x in ["a", "b"])", i) == Truth::UNKNOWN);
    CHECK(truth(R"(x not in ["a", "b"])", i) == Truth::UNKNOWN);
    CHECK(truth("x in [1, 2]", s) == Truth::UNKNOWN);
    CHECK(truth("x not in [1, 2]", absent) == Truth::UNKNOWN);
    CHECK(eval("x in [1, 2]", i));
}

namespace {

// Keeps the records `query` selects, as their indexes in `records`.
std::vector<int> kept(const char* query, std::vector<const char*> records) {
    auto ast = parse(query);
    REQUIRE_MESSAGE(ast.has_value(), query);
    std::vector<int> out;
    for (std::size_t i = 0; i < records.size(); ++i) {
        JsonDoc doc(records[i]);
        REQUIRE(doc.valid);
        if (evaluate(**ast, doc.root())) out.push_back(static_cast<int>(i));
    }
    return out;
}

std::string compile_error(const char* query) {
    auto ast = parse(query);
    REQUIRE_FALSE(ast.has_value());
    return ast.error().message;
}

}  // namespace

TEST_CASE("expressions - field against field") {
    CHECK(kept("a > b", {R"({"a":3,"b":2})", R"({"a":1,"b":2})",
                         R"({"a":3})"}) == std::vector<int>{0});
}

TEST_CASE("expressions - negative index") {
    CHECK(eval(R"(tags[-1] == "y")", R"({"tags":["x","y"]})"));
    CHECK_FALSE(eval(R"(tags[-3] == "y")", R"({"tags":["x","y"]})"));
    CHECK(eval(R"(a.b[-1].c == 2)", R"({"a":{"b":[{"c":1},{"c":2}]}})"));
}

TEST_CASE("expressions - numbers") {
    CHECK(eval("x / 2 == 3.5", R"({"x":7})"));
    CHECK(eval("x // 2 == 3", R"({"x":7})"));
    CHECK(eval("-x // 2 == -4", R"({"x":7})"));
    CHECK(eval("x % 3 == 2", R"({"x":-7})"));
    CHECK(eval("x % -3 == -2", R"({"x":7})"));
    CHECK(eval("x + y == 3.5", R"({"x":1,"y":2.5})"));
    CHECK(truth("x * 2 > 0", R"({"x":9223372036854775807})") == Truth::UNKNOWN);
    CHECK(eval("x - 1 == 9223372036854775807", R"({"x":9223372036854775808})"));
    CHECK(eval("x // 2.0 == 3.0", R"({"x":7.5})"));
}

TEST_CASE("expressions - division by zero and overflow are unknown") {
    CHECK(truth("x / z > 0", R"({"x":1,"z":0})") == Truth::UNKNOWN);
    CHECK(truth("not (x / z > 0)", R"({"x":1,"z":0})") == Truth::UNKNOWN);
    CHECK(truth("x // z > 0", R"({"x":1,"z":0})") == Truth::UNKNOWN);
    CHECK(truth("x % z > 0", R"({"x":1,"z":0})") == Truth::UNKNOWN);
    CHECK(truth("x * 4 > 0", R"({"x":9223372036854775807})") == Truth::UNKNOWN);
    CHECK(truth("x + 1 > 0", R"({"x":18446744073709551615})") ==
          Truth::UNKNOWN);
    CHECK(truth("x + 1 > 0", R"({"x":"a"})") == Truth::UNKNOWN);
    CHECK(truth("x + 1 > 0", R"({})") == Truth::UNKNOWN);
}

TEST_CASE("expressions - presence tests") {
    const std::vector<const char*> rs = {R"({"x":null})", R"({})",
                                         R"({"x":0})"};
    CHECK(kept("x is null", rs) == std::vector<int>{0});
    CHECK(kept("x is missing", rs) == std::vector<int>{1});
    CHECK(kept("exists(x)", rs) == std::vector<int>{0, 2});
    CHECK(kept("x is not null", rs) == std::vector<int>{1, 2});
    CHECK(kept("x is not missing", rs) == std::vector<int>{0, 2});
    CHECK(kept("not exists(x)", rs) == std::vector<int>{1});
}

TEST_CASE("expressions - coalesce") {
    const std::vector<const char*> rs = {R"({"a":null,"b":2})", R"({"b":3})",
                                         R"({"a":2,"b":9})"};
    CHECK(kept("(a ?? b) == 2", rs) == std::vector<int>{0, 2});
    CHECK(kept("coalesce(a, b) == 2", rs) == std::vector<int>{0, 2});
    CHECK(truth("(a ?? b) == 2", R"({})") == Truth::UNKNOWN);
}

TEST_CASE("expressions - between") {
    const std::vector<const char*> rs = {R"({"x":1})", R"({"x":2})",
                                         R"({"x":3})", R"({"x":4})", R"({})"};
    CHECK(kept("x between 2 and 3", rs) == std::vector<int>{1, 2});
    CHECK(kept("x not between 2 and 3", rs) == std::vector<int>{0, 3});
    CHECK(kept("x between 2 and 3 and x != 3", rs) == std::vector<int>{1});
}

TEST_CASE("expressions - in with expressions") {
    const std::vector<const char*> rs = {R"({"x":1,"a":1})", R"({"x":2,"a":1})",
                                         R"({"x":2})"};
    CHECK(kept("x in [a, a + 5]", rs) == std::vector<int>{0});
    CHECK(kept("x not in [a]", rs) == std::vector<int>{1});
}

TEST_CASE("expressions - functions of the wrong input type are unknown") {
    CHECK(truth(R"(lower(x) == "5")", R"({"x":5})") == Truth::UNKNOWN);
    CHECK(truth(R"(not (lower(x) == "5"))", R"({"x":5})") == Truth::UNKNOWN);
    CHECK(truth("len(x) > 0", R"({"x":5})") == Truth::UNKNOWN);
    CHECK(truth("abs(x) > 0", R"({"x":"a"})") == Truth::UNKNOWN);
    CHECK(truth("sum(x) > 0", R"({"x":[1,"a"]})") == Truth::UNKNOWN);
}

TEST_CASE("expressions - array functions") {
    const char* r = R"({"tags":["a","b"],"n":[1,2.5,3],"e":[]})";
    CHECK(eval(R"(len(tags) == 2 and contains(tags, "b"))", r));
    CHECK(eval(R"(first(tags) == "a" and last(tags) == "b")", r));
    CHECK(eval("sum(n) == 6.5 and min(n) == 1 and max(n) == 3", r));
    CHECK(eval("len(e) == 0 and sum(e) == 0", r));
    CHECK(truth("first(e) == 1", r) == Truth::UNKNOWN);
    CHECK_FALSE(eval(R"(contains(tags, "z"))", r));
    CHECK(eval(R"(len(o) == 2)", R"({"o":{"a":1,"b":2}})"));
}

TEST_CASE("expressions - array and object functions") {
    const char* r =
        R"({"xs":[1,2,3,4],"nest":[[1,2],3,[]],"o":{"b":"y","a":"x"},)"
        R"("s":"a,b,,c","j":"{\"k\": [1, 2]}","bad":"{"})";
    CHECK(eval(R"(json(slice(xs, 1, -1)) == "[2,3]")", r));
    CHECK(eval(R"(json(slice(xs, -2)) == "[3,4]")", r));
    CHECK(eval(R"(json(slice(xs, 9)) == "[]")", r));
    CHECK(eval(R"(json(flatten(nest)) == "[1,2,3]")", r));
    CHECK(eval(R"(first(keys(o)) == "a" and last(keys(o)) == "b")", r));
    CHECK(eval(R"(first(values(o)) == "x" and last(values(o)) == "y")", r));
    CHECK(eval(R"(len(split(s, ",")) == 4 and last(split(s, ",")) == "c")", r));
    CHECK(eval(R"(contains(split(s, ","), ""))", r));
    CHECK(eval("len(json(parse_json(j))) == 11", r));
    CHECK(eval(R"(len(parse_json(j)) == 1 and parse_json("7") == 7)", r));
    CHECK(eval(R"(any(split(s, ","), . == "c"))", r));
    for (const char* q :
         {"slice(s, 0) is null", "flatten(o) is null", "keys(xs) is null",
          "values(s) is null", "parse_json(bad) is null",
          "parse_json(xs) is null", R"(split(s, "") is null)",
          R"(slice(xs, "a") is null)"}) {
        INFO(q);
        CHECK(eval(q, r));
    }
}

TEST_CASE("expressions - computed array index") {
    const char* r = R"({"xs":[10,20,30],"i":2,"f":1.5,"n":null,"s":"x"})";
    CHECK(eval("xs[i] == 30", r));
    CHECK(eval("xs[i - 3] == 30", r));
    CHECK(eval("xs[i - 2] == 10", r));
    CHECK(eval("xs[-i] == 20", r));
    for (const char* q :
         {"xs[i + 1] is null", "xs[-i - 2] is null", "xs[n] is null",
          "xs[missing] is null", "xs[f] is null", "xs[s] is null",
          "s[i] is null", "missing[i] is null"}) {
        INFO(q);
        CHECK(eval(q, r));
    }
    CHECK(truth("xs[i + 1] == 1", r) == Truth::UNKNOWN);
}

TEST_CASE("expressions - array index parameters fold to a literal step") {
    Params p;
    p.emplace("n", LiteralValue{std::int64_t{1}});
    p.emplace("m", LiteralValue{std::int64_t{-1}});
    p.emplace("big", LiteralValue{std::uint64_t{1}});
    const char* r = R"({"xs":["a","b","c"],"o":[{"k":7}]})";
    auto at = [&](const char* q) {
        auto ast = parse(q, p);
        REQUIRE_MESSAGE(ast.has_value(), q);
        JsonDoc doc(r);
        return evaluate(**ast, doc.root());
    };
    CHECK(at(R"(xs[$n] == "b")"));
    CHECK(at(R"(xs[$m] == "c")"));
    CHECK(at(R"(xs[$big] == "b")"));
    CHECK(at("o[$n] is missing"));
    CHECK_FALSE(at(R"(xs[$n] == "a")"));
    p.emplace("bad", LiteralValue{std::string("1")});
    auto bad = parse("xs[$bad] == 1", p);
    REQUIRE_FALSE(bad.has_value());
    CHECK(
        bad.error().message.find("an array index parameter holds an integer") !=
        std::string::npos);
}

TEST_CASE("expressions - list literals") {
    const char* r =
        R"({"name":"read","cat":"POSIX","xs":[1,2,3],"n":null,"i":1})";
    CHECK(eval(R"(contains(["read", "write"], name))", r));
    CHECK_FALSE(eval(R"(contains(["open", "write"], name))", r));
    CHECK(eval("len([1, 2, 3]) == 3 and len([]) == 0", r));
    CHECK(eval("first([cat, name]) == \"POSIX\"", r));
    CHECK(eval("last([cat, name]) == \"read\"", r));
    CHECK(eval(R"(json([cat, name, n, missing, 1.5, true, [1, 2]]) ==
        "[\"POSIX\",\"read\",null,null,1.5,true,[1,2]]")",
               r));
    CHECK(eval("any([1, 2, 3], . > 2)", r));
    CHECK_FALSE(eval("all([1, 2, 3], . > 2)", r));
    CHECK(eval("sum([i, i + 1]) == 3", r));
}

TEST_CASE("expressions - index_of") {
    const char* r = R"({"tags":["b","a","b",null],"xs":[1,2.5,3],"s":"a"})";
    CHECK(eval(R"(index_of(tags, "a") == 1)", r));
    CHECK(eval(R"(index_of(tags, "b") == 0)", r));
    CHECK(eval("index_of(xs, 3) == 2 and index_of(xs, 2.5) == 1", r));
    for (const char* q :
         {R"(index_of(tags, "z") is null)", "index_of(tags, null) is null",
          "index_of(tags, missing) is null", R"(index_of(s, "a") is null)",
          "index_of(missing, 1) is null", R"(index_of(xs, "a") is null)"}) {
        INFO(q);
        CHECK(eval(q, r));
    }
}

TEST_CASE("expressions - sort") {
    const char* r =
        R"({"tags":["b","a","b",null],"n":[3,1.5,2,-1],"e":[],"one":[[1]],)"
        R"("mix":[1,"a"],"b":[true,false],"objs":[{"a":1},{"a":2}],)"
        R"("nulls":[null,null],"recs":[{"k":2,"v":"x"},{"k":1,"v":"y"}],"s":"a"})";
    CHECK(eval(R"(json(sort(tags)) == "[\"a\",\"b\",\"b\",null]")", r));
    CHECK(eval(R"(json(sort(n)) == "[-1,1.5,2,3]")", r));
    CHECK(eval(R"(json(sort(b)) == "[false,true]")", r));
    CHECK(eval(R"(json(sort(e)) == "[]")", r));
    CHECK(eval(R"(json(sort(nulls)) == "[null,null]")", r));
    CHECK(eval(R"(json(sort(one)) == "[[1]]")", r));
    CHECK(eval("first(sort(n)) == -1 and last(sort(n)) == 3", r));
    CHECK(eval("type(sort(mix)) == \"null\"", r));
    CHECK(eval("type(sort(objs)) == \"null\"", r));
    CHECK(eval("type(sort(s)) == \"null\" and type(sort(missing)) == \"null\"",
               r));
    CHECK(eval("len(sort(tags)) == 4 and last(sort(tags)) is null", r));
}

TEST_CASE("expressions - unique") {
    const char* r = R"({"tags":["b","a","b",null,null],"n":[1,1.0,2,1],"e":[],)"
                    R"("o":[{"a":1},{"a":1},{"a":2},[1],[1]],"s":"a"})";
    CHECK(eval(R"(json(unique(tags)) == "[\"b\",\"a\",null]")", r));
    CHECK(eval(R"(json(unique(n)) == "[1,2]")", r));
    CHECK(eval(R"(json(unique(e)) == "[]")", r));
    CHECK(eval(R"(json(unique(o)) == "[{\"a\":1},{\"a\":2},[1]]")", r));
    CHECK(eval("unique(s) is null and unique(missing) is null", r));
}

TEST_CASE("expressions - join") {
    const char* r =
        R"({"tags":["b","a","b",null],"e":[],"n":["a",1],"s":"a","sep":"-"})";
    CHECK(eval(R"(join(tags, ",") == "b,a,b")", r));
    CHECK(eval(R"(join(e, ",") == "")", r));
    CHECK(eval(R"(join(["x"], ",") == "x")", r));
    CHECK(eval(R"(join(tags, "") == "bab")", r));
    CHECK(eval(R"(join(n, ",") is null)", r));
    CHECK(eval(R"(join(s, ",") is null and join(missing, ",") is null)", r));
    Params p;
    p.emplace("sep", LiteralValue{std::string("; ")});
    auto ast = parse("join(tags, $sep) == \"b; a; b\"", p);
    REQUIRE(ast.has_value());
    JsonDoc doc(r);
    CHECK(evaluate(**ast, doc.root()));
    CHECK(compile_error("join(tags, sep) == \"x\"")
              .find("join() takes a string separator") != std::string::npos);
    CHECK(compile_error("join(tags, 1) == \"x\"")
              .find("join() takes a string separator") != std::string::npos);
}

TEST_CASE("expressions - sort is a stage keyword and a function") {
    CHECK(eval(R"(first(sort(xs)) == 1)", R"({"xs":[3,1,2]})"));
    CHECK(kept("first(sort(xs)) == 1", {R"({"xs":[3,1]})", R"({"xs":[2]})"}) ==
          std::vector<int>{0});
}

TEST_CASE("expressions - string functions") {
    const char* r =
        R"({"s":"  Hello World  ","u":"h\u00e9llo","p":"/a/b.txt"})";
    CHECK(eval(R"(trim(s) == "Hello World")", r));
    CHECK(eval(R"(lower(trim(s)) == "hello world")", r));
    CHECK(eval(R"(upper(p) == "/A/B.TXT")", r));
    CHECK(eval(R"(len(u) == 5)", r));
    CHECK(eval(R"(substr(u, 2) == "llo" and substr(u, 2, 1) == "l")", r));
    CHECK(eval(R"(substr(p, 3) == "b.txt")", r));
    CHECK(eval(R"(starts_with(p, "/a") and ends_with(p, ".txt"))", r));
    CHECK(eval(R"(contains(p, "b.t"))", r));
    CHECK(eval(R"(replace(p, "/", ":") == ":a:b.txt")", r));
    CHECK(eval(R"(concat(p, "#", 3) == "/a/b.txt#3")", r));
    CHECK(truth(R"(concat(p, missing_field) == "x")", r) == Truth::UNKNOWN);
}

TEST_CASE("expressions - number functions") {
    const char* r = R"({"x":-2.5,"i":-3})";
    CHECK(eval("abs(x) == 2.5 and abs(i) == 3", r));
    CHECK(eval("floor(x) == -3 and ceil(x) == -2", r));
    CHECK(eval("round(x) == -3 and round(2.345, 2) == 2.35", r));
    CHECK(eval("min(x, i) == -3 and max(x, i, 0) == 0", r));
    CHECK(eval("pow(2, 10) == 1024 and exp(0) == 1 and log(1) == 0", r));
    CHECK(truth("log(i) > 0", r) == Truth::UNKNOWN);
}

TEST_CASE("expressions - type conversions") {
    const char* r =
        R"({"s":"42","f":"2.5","d":3.9,"b":true,"o":{"b":1,"a":2},"z":null})";
    CHECK(eval("int(s) == 42 and int(d) == 3 and int(b) == 1", r));
    CHECK(eval("float(f) == 2.5 and float(s) == 42", r));
    CHECK(eval(R"(string(d) == "3.9" and string(b) == "true")", r));
    CHECK(eval(R"(json(o) == '{"a":2,"b":1}')", r));
    CHECK(eval(R"(json(s) == '"42"' and json(z) == "null")", r));
    CHECK(eval(R"(type(o) == "object" and type(z) == "null")", r));
    CHECK(eval(R"(type(nope) == "missing" and type(d) == "number")", r));
    CHECK(truth(R"(int(o) == 1)", r) == Truth::UNKNOWN);
}

TEST_CASE("expressions - if and case") {
    const char* r = R"({"x":5,"y":null})";
    CHECK(eval(R"(if(x > 3, "big", "small") == "big")", r));
    CHECK(eval(R"(if(y > 3, "big", "small") == "small")", r));
    CHECK(eval(R"(case(x < 3, "a", x < 6, "b", "c") == "b")", r));
}

TEST_CASE("expressions - object against a string is unknown") {
    CHECK(truth(R"(o == '{"a":1}')", R"({"o":{"a":1}})") == Truth::UNKNOWN);
    CHECK(truth(R"(not (o == '{"a":1}'))", R"({"o":{"a":1}})") ==
          Truth::UNKNOWN);
    CHECK(truth(R"(o in ['{"a":1}'])", R"({"o":{"a":1}})") == Truth::UNKNOWN);
}

TEST_CASE("expressions - compile errors") {
    CHECK(compile_error("x == null").find("is null") != std::string::npos);
    CHECK(compile_error("x != null").find("is null") != std::string::npos);
    CHECK(compile_error("x in [1, null]").find("null") != std::string::npos);
    CHECK(compile_error("lenn(x) > 1").find("lenn") != std::string::npos);
    CHECK(compile_error("len(x, y) > 1").find("len") != std::string::npos);
    CHECK(compile_error("case(a, 1) == 1").find("case") != std::string::npos);
    CHECK(compile_error("exists(x + 1)").find("path") != std::string::npos);
    CHECK(compile_error("dur > 1ms").find("as_time") != std::string::npos);
    CHECK(compile_error("now() > 1").find("time roles") != std::string::npos);
    CHECK(compile_error("date_part(ts, \"hour\") > 1").find("needs a time") !=
          std::string::npos);
}

TEST_CASE("expressions - field maps agree with JSON records") {
    ValueMap m;
    m["a"] = std::int64_t{7};
    m["n"] = Cell::null();
    m["tags"] = Cell::json(R"(["x","y"])", true);
    m["o"] = Cell::json(R"({"k":1})", false);
    CHECK(eval("a // 2 == 3", m));
    CHECK(eval("n is null and nope is missing and exists(n)", m));
    CHECK(eval("len(tags) == 2", m));
    CHECK(eval(R"(tags[-1] == "y")", m));
    CHECK(eval(R"(json(o) == '{"k":1}')", m));
    CHECK(truth("n == 1", m) == Truth::UNKNOWN);
    CHECK(truth(R"(tags == "x")", m) == Truth::UNKNOWN);
}

TEST_CASE("quantifiers - any and all truth tables") {
    const char* rec =
        R"({"e":[],"t":[true,true],"m":[true,false],"u":[null,true],)"
        R"("f":[false,null],"s":"x","n":null,"o":{"a":true},"cat":"io",)"
        R"("h":[{"up":false,"k":1},{"up":true,"k":2}],"g":[[1,2],[3]]})";
    const std::vector<std::pair<const char*, Truth>> cases = {
        {"any(e, .)", Truth::NO},
        {"all(e, .)", Truth::YES},
        {"any(t, .)", Truth::YES},
        {"all(t, .)", Truth::YES},
        {"any(m, .)", Truth::YES},
        {"all(m, .)", Truth::NO},
        {"any(u, .)", Truth::YES},
        {"all(u, .)", Truth::UNKNOWN},
        {"any(f, .)", Truth::UNKNOWN},
        {"all(f, .)", Truth::NO},
        {"any(s, .)", Truth::UNKNOWN},
        {"all(n, .)", Truth::UNKNOWN},
        {"any(nope, .)", Truth::UNKNOWN},
        {"all(o, .)", Truth::UNKNOWN},
        {"not all(e, .)", Truth::NO},
        {"not any(f, .)", Truth::UNKNOWN},
        {R"(any(h, .up and ^.cat == "io"))", Truth::YES},
        {R"(all(h, .k > 0 and ^.cat == "io"))", Truth::YES},
        {"all(h, .k > 1)", Truth::NO},
        {"any(h, .nope == 1)", Truth::UNKNOWN},
        {"any(g, .[0] == 3)", Truth::YES},
        {"all(g, len(.) > 0)", Truth::YES},
        {"any(h, .k == 2) and all(t, .)", Truth::YES},
        {"all(t) == true", Truth::YES},
        {"any(h) == 1", Truth::NO},
    };
    for (const auto& [q, want] : cases) {
        CAPTURE(std::string(q));
        CHECK(truth(q, rec) == want);
    }
}

TEST_CASE("quantifiers - a flattened record reads the array cell") {
    ValueMap m;
    m["h"] = Cell::json("[1,5]", true);
    m["cat"] = Cell(std::string("io"));
    CHECK(truth("any(h, . > 3)", m) == Truth::YES);
    CHECK(truth("all(h, . > 3)", m) == Truth::NO);
    CHECK(truth(R"(any(h, . == 1 and ^.cat == "io"))", m) == Truth::YES);
    CHECK(truth("any(x, . > 3)", m) == Truth::UNKNOWN);
}

TEST_CASE("evaluate - regex_replace") {
    CHECK(eval(R"q(regex_replace(name, "(?<op>[a-z]+)64_(\d+)", "${op}#$2")
                   == "open#17")q",
               R"({"name":"open64_17"})"));
    CHECK(eval(R"(regex_replace(path, "/+", "/") == "/a/b/c")",
               R"({"path":"/a//b///c"})"));
    CHECK(eval(R"(regex_replace("ab", "x*", "-") == "-a-b-")", R"({})"));
    CHECK(eval(R"(regex_replace(s, "(a)(b)?", "[$2]") == "[]c")",
               R"({"s":"ac"})"));
    CHECK(eval(R"(regex_replace(s, "z", "y") == "abc")", R"({"s":"abc"})"));
    CHECK(eval(R"(regex_replace(s, "a", "$$") == "$bc")", R"({"s":"abc"})"));
    CHECK(truth(R"(regex_replace(s, "a", "b") == "b")", R"({"s":null})") ==
          Truth::UNKNOWN);
    CHECK(truth(R"(regex_replace(s, "a", "b") == "b")", R"({"s":5})") ==
          Truth::UNKNOWN);
    CHECK(truth(R"(regex_replace(s, "a", "b") == "b")", R"({})") ==
          Truth::UNKNOWN);
}

namespace {

std::optional<Number> parse_number_with_strtod_only(std::string_view text) {
    const char* first = text.data();
    const char* last = first + text.size();
    if (std::int64_t i = 0;
        std::from_chars(first, last, i).ptr == last && !text.empty())
        return Number{i};
    if (std::uint64_t u = 0;
        std::from_chars(first, last, u).ptr == last && !text.empty())
        return Number{u};
    const std::string copy(text);
    char* end = nullptr;
    const double d = std::strtod(copy.c_str(), &end);
    if (!copy.empty() && end == copy.c_str() + copy.size()) return Number{d};
    return std::nullopt;
}

}  // namespace

TEST_CASE("parse_number - same results as strtod on every first character") {
    const std::vector<std::string> inputs = {
        "",
        "0",
        "007",
        "-1.5e3",
        "+5",
        " 5",
        "5 ",
        "\t5",
        "\n5",
        ".5",
        "5.",
        "-.5",
        "1e5",
        "1E5",
        "1e",
        "e5",
        "0x10",
        "0x1p3",
        "inf",
        "-inf",
        "+inf",
        "Infinity",
        "INF",
        "nan",
        "NaN",
        "-nan",
        "nano",
        "abc",
        "f911e308",
        "n/a",
        "i",
        "n",
        "-",
        "+",
        ".",
        "--5",
        "9223372036854775807",
        "9223372036854775808",
        "18446744073709551615",
        "18446744073709551616",
        "-9223372036854775808",
        "-9223372036854775809",
        "1,5",
        ",5",
        "/UqBWr9S",
        "true",
        "null",
        "_1",
    };
    for (const auto& in : inputs) {
        CAPTURE(in);
        const auto want = parse_number_with_strtod_only(in);
        const auto got = parse_number(in);
        REQUIRE(want.has_value() == got.has_value());
        if (!want) continue;
        REQUIRE(want->index() == got->index());
        if (const auto* d = std::get_if<double>(&*want)) {
            const double g = std::get<double>(*got);
            CHECK((g == *d || (g != g && *d != *d)));
        } else {
            CHECK(*want == *got);
        }
    }
    for (int c = 1; c < 256; ++c) {
        const std::string in(1, static_cast<char>(c));
        CAPTURE(c);
        CHECK(parse_number_with_strtod_only(in).has_value() ==
              parse_number(in).has_value());
        const std::string two = in + "5";
        CHECK(parse_number_with_strtod_only(two).has_value() ==
              parse_number(two).has_value());
    }
}
