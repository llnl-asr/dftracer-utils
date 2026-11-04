// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/value.h>
#include <doctest/doctest.h>
#include <duql/eval_json.h>
#include <duql/filter_corpus.h>
#include <simdjson.h>
#include <testing_utilities.h>

#include <cstring>

using namespace dftracer::utils;

using namespace dftracer::utils::duql;

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
    json::JsonValue root() {
        return valid ? json::JsonValue(elem) : json::JsonValue();
    }
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
    CHECK(eval_json(*q, match.root()));

    JsonDoc no_match(R"({"cat":"STDIO","dur":100})");
    CHECK_FALSE(eval_json(*q, no_match.root()));
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
    CHECK_THROWS_AS(parse_or_throw("cat =="), DuqlParseError);
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
    CHECK(eval_json(*q, doc.root()));
}

TEST_CASE("Query with NOT IN") {
    auto q = Query::from_string(R"(cat NOT IN ["STDIO", "MPI"])");
    REQUIRE(q.has_value());

    JsonDoc match(R"({"cat":"POSIX"})");
    CHECK(eval_json(*q, match.root()));

    JsonDoc no_match(R"({"cat":"STDIO"})");
    CHECK_FALSE(eval_json(*q, no_match.root()));
}

TEST_CASE("Query resolves bare nested args fields") {
    // DFTracer nests domain fields under "args"; a bare reference resolves
    // there so nested fields are queryable by bare name.
    JsonDoc doc(R"({"cat":"POSIX","args":{"fhash":"abc","ret":64}})");

    auto qh = Query::from_string(R"(fhash == "abc")");
    REQUIRE(qh.has_value());
    CHECK(eval_json(*qh, doc.root()));

    auto qr = Query::from_string("ret == 64");
    REQUIRE(qr.has_value());
    CHECK(eval_json(*qr, doc.root()));

    // Dotted form still works.
    auto qd = Query::from_string("args.ret == 64");
    REQUIRE(qd.has_value());
    CHECK(eval_json(*qd, doc.root()));

    // Top-level still wins / unaffected.
    auto qc = Query::from_string(R"(cat == "POSIX")");
    REQUIRE(qc.has_value());
    CHECK(eval_json(*qc, doc.root()));

    auto qmiss = Query::from_string(R"(fhash == "zzz")");
    REQUIRE(qmiss.has_value());
    CHECK_FALSE(eval_json(*qmiss, doc.root()));
}

TEST_CASE("Query bare-nested substring/regex on fhash") {
    JsonDoc doc(R"({"cat":"POSIX","args":{"fhash":"deadbeefcafe"}})");
    auto q = Query::from_string(R"('beef' in fhash)");
    REQUIRE(q.has_value());
    CHECK(eval_json(*q, doc.root()));
}

TEST_CASE("Query LIKE / ILIKE") {
    auto q = Query::from_string(R"(name like "MPI_%")");
    REQUIRE(q.has_value());
    JsonDoc a(R"({"name":"MPI_Send"})");
    CHECK(eval_json(*q, a.root()));
    JsonDoc b(R"({"name":"POSIX_read"})");
    CHECK_FALSE(eval_json(*q, b.root()));
    JsonDoc c(R"({"name":"mpi_send"})");
    CHECK_FALSE(eval_json(*q, c.root()));  // LIKE is case-sensitive

    auto qi = Query::from_string(R"(name ilike "mpi_%")");
    REQUIRE(qi.has_value());
    CHECK(eval_json(*qi, a.root()));  // ILIKE matches MPI_Send
}

TEST_CASE("Query regex ~ and ~*") {
    auto q = Query::from_string(R"DSL(name ~ "MPI_(Send|Recv)")DSL");
    REQUIRE(q.has_value());
    JsonDoc a(R"({"name":"MPI_Send"})");
    JsonDoc b(R"({"name":"MPI_Init"})");
    CHECK(eval_json(*q, a.root()));
    CHECK_FALSE(eval_json(*q, b.root()));

    auto qi = Query::from_string(R"(name ~* "send|recv")");
    REQUIRE(qi.has_value());
    JsonDoc c(R"({"name":"MPI_SEND"})");
    CHECK(eval_json(*qi, c.root()));
}

