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

// `text`'s scan filter with every lookup bound to runs().
Query bound(const std::string& text) {
    auto p = compile_program(text, {}, nullptr);
    REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().format()));
    REQUIRE(p->main.filter);
    for_each_pipeline_term(p->main, [](const Term& t) {
        if (const auto* l = std::get_if<TLookup>(&t.node))
            l->slot->table = make_lookup_table(*l, runs());
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

TEST_CASE("a lookup leaf is a residual to subsumption") {
    const Query q =
        bound("let runs = where run > 0; where run -> runs.app == \"amg\"");
    const Query plain = Query::from_node(
        clone(*compile_program("where run == 1", {}, nullptr)->main.filter));
    CHECK_FALSE(query_subsumes(plain.root(), q.root()));
    CHECK_FALSE(query_subsumes(q.root(), plain.root()));
}

TEST_CASE("an unbound lookup fails loudly") {
    auto p = compile_program(
        "let runs = where run > 0; where run -> runs.app == \"amg\"", {},
        nullptr);
    REQUIRE(p);
    const Query q = Query::from_node(clone(*p->main.filter));
    CHECK_THROWS_AS((void)on_json(q, R"({"run":1})"),
                    dftracer::utils::DFTUtilsException);
}

TEST_CASE("a joined sub-query term leaves the scan filter") {
    auto p = compile_program(
        "where name == \"a\" and run in (from data | select run) and "
        "dur > (from data | where run == ^.run | agg { m = mean(dur) }) and "
        "dur > (from data | where ts < ^.ts | agg { m = max(dur) }) and "
        "not (run in (from data | select run))",
        {}, nullptr);
    REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().format()));
    REQUIRE(p->main.filter);
    CHECK(p->main.filter_text.find("name") != std::string::npos);
    CHECK(p->main.filter_text.find("run in (") != std::string::npos);
    CHECK(p->main.filter_text.find("range < ts") != std::string::npos);
    CHECK(p->main.filter_text.find(" on run") == std::string::npos);
    CHECK(p->main.filter_text.find("not") == std::string::npos);
    CHECK(p->main.stages.size() == 2);
    std::vector<const TLookup*> joins;
    std::vector<const TLookup*> sets;
    for_each_pipeline_term(p->main, [&](const Term& t) {
        if (const auto* l = std::get_if<TLookup>(&t.node)) {
            if (joined(*l)) joins.push_back(l);
            if (l->key_set) sets.push_back(l);
        }
    });
    CHECK(joins.size() == 2);
    CHECK(sets.size() == 1);
    CHECK_THROWS_AS(make_lookup_table(*joins.front(), runs()),
                    dftracer::utils::DFTUtilsException);
}

