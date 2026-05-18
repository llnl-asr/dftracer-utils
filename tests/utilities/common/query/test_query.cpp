#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/query/query.h>
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

TEST_CASE("Query::fields - simple equality") {
    auto q = Query::from_string(R"(cat == "POSIX")");
    REQUIRE(q.has_value());
    auto& f = q->fields();
    CHECK(f.size() == 1);
    CHECK(f.count("cat") == 1);
}

TEST_CASE("Query::fields - compound OR") {
    auto q = Query::from_string(R"(pid == 1 or tid == 2)");
    REQUIRE(q.has_value());
    auto& f = q->fields();
    CHECK(f.size() == 2);
    CHECK(f.count("pid") == 1);
    CHECK(f.count("tid") == 1);
}

TEST_CASE("Query::fields - compound AND") {
    auto q = Query::from_string(R"(cat == "POSIX" and dur > 100)");
    REQUIRE(q.has_value());
    auto& f = q->fields();
    CHECK(f.size() == 2);
    CHECK(f.count("cat") == 1);
    CHECK(f.count("dur") == 1);
}

TEST_CASE("Query::fields - NOT query") {
    auto q = Query::from_string(R"(not cat == "STDIO")");
    REQUIRE(q.has_value());
    auto& f = q->fields();
    CHECK(f.size() == 1);
    CHECK(f.count("cat") == 1);
}

TEST_CASE("Query::fields - IN query") {
    auto q = Query::from_string(R"(cat in ["POSIX", "STDIO"])");
    REQUIRE(q.has_value());
    auto& f = q->fields();
    CHECK(f.size() == 1);
    CHECK(f.count("cat") == 1);
}

TEST_CASE("Query::references") {
    auto q = Query::from_string(R"(pid == 1 and dur > 50)");
    REQUIRE(q.has_value());
    CHECK(q->references("pid"));
    CHECK(q->references("dur"));
    CHECK_FALSE(q->references("cat"));
    CHECK_FALSE(q->references("tid"));
}

TEST_CASE("Query::fields - no duplicates for repeated field") {
    auto q = Query::from_string(R"(pid == 1 or pid == 2)");
    REQUIRE(q.has_value());
    auto& f = q->fields();
    CHECK(f.size() == 1);
    CHECK(f.count("pid") == 1);
}
