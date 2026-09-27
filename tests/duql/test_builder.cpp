#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/duql/abi.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/internal/duql_handle.h>
#include <dftracer/utils/duql/query.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <cstring>
#include <string>

using namespace dftracer::utils::duql;
using dftracer::utils::json::JsonValue;

namespace {

struct JsonDoc {
    simdjson::dom::parser parser;
    simdjson::dom::element elem;
    bool valid = false;

    explicit JsonDoc(const char* json) {
        auto result = parser.parse(json, std::strlen(json));
        if (!result.error()) {
            elem = result.value_unsafe();
            valid = true;
        }
    }
    JsonValue root() { return valid ? JsonValue(elem) : JsonValue(); }
};

// Owns a dftu_duql and its serialized string across the assertions.
struct AbiDuql {
    dftu_duql* h;
    explicit AbiDuql(dftu_duql* q) : h(q) {}
    ~AbiDuql() { dftu_duql_free(h); }
    std::string str() const {
        char* s = dftu_duql_to_string(h);
        std::string out = s ? s : "";
        dftu_duql_string_free(s);
        return out;
    }
    const Query& duql() const { return duql_handle_unwrap(h); }
};

}  // namespace

TEST_CASE("C++ fluent Field builder matches parse") {
    // (cat == "POSIX") && (dur > 100), Python-style.
    auto built = ((Field("cat") == "POSIX") && (Field("dur") > 100)).build();
    REQUIRE(built.has_value());
    auto parsed = Query::from_string(R"(cat == "POSIX" and dur > 100)");
    REQUIRE(parsed.has_value());
    CHECK(built->to_string() == parsed->to_string());

    JsonDoc hit(R"({"cat":"POSIX","dur":200})");
    JsonDoc miss(R"({"cat":"POSIX","dur":50})");
    CHECK(built->evaluate(hit.root()));
    CHECK_FALSE(built->evaluate(miss.root()));

    CHECK(Field("name").like("%read%").to_string() ==
          Query::from_string(R"(name like "%read%")")->to_string());
    CHECK(Field("pid").in({1, 2, 3}).to_string() ==
          Query::from_string("pid in [1, 2, 3]")->to_string());
}

TEST_CASE("C++ builder compare matches parse") {
    auto built = field_eq("cat", "POSIX").build();
    REQUIRE(built.has_value());
    auto parsed = Query::from_string(R"(cat == "POSIX")");
    REQUIRE(parsed.has_value());
    CHECK(built->to_string() == parsed->to_string());

    JsonDoc match(R"({"cat":"POSIX"})");
    JsonDoc no_match(R"({"cat":"STDIO"})");
    CHECK(built->evaluate(match.root()) == parsed->evaluate(match.root()));
    CHECK(built->evaluate(no_match.root()) ==
          parsed->evaluate(no_match.root()));
    CHECK(built->evaluate(match.root()));
    CHECK_FALSE(built->evaluate(no_match.root()));
}

TEST_CASE("C++ builder numeric typing round-trips") {
    CHECK(field_gt("dur", 50).to_string() == "dur > 50");
    CHECK(field_lt("off", -5).to_string() == "off < -5");
    CHECK(field_eq("ratio", 1.5).to_string() ==
          Query::from_string("ratio == 1.5")->to_string());
    CHECK(field_eq("ok", true).to_string() == "ok == true");
}

TEST_CASE("C++ builder combinators match parse") {
    auto built = (field_eq("cat", "POSIX") && field_gt("dur", 50)).build();
    REQUIRE(built.has_value());
    auto parsed = Query::from_string(R"(cat == "POSIX" and dur > 50)");
    REQUIRE(parsed.has_value());
    CHECK(built->to_string() == parsed->to_string());

    const char* events[] = {R"({"cat":"POSIX","dur":100})",
                            R"({"cat":"POSIX","dur":10})",
                            R"({"cat":"STDIO","dur":100})"};
    for (const char* ev : events) {
        JsonDoc doc(ev);
        CHECK(built->evaluate(doc.root()) == parsed->evaluate(doc.root()));
    }
}

TEST_CASE("C++ builder or / not match parse") {
    auto built =
        (!(field_eq("cat", "STDIO") || field_eq("cat", "MPI"))).build();
    REQUIRE(built.has_value());
    auto parsed = Query::from_string(R"(not (cat == "STDIO" or cat == "MPI"))");
    REQUIRE(parsed.has_value());
    CHECK(built->to_string() == parsed->to_string());
    JsonDoc doc(R"({"cat":"POSIX"})");
    CHECK(built->evaluate(doc.root()) == parsed->evaluate(doc.root()));
    CHECK(built->evaluate(doc.root()));
}

TEST_CASE("C++ builder in / match round-trip") {
    auto in = field_in("cat", std::vector<std::string>{"POSIX", "STDIO"});
    CHECK(in.to_string() ==
          Query::from_string(R"(cat in ["POSIX", "STDIO"])")->to_string());

    auto ints = field_in("pid", std::vector<std::int64_t>{1, 2, 3});
    CHECK(ints.to_string() ==
          Query::from_string("pid in [1, 2, 3]")->to_string());

    auto like = field_match("name", MatchOp::LIKE, "MPI_%").build();
    REQUIRE(like.has_value());
    auto parsed = Query::from_string(R"(name like "MPI_%")");
    REQUIRE(parsed.has_value());
    CHECK(like->to_string() == parsed->to_string());
    JsonDoc a(R"({"name":"MPI_Send"})");
    JsonDoc b(R"({"name":"POSIX_read"})");
    CHECK(like->evaluate(a.root()) == parsed->evaluate(a.root()));
    CHECK(like->evaluate(b.root()) == parsed->evaluate(b.root()));
    CHECK(like->evaluate(a.root()));
    CHECK_FALSE(like->evaluate(b.root()));
}