TEST_CASE("a range sub-query reads the rows between the enclosing bounds") {
    // pid 1: (ts 10, dur 5) (20, 9) (20, 1) (30, null) (null, 100);
    // pid 2: (15, 7).
    auto side = std::make_shared<df::DataFrame>();
    const std::int64_t dur[] = {5, 9, 1, 0, 100, 7};
    const std::int64_t pid[] = {1, 1, 1, 1, 1, 2};
    const std::int64_t ts[] = {10, 20, 20, 30, 0, 15};
    const std::uint8_t dur_valid[] = {0b110111};
    const std::uint8_t ts_valid[] = {0b101111};
    side->names = {"__correlated_value", "__ck0", "__correlated_range"};
    side->columns.push_back(df::Series::flat_i64(dur, 6, dur_valid));
    side->columns.push_back(df::Series::flat_i64(pid, 6));
    side->columns.push_back(df::Series::flat_i64(ts, 6, ts_valid));

    auto lookup_of = [&](const std::string& text) {
        auto p = compile_program(text, {}, nullptr);
        REQUIRE_MESSAGE(p.has_value(),
                        (p ? std::string() : p.error().format()));
        std::shared_ptr<Program> program =
            std::make_shared<Program>(std::move(*p));
        const TLookup* found = nullptr;
        for_each_pipeline_term(program->main, [&](const Term& t) {
            if (const auto* l = std::get_if<TLookup>(&t.node)) {
                REQUIRE(l->range != RangeRead::NONE);
                l->slot->table = make_lookup_table(*l, side);
                found = l;
            }
        });
        REQUIRE(found);
        return std::make_pair(program, found);
    };

    const auto [p, l] = lookup_of(
        "where dur == (from data | where pid == ^.pid and ts < ^.ts | agg { "
        "m = max(dur) })");
    const Query q = Query::from_node(clone(*p->main.filter));
    CHECK(on_json(q, R"({"pid":1,"ts":25,"dur":9})"));
    CHECK(on_json(q, R"({"pid":1,"ts":20,"dur":5})"));
    CHECK(on_json(q, R"({"pid":1,"ts":20.5,"dur":9})"));
    CHECK_FALSE(on_json(q, R"({"pid":1,"ts":10,"dur":0})"));
    CHECK_FALSE(on_json(q, R"({"pid":1,"ts":"x","dur":9})"));
    CHECK_FALSE(on_json(q, R"({"pid":2,"ts":10,"dur":7})"));
    CHECK(on_json(q, R"({"pid":2,"ts":16,"dur":7})"));

    const std::int64_t opid[] = {1, 1, 1, 2, 3, 1};
    const std::int64_t lo[] = {10, 11, 0, 0, 0, 0};
    const std::int64_t hi[] = {20, 30, 5, 100, 100, 100};
    const std::uint8_t hi_valid[] = {0b011111};
    const df::Series k = df::Series::flat_i64(opid, 6);
    const df::Series a = df::Series::flat_i64(lo, 6);
    const df::Series b = df::Series::flat_i64(hi, 6, hi_valid);
    const std::vector<const df::Series*> keys = {&k, &a, &b};
    auto band = [&](const std::string& agg) {
        const auto [bp, bl] = lookup_of(
            "derive v = (from data | where pid == ^.pid and ts between ^.lo "
            "and ^.hi | agg { v = " +
            agg + " })");
        return lookup_column(*bl, keys, 6);
    };
    auto ints = [](const df::Series& s) {
        std::vector<std::string> out;
        for (std::int64_t r = 0; r < s.length(); ++r)
            out.push_back(
                s.is_null(r)
                    ? "null"
                    : std::to_string(
                          s.cast(df::TypeId::Int64).data<std::int64_t>()[r]));
        return out;
    };
    using V = std::vector<std::string>;
    CHECK(ints(band("count()")) == V{"3", "3", "0", "1", "0", "0"});
    CHECK(ints(band("count(dur)")) == V{"3", "2", "0", "1", "0", "0"});
    CHECK(ints(band("sum(dur)")) == V{"15", "10", "null", "7", "null", "null"});
    CHECK(ints(band("min(dur)")) == V{"1", "1", "null", "7", "null", "null"});
    CHECK(ints(band("max(dur)")) == V{"9", "9", "null", "7", "null", "null"});
    const df::Series mean = band("mean(dur)");
    CHECK(mean.type() == df::TypeId::Float64);
    CHECK(mean.data<double>()[0] == doctest::Approx(5.0));
    CHECK(mean.is_null(2));

    const auto [sp, sl] = lookup_of(
        "derive v = (from data | where pid == ^.pid and ts between ^.lo and "
        "^.hi | select dur)");
    const std::int64_t one_lo[] = {10, 0};
    const std::int64_t one_hi[] = {10, 100};
    const df::Series k2 = df::Series::flat_i64(opid, 2);
    const df::Series a2 = df::Series::flat_i64(one_lo, 2);
    const df::Series b2 = df::Series::flat_i64(one_hi, 1);
    CHECK(ints(lookup_column(*sl, {&k2, &a2, &b2}, 1)) == V{"5"});
    std::string error;
    try {
        const df::Series b3 = df::Series::flat_i64(one_hi + 1, 1);
        const df::Series a3 = df::Series::flat_i64(one_lo + 1, 1);
        (void)lookup_column(*sl, {&k2, &a3, &b3}, 1);
    } catch (const dftracer::utils::DFTUtilsException& e) {
        error = e.what();
    }
    CHECK(error.find("rows in the range") != std::string::npos);
}
