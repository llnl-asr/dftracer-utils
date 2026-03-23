#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/query/query.h>
#include <doctest/doctest.h>
#include <yyjson.h>

#include <cstring>

using namespace dftracer::utils::utilities::common::query;
using dftracer::utils::utilities::common::json::JsonValue;

namespace {

struct JsonDoc {
    yyjson_doc* doc;
    JsonDoc(const char* json) : doc(yyjson_read(json, std::strlen(json), 0)) {}
    ~JsonDoc() {
        if (doc) yyjson_doc_free(doc);
    }
    JsonValue root() { return JsonValue(yyjson_doc_get_root(doc)); }
};

}  // namespace

TEST_CASE("Query::from_string - valid") {
    auto q = Query::from_string(R"(cat == "POSIX" and dur > 100)");
    REQUIRE(q.has_value());
    CHECK(q->source() == R"(cat == "POSIX" and dur > 100)");
}

TEST_CASE("Query::from_string - invalid") {
    auto q = Query::from_string("cat ==");
    REQUIRE_FALSE(q.has_value());
}

TEST_CASE("Query::evaluate") {
    auto q = Query::from_string(R"(cat == "POSIX" and dur > 50)");
    REQUIRE(q.has_value());

    JsonDoc match(R"({"cat":"POSIX","dur":100})");
    CHECK(q->evaluate(match.root()));

    JsonDoc no_match(R"({"cat":"STDIO","dur":100})");
    CHECK_FALSE(q->evaluate(no_match.root()));
}

TEST_CASE("Query::to_string") {
    auto q = Query::from_string(R"(cat == "POSIX")");
    REQUIRE(q.has_value());
    CHECK(q->to_string() == R"(cat == "POSIX")");
}

TEST_CASE("parse_or_throw - valid") {
    CHECK_NOTHROW(parse_or_throw(R"(cat == "POSIX")"));
}

TEST_CASE("parse_or_throw - invalid throws") {
    CHECK_THROWS_AS(parse_or_throw("cat =="), QueryParseError);
}

TEST_CASE("try_parse - valid") {
    auto q = try_parse(R"(cat == "POSIX")");
    REQUIRE(q.has_value());
}

TEST_CASE("try_parse - invalid returns nullopt") {
    auto q = try_parse("cat ==");
    CHECK_FALSE(q.has_value());
}

TEST_CASE("Query with case-insensitive keywords") {
    auto q = Query::from_string(R"(cat == "POSIX" AND dur > 50)");
    REQUIRE(q.has_value());

    JsonDoc doc(R"({"cat":"POSIX","dur":100})");
    CHECK(q->evaluate(doc.root()));
}

TEST_CASE("Query with NOT IN") {
    auto q = Query::from_string(R"(cat NOT IN ["STDIO", "MPI"])");
    REQUIRE(q.has_value());

    JsonDoc match(R"({"cat":"POSIX"})");
    CHECK(q->evaluate(match.root()));

    JsonDoc no_match(R"({"cat":"STDIO"})");
    CHECK_FALSE(q->evaluate(no_match.root()));
}