TEST_CASE("C ABI builder compare matches parse") {
    AbiDuql built(dftu_duql_cmp_str("cat", DFTU_DUQL_CMP_EQ, "POSIX"));
    REQUIRE(built.h != nullptr);
    AbiDuql parsed(dftu_duql_parse(R"(cat == "POSIX")"));
    REQUIRE(parsed.h != nullptr);
    CHECK(built.str() == parsed.str());

    JsonDoc match(R"({"cat":"POSIX"})");
    JsonDoc no_match(R"({"cat":"STDIO"})");
    CHECK(built.duql().evaluate(match.root()));
    CHECK_FALSE(built.duql().evaluate(no_match.root()));
}

TEST_CASE("C ABI builder numeric and combinators") {
    AbiDuql gt(dftu_duql_cmp_i64("dur", DFTU_DUQL_CMP_GT, 50));
    REQUIRE(gt.h != nullptr);
    CHECK(gt.str() == "dur > 50");

    // dftu_duql_and consumes its arguments.
    AbiDuql both(
        dftu_duql_and(dftu_duql_cmp_str("cat", DFTU_DUQL_CMP_EQ, "POSIX"),
                      dftu_duql_cmp_i64("dur", DFTU_DUQL_CMP_GT, 50)));
    REQUIRE(both.h != nullptr);
    AbiDuql parsed(dftu_duql_parse(R"(cat == "POSIX" and dur > 50)"));
    REQUIRE(parsed.h != nullptr);
    CHECK(both.str() == parsed.str());

    JsonDoc doc(R"({"cat":"POSIX","dur":100})");
    JsonDoc doc2(R"({"cat":"POSIX","dur":10})");
    CHECK(both.duql().evaluate(doc.root()) ==
          parsed.duql().evaluate(doc.root()));
    CHECK(both.duql().evaluate(doc2.root()) ==
          parsed.duql().evaluate(doc2.root()));
    CHECK(both.duql().evaluate(doc.root()));
    CHECK_FALSE(both.duql().evaluate(doc2.root()));
}

TEST_CASE("C ABI builder in / not_in / match") {
    const char* cats[] = {"POSIX", "STDIO"};
    AbiDuql in(dftu_duql_in_str("cat", cats, 2));
    REQUIRE(in.h != nullptr);
    AbiDuql in_parsed(dftu_duql_parse(R"(cat in ["POSIX", "STDIO"])"));
    REQUIRE(in_parsed.h != nullptr);
    CHECK(in.str() == in_parsed.str());

    int64_t pids[] = {1, 2, 3};
    AbiDuql nin(dftu_duql_not_in_i64("pid", pids, 3));
    REQUIRE(nin.h != nullptr);
    AbiDuql nin_parsed(dftu_duql_parse("pid not in [1, 2, 3]"));
    REQUIRE(nin_parsed.h != nullptr);
    CHECK(nin.str() == nin_parsed.str());

    AbiDuql match(dftu_duql_match("name", DFTU_DUQL_MATCH_LIKE, "MPI_%"));
    REQUIRE(match.h != nullptr);
    AbiDuql match_parsed(dftu_duql_parse(R"(name like "MPI_%")"));
    REQUIRE(match_parsed.h != nullptr);
    CHECK(match.str() == match_parsed.str());
    JsonDoc a(R"({"name":"MPI_Send"})");
    CHECK(match.duql().evaluate(a.root()) ==
          match_parsed.duql().evaluate(a.root()));
    CHECK(match.duql().evaluate(a.root()));
}

TEST_CASE("C ABI builder or / not") {
    AbiDuql q(dftu_duql_not(
        dftu_duql_or(dftu_duql_cmp_str("cat", DFTU_DUQL_CMP_EQ, "STDIO"),
                     dftu_duql_cmp_str("cat", DFTU_DUQL_CMP_EQ, "MPI"))));
    REQUIRE(q.h != nullptr);
    AbiDuql parsed(dftu_duql_parse(R"(not (cat == "STDIO" or cat == "MPI"))"));
    REQUIRE(parsed.h != nullptr);
    CHECK(q.str() == parsed.str());
    JsonDoc doc(R"({"cat":"POSIX"})");
    CHECK(q.duql().evaluate(doc.root()));
}

TEST_CASE("C ABI builder null field is safe") {
    CHECK(dftu_duql_cmp_i64(nullptr, DFTU_DUQL_CMP_EQ, 1) == nullptr);
    CHECK(dftu_duql_and(nullptr, nullptr) == nullptr);
}

TEST_CASE("any() builds the same leaf from C++, the C ABI and the DSL") {
    auto built = (Field("tags").any() == "a").build();
    REQUIRE(built.has_value());
    CHECK(built->to_string() == R"(any(tags) == "a")");
    AbiDuql c(dftu_duql_cmp_str("any(tags)", DFTU_DUQL_CMP_EQ, "a"));
    REQUIRE(c.h != nullptr);
    CHECK(c.str() == R"(any(tags) == "a")");
    AbiDuql in(dftu_duql_cmp_i64("any(sizes)", DFTU_DUQL_CMP_GT, 100));
    REQUIRE(in.h != nullptr);
    CHECK(in.str() == "any(sizes) > 100");

    JsonDoc hit(R"({"tags":["b","a"],"sizes":[1,200]})");
    JsonDoc miss(R"({"tags":["b"],"sizes":[1]})");
    CHECK(c.duql().evaluate(hit.root()));
    CHECK_FALSE(c.duql().evaluate(miss.root()));
    CHECK(in.duql().evaluate(hit.root()));
    CHECK_FALSE(in.duql().evaluate(miss.root()));
}
