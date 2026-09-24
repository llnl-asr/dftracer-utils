#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/query/evaluator.h>
#include <dftracer/utils/query/parser.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <cstring>

using namespace dftracer::utils::query;
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