TEST_CASE("Query negated match !~ and not like") {
    auto q = Query::from_string(R"(name !~ "Init")");
    REQUIRE(q.has_value());
    JsonDoc a(R"({"name":"MPI_Send"})");
    JsonDoc b(R"({"name":"MPI_Init"})");
    CHECK(eval_json(*q, a.root()));
    CHECK_FALSE(eval_json(*q, b.root()));
}

TEST_CASE("Query substring 'sub' in field is case-insensitive") {
    auto q = Query::from_string(R"('send' in name or 'recv' in name)");
    REQUIRE(q.has_value());
    JsonDoc a(R"({"name":"MPI_Send"})");
    JsonDoc b(R"({"name":"MPI_RECV"})");
    JsonDoc c(R"({"name":"MPI_Init"})");
    CHECK(eval_json(*q, a.root()));
    CHECK(eval_json(*q, b.root()));
    CHECK_FALSE(eval_json(*q, c.root()));
}

TEST_CASE("Query substring not in") {
    auto q = Query::from_string(R"('init' not in name)");
    REQUIRE(q.has_value());
    JsonDoc a(R"({"name":"MPI_Send"})");
    JsonDoc b(R"({"name":"MPI_Init"})");
    CHECK(eval_json(*q, a.root()));
    CHECK_FALSE(eval_json(*q, b.root()));
}

TEST_CASE("Query match on missing/non-string field is false") {
    auto q = Query::from_string(R"(name ~ "x")");
    REQUIRE(q.has_value());
    JsonDoc missing(R"({"cat":"POSIX"})");
    JsonDoc numeric(R"({"name":42})");
    CHECK_FALSE(eval_json(*q, missing.root()));
    CHECK_FALSE(eval_json(*q, numeric.root()));
}

