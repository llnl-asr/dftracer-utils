#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/query/evaluator.h>
#include <dftracer/utils/utilities/common/query/parser.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <cstring>

using namespace dftracer::utils::utilities::common::query;
using dftracer::utils::utilities::common::json::JsonValue;

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
