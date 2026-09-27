#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/duql/subsumption.h>
#include <dftracer/utils/duql/vectorize.h>
#include <dftracer/utils/json/json_value.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <memory>
#include <string>
#include <vector>

namespace df = dftracer::utils::dataframe;
using namespace dftracer::utils::duql;

namespace {

// The runs table {run, app}: 1 laghos, 2 amg, 2 amg.
std::shared_ptr<const df::DataFrame> runs() {
    auto f = std::make_shared<df::DataFrame>();
    const std::int64_t ids[] = {1, 2, 2};
    f->names = {"run", "app"};
    f->columns.push_back(df::Series::flat_i64(ids, 3));
    f->columns.push_back(
        df::Series::strings(std::vector<std::string>{"laghos", "amg", "amg"}));
    return f;
}

std::shared_ptr<const df::DataFrame> run_ids() {
    auto f = std::make_shared<df::DataFrame>();
    const std::int64_t ids[] = {1, 2, 2};
    f->names = {"run"};
    f->columns.push_back(df::Series::flat_i64(ids, 3));
    return f;
}

// `text`'s scan filter with every lookup bound to runs().
Query bound(const std::string& text) {
    auto p = compile_program(text, {}, nullptr);
    REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().format()));
    REQUIRE(p->main.filter);
    for_each_pipeline_term(p->main, [](const Term& t) {
        if (const auto* l = std::get_if<TLookup>(&t.node))
            l->slot->table = make_lookup_table(
                *l, l->kind == LookupKind::IN ? run_ids() : runs());
    });
    return Query::from_node(clone(*p->main.filter));
}

bool on_json(const Query& q, const std::string& json) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    REQUIRE(parser.parse(json).get(doc) == simdjson::SUCCESS);
    return q.evaluate(dftracer::utils::json::JsonValue(doc));
}

bool on_map(const Query& q, std::int64_t run) {
    ValueMap m;
    m["run"] = Cell{run};
    return q.evaluate(m);
}

}  // namespace

TEST_CASE("lookups evaluate on JSON and flattened records") {
    const std::string lets = "let runs = where run > 0; ";
    const Query arrow = bound(lets + "where run -> runs.app == \"laghos\"");
    CHECK(on_json(arrow, R"({"run":1})"));
    CHECK_FALSE(on_json(arrow, R"({"run":2})"));
    CHECK_FALSE(on_json(arrow, R"({"run":3})"));
    CHECK_FALSE(on_json(arrow, R"({})"));
    CHECK(on_map(arrow, 1));
    CHECK_FALSE(on_map(arrow, 2));

    const Query semi =
        bound("where run in (from data | where run > 0 | select run)");
    CHECK(on_json(semi, R"({"run":2})"));
    CHECK(on_json(semi, R"({"run":2.0})"));
    CHECK_FALSE(on_json(semi, R"({"run":"2"})"));
    CHECK_FALSE(on_json(semi, R"({"run":4})"));
    CHECK(on_map(semi, 1));

    const Query anti =
        bound("where run not in (from data | where run > 0 | select run)");
    CHECK(on_json(anti, R"({"run":4})"));
    CHECK_FALSE(on_json(anti, R"({})"));

    // Run 2 has two rows with equal values: one value, no conflict.
    const Query same =
        bound(lets + "where (run -> runs.app) ?? \"x\" == \"amg\"");
    CHECK(on_json(same, R"({"run":2})"));
}

TEST_CASE("an arrow's conflicting values fail in a value position") {
    auto f = std::make_shared<df::DataFrame>();
    const std::int64_t ids[] = {1, 1};
    f->names = {"run", "app"};
    f->columns.push_back(df::Series::flat_i64(ids, 2));
    f->columns.push_back(
        df::Series::strings(std::vector<std::string>{"laghos", "amg"}));
    auto p = compile_program(
        "let runs = where run > 0; where (run -> runs.app) ?? \"\" == \"amg\"",
        {}, nullptr);
    REQUIRE(p);
    for_each_pipeline_term(p->main, [&](const Term& t) {
        if (const auto* l = std::get_if<TLookup>(&t.node))
            l->slot->table = make_lookup_table(*l, f);
    });
    const Query q = Query::from_node(clone(*p->main.filter));
    try {
        (void)on_json(q, R"({"run":1})");
        FAIL("no conflict error");
    } catch (const dftracer::utils::DFTUtilsException& e) {
        const std::string what = e.what();
        CHECK(what.find("'runs'") != std::string::npos);
        CHECK(what.find("key 1") != std::string::npos);
    }
    // In a comparison, any matching row may make it hold.
    auto any = compile_program(
        "let runs = where run > 0; where run -> runs.app == \"amg\"", {},
        nullptr);
    REQUIRE(any);
    for_each_pipeline_term(any->main, [&](const Term& t) {
        if (const auto* l = std::get_if<TLookup>(&t.node))
            l->slot->table = make_lookup_table(*l, f);
    });
    CHECK(on_json(Query::from_node(clone(*any->main.filter)), R"({"run":1})"));
}

TEST_CASE("a lookup leaf is a residual to masks and subsumption") {
    const Query q =
        bound("where run in (from data | where run > 0 | select run)");
    df::DataFrame batch;
    const std::int64_t ids[] = {1, 4};
    batch.names = {"run"};
    batch.columns.push_back(df::Series::flat_i64(ids, 2));
    CHECK_THROWS(batch.mask(q));
    const Query plain = Query::from_node(
        clone(*compile_program("where run == 1", {}, nullptr)->main.filter));
    CHECK_FALSE(query_subsumes(plain.root(), q.root()));
    CHECK_FALSE(query_subsumes(q.root(), plain.root()));
}

TEST_CASE("an unbound lookup fails loudly") {
    auto p = compile_program(
        "where run in (from data | where run > 0 | select run)", {}, nullptr);
    REQUIRE(p);
    const Query q = Query::from_node(clone(*p->main.filter));
    CHECK_THROWS_AS((void)on_json(q, R"({"run":1})"),
                    dftracer::utils::DFTUtilsException);
}