TEST_CASE("Query match participates in fields()") {
    auto q = Query::from_string(R"('send' in name and cat == "MPI")");
    REQUIRE(q.has_value());
    CHECK(q->references("name"));
    CHECK(q->references("cat"));
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

TEST_CASE("Query copies own their field views and any paths") {
    dstd::Optional<Query> copy;
    {
        auto q = Query::from_string(R"(any(tags) == "a" and cat == "POSIX")");
        REQUIRE(q.has_value());
        copy.emplace(*q);
    }
    CHECK(copy->references("cat"));
    CHECK(copy->references("tags"));
    CHECK(copy->any_paths() == dstd::Vector<dstd::String>{"tags"});
}

TEST_CASE("Query::evaluate keeps only TRUE records") {
    auto q = Query::from_string("not (x == 1)");
    REQUIRE(q.has_value());
    JsonDoc one(R"({"x":1})");
    JsonDoc two(R"({"x":2})");
    JsonDoc missing("{}");
    JsonDoc null_x(R"({"x":null})");
    JsonDoc empty(R"({"x":""})");
    CHECK_FALSE(eval_json(*q, one.root()));
    CHECK(eval_json(*q, two.root()));
    CHECK_FALSE(eval_json(*q, missing.root()));
    CHECK_FALSE(eval_json(*q, null_x.root()));
    CHECK_FALSE(eval_json(*q, empty.root()));

    ValueMap m;
    CHECK_FALSE(q->evaluate(m));
    m["x"] = std::int64_t{2};
    CHECK(q->evaluate(m));
    m["x"] = dstd::String();
    CHECK_FALSE(q->evaluate(m));
}

TEST_CASE("Query::matches agrees with the engine on every filter and event") {
    json::Document doc;
    std::size_t kept = 0;
    std::size_t dropped = 0;
    std::size_t pairs = 0;

    const auto check = [&](const Query& q, const char* event) {
        JsonDoc dom(event);
        REQUIRE(dom.valid);
        const bool expected = eval_json(q, dom.root());
        auto parsed = doc.parse(event);
        REQUIRE(parsed.has_value());
        const bool got = q.matches(*parsed);
        CAPTURE(q.source());
        CAPTURE(event);
        CHECK(got == expected);
        ++pairs;
        (expected ? kept : dropped)++;
    };

    for (const char* text : FILTERS) {
        auto q = Query::from_string(text);
        CAPTURE(text);
        if (!q) continue;
        for (const char* event : EVENTS) check(*q, event);
    }

    const auto golden = dstd::fs::read_file(DUQL_GOLDEN_PATH);
    REQUIRE(golden.has_value());
    dftu_utils_test::LineStream in(*golden);
    simdjson::dom::parser rows;
    std::size_t golden_filters = 0;
    for (dstd::String line; in.next(line);) {
        if (line.empty()) continue;
        simdjson::dom::element row;
        const simdjson::padded_string padded(line);
        REQUIRE(rows.parse(padded).get(row) == simdjson::SUCCESS);
        if (!row["ok"].get_bool().value()) continue;
        const dstd::String text(row["q"].get_string().value());
        auto q = Query::from_string(text);
        if (!q) continue;
        ++golden_filters;
        for (const char* event : EVENTS) check(*q, event);
    }

    CHECK(golden_filters > 300);
    CHECK(pairs > 5000);
    CHECK(kept > 100);
    CHECK(dropped > 100);
}

TEST_CASE("Query::matches keeps the event readable") {
    auto q = Query::from_string(R"(cat == "POSIX" and dur > 1000)");
    REQUIRE(q.has_value());
    json::Document doc;
    auto event = doc.parse(
        R"({"name":"read","cat":"POSIX","dur":2000,"args":{"size":4096}})");
    REQUIRE(event.has_value());
    CHECK(q->matches(*event));
    auto dur = event->find("dur");
    REQUIRE(dur.has_value());
    CHECK(dur->as_int() == 2000);
    CHECK(q->matches(*event));
    auto name = event->find("name");
    REQUIRE(name.has_value());
    CHECK(name->as_string() == "read");
    auto args = event->find("args");
    REQUIRE(args.has_value());
    auto size = args->find("size");
    REQUIRE(size.has_value());
    CHECK(size->as_int() == 4096);
    CHECK(q->matches(*event));

    auto other = doc.parse(R"({"cat":"MPI","dur":2000})");
    REQUIRE(other.has_value());
    CHECK_FALSE(q->matches(*other));
}

TEST_CASE("Query::matches evaluates a value taken out of an event") {
    auto q = Query::from_string("size > 100 and fd == 3");
    REQUIRE(q.has_value());
    json::Document doc;
    auto event = doc.parse(R"({"name":"read","args":{"size":4096,"fd":3}})");
    REQUIRE(event.has_value());
    auto args = event->find("args");
    REQUIRE(args.has_value());
    CHECK(args->find("fd").has_value());
    CHECK(q->matches(*args));
    CHECK(q->matches(*event));
    auto name = event->find("name");
    REQUIRE(name.has_value());
    CHECK_FALSE(q->matches(*name));
}

TEST_CASE("Query::matches on a line that is not an object keeps nothing") {
    json::Document doc;
    for (const char* line : {"[]", "[1,2]", R"([{"cat":"POSIX"}])"}) {
        auto q = Query::from_string(R"(cat == "POSIX")");
        REQUIRE(q.has_value());
        auto event = doc.parse(line);
        REQUIRE(event.has_value());
        CHECK_FALSE(q->matches(*event));
    }
    auto q = Query::from_string("not exists(cat)");
    REQUIRE(q.has_value());
    auto event = doc.parse("[]");
    REQUIRE(event.has_value());
    JsonDoc dom("[]");
    CHECK(q->matches(*event) == eval_json(*q, dom.root()));
}
