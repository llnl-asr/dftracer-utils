#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/duql/lower.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/syntax/lexer.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/syntax/tree.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace duql = dftracer::utils::duql;
using duql::syntax::Tok;

namespace {

std::vector<Tok> kinds(std::string_view text) {
    auto tokens = duql::syntax::lex(text);
    REQUIRE(tokens.has_value());
    std::vector<Tok> out;
    for (const auto& t : *tokens) out.push_back(t.kind);
    return out;
}

duql::syntax::Program parsed(std::string_view text) {
    auto q = duql::syntax::parse(text);
    if (!q) FAIL(q.error().format());
    return std::move(*q);
}

const duql::syntax::Expr& where_condition(const duql::syntax::Program& q) {
    REQUIRE(q.pipeline);
    REQUIRE(q.pipeline->stages.size() == 1);
    const auto* w =
        std::get_if<duql::syntax::Where>(&q.pipeline->stages[0].node);
    REQUIRE(w);
    return *w->condition;
}

std::string lowered(std::string_view text, const duql::Params& params = {}) {
    auto node = duql::parse(text, params);
    if (!node) FAIL(node.error().format());
    return duql::to_string(**node);
}

dftracer::utils::expected<duql::Pipeline, duql::DuqlError> compile_pipeline(
    std::string_view text, const duql::Params& params,
    const duql::Roles* roles) {
    auto p = duql::compile_program(text, params, roles);
    if (!p) return dftracer::utils::unexpected(p.error());
    return std::move(p->main);
}

bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("lex - minus is always an operator") {
    CHECK(kinds("5-3") ==
          std::vector<Tok>{Tok::INT, Tok::MINUS, Tok::INT, Tok::END});
    CHECK(kinds("x > -5") ==
          std::vector<Tok>{Tok::NAME, Tok::GT, Tok::MINUS, Tok::INT, Tok::END});
    CHECK(kinds("a->b") ==
          std::vector<Tok>{Tok::NAME, Tok::ARROW, Tok::NAME, Tok::END});
}

TEST_CASE("lex - backtick keys") {
    auto tokens = duql::syntax::lex("counters.`cpu.idle_pct`.p50 > 90");
    REQUIRE(tokens.has_value());
    REQUIRE(tokens->size() >= 6);
    CHECK((*tokens)[2].kind == Tok::QNAME);
    CHECK((*tokens)[2].text == "cpu.idle_pct");
    CHECK_FALSE(duql::syntax::lex("`` == 1").has_value());
    CHECK_FALSE(duql::syntax::lex("`abc == 1").has_value());
}

TEST_CASE("lex - comments are skipped") {
    CHECK(kinds("# only a comment") == std::vector<Tok>{Tok::END});
    CHECK(kinds("a == 1 # trailing\nand b == 2") == kinds("a == 1 and b == 2"));
    CHECK(kinds("a # mid\n== 1") == kinds("a == 1"));
}

TEST_CASE("lex - params, durations and numbers") {
    auto tokens = duql::syntax::lex("$app 250ms 1.5 1e3 42");
    REQUIRE(tokens.has_value());
    CHECK((*tokens)[0].kind == Tok::PARAM);
    CHECK((*tokens)[0].text == "app");
    CHECK((*tokens)[1].kind == Tok::DURATION);
    CHECK((*tokens)[1].text == "250ms");
    CHECK((*tokens)[2].kind == Tok::FLOAT);
    CHECK((*tokens)[3].kind == Tok::FLOAT);
    CHECK((*tokens)[4].kind == Tok::INT);
    auto bad = duql::syntax::lex("x == 1abc");
    REQUIRE_FALSE(bad.has_value());
    CHECK(has(bad.error().message, "Invalid number"));
}

TEST_CASE("lex - call adjacency") {
    auto call = duql::syntax::lex("any(tags)");
    REQUIRE(call.has_value());
    CHECK((*call)[1].kind == Tok::LPAREN);
    CHECK((*call)[1].adjacent);
    auto spaced = duql::syntax::lex("f (x)");
    REQUIRE(spaced.has_value());
    CHECK_FALSE((*spaced)[1].adjacent);

    const auto q = parsed(R"(any(tags) == "y")");
    const auto& b = std::get<duql::syntax::Binary>(where_condition(q).node);
    CHECK(std::holds_alternative<duql::syntax::Call>(b.left->node));

    auto not_call = duql::syntax::parse("f (x) == 1");
    if (not_call) {
        const auto& e = where_condition(*not_call);
        const auto* nb = std::get_if<duql::syntax::Binary>(&e.node);
        CHECK_FALSE(
            (nb && std::holds_alternative<duql::syntax::Call>(nb->left->node)));
    }
}

TEST_CASE("parse - duql pragma") {
    auto q = duql::syntax::parse("duql 1\nwhere a == 1");
    REQUIRE(q.has_value());
    CHECK(q->version == 1);
    CHECK(q->pipeline);
    auto bad = duql::syntax::parse("duql 2\nwhere a == 1");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().line == 1);
}

TEST_CASE("parse - case-insensitive keywords") {
    CHECK(lowered("a == 1 AND b == 2 Or c == 3") ==
          lowered("a == 1 and b == 2 or c == 3"));
    CHECK(lowered("x NOT IN [1, 2]") == lowered("x not in [1, 2]"));
    CHECK(lowered("x == TRUE and y == False") ==
          lowered("x == true and y == false"));
}

TEST_CASE("parse - negative literal lowers to int64") {
    auto node = duql::parse("x > -5");
    REQUIRE(node.has_value());
    const auto& cmp = std::get<duql::CompareNode>((*node)->data);
    CHECK(cmp.field.path == "x");
    CHECK(cmp.op == duql::CompareOp::GT);
    REQUIRE(std::holds_alternative<std::int64_t>(cmp.value.value));
    CHECK(std::get<std::int64_t>(cmp.value.value) == -5);
}

TEST_CASE("parse - subtraction") {
    const auto q = parsed("ts-1000 > 5");
    const auto& gt = std::get<duql::syntax::Binary>(where_condition(q).node);
    CHECK(gt.op == duql::syntax::BinaryOp::GT);
    const auto* sub = std::get_if<duql::syntax::Binary>(&gt.left->node);
    REQUIRE(sub);
    CHECK(sub->op == duql::syntax::BinaryOp::SUB);
    auto node = duql::parse("ts-1000 > 5");
    REQUIRE(node.has_value());
    CHECK(duql::to_string(**node) == "(ts - 1000) > 5");
}

TEST_CASE("parse - quoted key path") {
    const auto q = parsed("counters.`cpu.idle_pct`.p50 > 90");
    const auto& gt = std::get<duql::syntax::Binary>(where_condition(q).node);
    const auto& path = std::get<duql::syntax::Path>(gt.left->node);
    REQUIRE(path.steps.size() == 3);
    CHECK(path.steps[0].key == "counters");
    CHECK_FALSE(path.steps[0].quoted);
    CHECK(path.steps[1].key == "cpu.idle_pct");
    CHECK(path.steps[1].quoted);
    CHECK(path.steps[2].key == "p50");
}

TEST_CASE("parse - a stage name as a field") {
    const auto q = parsed("where sample == 1");
    const auto& eq = std::get<duql::syntax::Binary>(where_condition(q).node);
    const auto& path = std::get<duql::syntax::Path>(eq.left->node);
    REQUIRE(path.steps.size() == 1);
    CHECK(path.steps[0].key == "sample");

    CHECK_FALSE(duql::syntax::parse("sample == 1").has_value());
    const auto quoted = parsed("`sample` == 1");
    const auto& qe =
        std::get<duql::syntax::Binary>(where_condition(quoted).node);
    CHECK(std::get<duql::syntax::Path>(qe.left->node).steps[0].key == "sample");
}

TEST_CASE("parse - session clauses") {
    const auto bare = parsed("session gap 1s");
    const auto& b =
        std::get<duql::syntax::Session>(bare.pipeline->stages[0].node);
    CHECK(b.keys.empty());
    CHECK(b.gap);
    CHECK_FALSE(b.max);
    CHECK(b.as.empty());

    const auto full = parsed("session pid, t = tid gap 30s max 1h as visit");
    const auto& f =
        std::get<duql::syntax::Session>(full.pipeline->stages[0].node);
    REQUIRE(f.keys.size() == 2);
    CHECK(f.keys[1].name == "t");
    CHECK(f.max);
    CHECK(f.as == "visit");

    CHECK_FALSE(duql::syntax::parse("session pid max 1h").has_value());
    auto r = duql::syntax::parse("session pid");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().indicator == std::string(11, ' ') + "^");

    const auto field = parsed("where session == 1");
    const auto& eq =
        std::get<duql::syntax::Binary>(where_condition(field).node);
    CHECK(std::get<duql::syntax::Path>(eq.left->node).steps[0].key ==
          "session");
}

TEST_CASE("parse - let and a pipeline") {
    const auto q = parsed(
        R"(let runs = where type == "run"; from data | where run -> runs.app == "laghos" | group name { n = count() } | sort -n | take 10)");
    REQUIRE(q.decls.size() == 1);
    CHECK(std::holds_alternative<duql::syntax::Let>(q.decls[0]));
    REQUIRE(q.pipeline);
    REQUIRE(q.pipeline->sources.size() == 1);
    CHECK(std::get<std::string>(q.pipeline->sources[0].name) == "data");
    const auto& stages = q.pipeline->stages;
    REQUIRE(stages.size() == 4);
    CHECK(std::holds_alternative<duql::syntax::Where>(stages[0].node));
    CHECK(std::holds_alternative<duql::syntax::Group>(stages[1].node));
    CHECK(std::holds_alternative<duql::syntax::Sort>(stages[2].node));
    CHECK(std::holds_alternative<duql::syntax::Take>(stages[3].node));
}

TEST_CASE("parse - every stage kind") {
    const std::vector<std::string> texts = {
        "where x > 1",
        "derive y = x * 2",
        "select a, b as c",
        "drop a",
        "rename p = pid",
        "distinct a",
        "group k { n = count() }",
        "agg { n = count() }",
        "window k sort ts { r = row_number() }",
        R"(pivot cat in ["a"] { n = count() })",
        "unpivot a, b as k, v",
        "sort -a",
        "take 5",
        "skip 3",
        "sample 10 seed 1",
        "expand hosts as h",
        "lookup runs on run",
        "lookup s on pid asof ts",
        "lookup s on pid overlap",
        "union (from runs)",
        "call p.t(x)",
        "time_range 1s .. 5s",
        "call_tree",
        "bucket 100ms fill",
        "session pid gap 1s",
    };
    std::vector<std::size_t> indexes;
    for (const auto& text : texts) {
        CAPTURE(text);
        auto q = duql::syntax::parse(text);
        CHECK(q.has_value());
        if (!q) continue;
        REQUIRE(q->pipeline);
        REQUIRE(q->pipeline->stages.size() == 1);
        indexes.push_back(q->pipeline->stages[0].node.index());
    }
    std::sort(indexes.begin(), indexes.end());
    CHECK(std::unique(indexes.begin(), indexes.end()) == indexes.end());
    CHECK(indexes.size() == std::variant_size_v<duql::syntax::StageNode>);
}

TEST_CASE("parse - source members need semicolons") {
    CHECK(duql::syntax::parse("source s { a = where x; b = where y }")
              .has_value());
    CHECK_FALSE(duql::syntax::parse("source s { a = where x b = where y }")
                    .has_value());
    CHECK_FALSE(duql::syntax::parse("source s { a = where x, b = where y }")
                    .has_value());
}

TEST_CASE("parse - as-of lookup") {
    const auto q =
        parsed("lookup s on pid, a == b asof ts == t nearest within $w");
    const auto* a =
        std::get_if<duql::syntax::AsofLookup>(&q.pipeline->stages[0].node);
    REQUIRE(a);
    CHECK(a->rowset == "s");
    CHECK(a->keys.size() == 2);
    CHECK(a->time.second);
    CHECK(a->direction == duql::syntax::AsofDirection::NEAREST);
    CHECK(a->within == "$w");

    const auto d = parsed("lookup s on pid asof ts");
    const auto* b =
        std::get_if<duql::syntax::AsofLookup>(&d.pipeline->stages[0].node);
    REQUIRE(b);
    CHECK(b->direction == duql::syntax::AsofDirection::BACKWARD);
    CHECK(b->within.empty());

    const auto both = duql::syntax::parse("lookup s on pid asof ts into m");
    REQUIRE_FALSE(both.has_value());
    CHECK(both.error().format().find("cannot be combined") !=
          std::string::npos);
    CHECK_FALSE(
        duql::syntax::parse("lookup s on pid asof ts within -1").has_value());
    CHECK_FALSE(
        duql::syntax::parse("lookup s on pid asof ts within 1.5").has_value());
}

TEST_CASE("parse - overlap lookup") {
    const auto q = parsed("lookup s on pid, a == b overlap into m");
    const auto* o =
        std::get_if<duql::syntax::OverlapLookup>(&q.pipeline->stages[0].node);
    REQUIRE(o);
    CHECK(o->rowset == "s");
    CHECK(o->keys.size() == 2);
    CHECK(o->into == "m");
    const auto p = parsed("lookup s on pid overlap");
    CHECK(std::get<duql::syntax::OverlapLookup>(p.pipeline->stages[0].node)
              .into.empty());
    CHECK_FALSE(
        duql::syntax::parse("lookup s on pid overlap into").has_value());
}

TEST_CASE("parse - sort keys") {
    const auto q = parsed("sort -a + b, - -c, d");
    const auto& keys =
        std::get<duql::syntax::Sort>(q.pipeline->stages[0].node).keys;
    REQUIRE(keys.size() == 3);

    CHECK(keys[0].descending);
    const auto* add = std::get_if<duql::syntax::Binary>(&keys[0].value->node);
    REQUIRE(add);
    CHECK(add->op == duql::syntax::BinaryOp::ADD);

    CHECK(keys[1].descending);
    const auto* neg = std::get_if<duql::syntax::Unary>(&keys[1].value->node);
    REQUIRE(neg);
    CHECK(neg->op == duql::syntax::UnaryOp::NEG);
    CHECK(std::holds_alternative<duql::syntax::Path>(neg->operand->node));

    CHECK_FALSE(keys[2].descending);
}

TEST_CASE("parse - arrows") {
    SUBCASE("tuple key") {
        const auto q = parsed(R"((run, host) -> hosts.name != "h1")");
        const auto& ne =
            std::get<duql::syntax::Binary>(where_condition(q).node);
        const auto& arrow = std::get<duql::syntax::Arrow>(ne.left->node);
        CHECK(arrow.rowset == "hosts");
        CHECK(std::get<duql::syntax::Tuple>(arrow.key->node).items.size() == 2);
        CHECK_FALSE(arrow.target_key);
        REQUIRE(arrow.path.steps.size() == 1);
        CHECK(arrow.path.steps[0].key == "name");
    }
    SUBCASE("target key") {
        const auto q = parsed(R"(fhash -> files(id).path == "/a")");
        const auto& eq =
            std::get<duql::syntax::Binary>(where_condition(q).node);
        const auto& arrow = std::get<duql::syntax::Arrow>(eq.left->node);
        CHECK(arrow.rowset == "files");
        REQUIRE(arrow.target_key);
        CHECK(arrow.target_key->steps[0].key == "id");
        CHECK(arrow.path.steps[0].key == "path");
    }
    SUBCASE("chain") {
        const auto q =
            parsed(R"(run -> runs.system -> systems.cpu == "a64fx")");
        const auto& eq =
            std::get<duql::syntax::Binary>(where_condition(q).node);
        const auto& outer = std::get<duql::syntax::Arrow>(eq.left->node);
        CHECK(outer.rowset == "systems");
        const auto& inner = std::get<duql::syntax::Arrow>(outer.key->node);
        CHECK(inner.rowset == "runs");
        CHECK(std::holds_alternative<duql::syntax::Path>(inner.key->node));
    }
}

TEST_CASE("parse - sub-query") {
    const auto q = parsed("x in (from runs | select run)");
    const auto& in = std::get<duql::syntax::In>(where_condition(q).node);
    CHECK_FALSE(in.negated);
    REQUIRE(in.subquery);
    CHECK(in.subquery->sources.size() == 1);
    REQUIRE(in.subquery->stages.size() == 1);
    CHECK(std::holds_alternative<duql::syntax::Select>(
        in.subquery->stages[0].node));
}

TEST_CASE("parse - legacy substring test") {
    const auto q = parsed(R"("io" in cat)");
    const auto& c = std::get<duql::syntax::Contains>(where_condition(q).node);
    CHECK(c.text == "io");
    CHECK_FALSE(c.negated);
    CHECK(c.path.steps[0].key == "cat");
    const auto n = parsed(R"("io" not in cat)");
    CHECK(std::get<duql::syntax::Contains>(where_condition(n).node).negated);
}

TEST_CASE("errors - caret position") {
    auto r = duql::syntax::parse("where a == ");
    REQUIRE_FALSE(r.has_value());
    const auto& e = r.error();
    CHECK_FALSE(e.message.empty());
    CHECK(e.line == 1);
    CHECK(e.column == 11);
    CHECK(e.source == "where a == ");
    CHECK(e.indicator == std::string(11, ' ') + "^");

    auto m = duql::syntax::parse("where a == 1\n| derive y = \n| take 5");
    REQUIRE_FALSE(m.has_value());
    CHECK(m.error().line == 3);
    CHECK(m.error().source == "| take 5");
    CHECK(m.error().column == 0);
    CHECK(m.error().indicator == "^");
}

TEST_CASE("round trip - grammar samples") {
    std::ifstream in(DUQL_SAMPLES_PATH);
    REQUIRE(in.good());
    std::vector<std::string> valid;
    std::vector<std::string> invalid;
    bool in_invalid = false;
    std::string current;
    std::string line;
    auto flush = [&] {
        (in_invalid ? invalid : valid).push_back(current);
        current.clear();
    };
    while (std::getline(in, line)) {
        if (line == "---") {
            flush();
        } else if (line == "### invalid") {
            flush();
            in_invalid = true;
        } else {
            current += line;
            current += '\n';
        }
    }
    flush();
    CHECK(valid.size() > 40);
    CHECK(invalid.size() > 10);

    for (const auto& text : valid) {
        INFO("sample:\n", text);
        auto a = duql::syntax::parse(text);
        CHECK_MESSAGE(a.has_value(), (a ? std::string() : a.error().format()));
        if (!a) continue;
        const std::string printed = duql::syntax::to_text(*a);
        INFO("printed:\n", printed);
        CHECK(printed.rfind("duql 1", 0) == 0);
        auto b = duql::syntax::parse(printed);
        CHECK_MESSAGE(b.has_value(), (b ? std::string() : b.error().format()));
        if (!b) continue;
        CHECK(duql::syntax::equal(*a, *b));
    }
    for (const auto& text : invalid) {
        INFO("invalid sample:\n", text);
        CHECK_FALSE(duql::syntax::parse(text).has_value());
    }
}

TEST_CASE("lower - unsupported constructs") {
    auto duration = duql::parse("dur > 1ms");
    REQUIRE_FALSE(duration.has_value());
    CAPTURE(duration.error().message);
    CHECK(has(duration.error().message, "'dur'"));
    CHECK(has(duration.error().message, "as_time"));

    auto take = duql::parse("where a == 1 | take 5");
    REQUIRE_FALSE(take.has_value());
    CAPTURE(take.error().message);
    CHECK(has(take.error().message, "'take' is a pipeline stage"));

    auto group = duql::parse("where a == 1 | group k { n = count() }");
    REQUIRE_FALSE(group.has_value());
    CHECK(has(group.error().message, "'group' is a pipeline stage"));
}

TEST_CASE("pipeline - stages") {
    duql::Params params;
    params.emplace("n", duql::LiteralValue{std::uint64_t{3}});
    auto p = compile_pipeline(
        "where cat == \"POSIX\" | where dur > 5 | select name, dur, ts"
        " | derive ms = dur / 1000 | sort -ms, name nulls first"
        " | distinct name | drop ts | rename d = dur | take $n | skip 1",
        params, nullptr);
    REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().format()));
    CHECK(std::holds_alternative<duql::AndNode>(p->filter->data));
    CHECK(p->scan_select == std::vector<std::string>{"name", "dur", "ts"});
    REQUIRE(p->stages.size() == 7);
    const auto& derive = std::get<duql::PipelineDerive>(p->stages[0]);
    CHECK(derive.items[0].name == "ms");
    CHECK(derive.items[0].text == "dur / 1000");
    const auto& sort = std::get<duql::PipelineSort>(p->stages[1]);
    CHECK(sort.keys[0].key.name == "ms");
    CHECK(sort.keys[0].descending);
    CHECK(sort.keys[1].nulls_first);
    CHECK(std::get<duql::PipelineRename>(p->stages[4]).pairs[0] ==
          std::pair<std::string, std::string>{"d", "dur"});
    CHECK(std::get<duql::PipelineTake>(p->stages[5]).count == 3);
    CHECK(std::get<duql::PipelineSkip>(p->stages[6]).count == 1);

    auto expr_select =
        compile_pipeline("select name, x = dur * 2", {}, nullptr);
    REQUIRE(expr_select.has_value());
    CHECK(expr_select->scan_select.empty());
    CHECK(
        std::get<duql::PipelineSelect>(expr_select->stages[0]).items[1].name ==
        "x");

    auto unnamed = compile_pipeline("derive dur * 2", {}, nullptr);
    CHECK_FALSE(unnamed.has_value());
    auto later = compile_pipeline("lookup runs on run", {}, nullptr);
    REQUIRE_FALSE(later.has_value());
    CHECK(has(later.error().message, "'runs'"));
    CHECK(has(later.error().message, "not a let or a row set"));
    auto by = compile_pipeline("take 5 by name sort -dur", {}, nullptr);
    REQUIRE_MESSAGE(by.has_value(), (by ? std::string() : by.error().format()));
    const auto& take_by = std::get<duql::PipelineTakeBy>(by->stages[0]);
    CHECK(take_by.count == 5);
    CHECK(take_by.keys[0].name == "name");
    CHECK(take_by.order[0].descending);
    auto no_roles = compile_pipeline("time_range 1s .. 2s", {}, nullptr);
    REQUIRE_FALSE(no_roles.has_value());
    CHECK(has(no_roles.error().message, "time role"));
    auto dangling = compile_pipeline("bucket 5 | take 1", {}, nullptr);
    REQUIRE_FALSE(dangling.has_value());
    auto negative = compile_pipeline(
        "take $n", {{"n", duql::LiteralValue{std::int64_t{-1}}}}, nullptr);
    CHECK_FALSE(negative.has_value());
}

TEST_CASE("pipeline - durations and time functions") {
    duql::Roles roles;
    roles.fields.emplace("dur", 1000);
    roles.fields.emplace("ts", 1000);
    auto p = compile_pipeline(
        "where dur > 250ms | derive b = bin(ts, 1s), s = to_seconds(dur),"
        " t = as_time(x, \"ms\"), u = as_time(x, \"ns\"), w = dur + 2us"
        " | where dur between 1ms and 1.5ms",
        {}, &roles);
    REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().format()));
    const auto& cmp = std::get<duql::CompareNode>(p->filter->data);
    CHECK(std::get<std::uint64_t>(cmp.value.value) == 250000);
    const auto& d = std::get<duql::PipelineDerive>(p->stages[0]);
    CHECK(d.items[0].text == "(ts // 1000000) * 1000000");
    CHECK(d.items[1].text == "dur / 1e+06");
    CHECK(d.items[2].text == "x * 1000");
    CHECK(d.items[3].text == "x / 1000.0");
    CHECK(d.items[4].text == "dur + 2");
    CHECK(std::get<duql::PipelineWhere>(p->stages[1]).text ==
          "dur between 1000 and 1500");

    auto no_role = compile_pipeline("where x > 1ms", {}, &roles);
    REQUIRE_FALSE(no_role.has_value());
    CHECK(has(no_role.error().message, "'x'"));
    CHECK(has(no_role.error().message, "as_time"));
    auto bare = compile_pipeline("derive b = bin(ts, 1s)", {}, nullptr);
    REQUIRE_FALSE(bare.has_value());
    CHECK(has(bare.error().message, "time roles"));
}

TEST_CASE("pipeline - session") {
    duql::Roles us;
    us.time = "ts";
    us.duration = "dur";
    us.schema = "dftracer";
    us.fields.emplace("ts", 1000);
    us.fields.emplace("dur", 1000);
    auto p =
        compile_pipeline("session pid, tid gap 1ms max 2s as burst", {}, &us);
    REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().format()));
    const auto& s = std::get<duql::PipelineSession>(p->stages[0]);
    CHECK(s.keys.size() == 2);
    CHECK(s.gap == 1000);
    CHECK(s.span == 2000000);
    CHECK(s.name == "burst");
    CHECK(duql::term_text(*s.end) == "ts + dur");

    // Time in seconds, duration in milliseconds.
    duql::Roles secs;
    secs.time = "start";
    secs.duration = "took";
    secs.time_ns_per_unit = 1000000000;
    secs.fields.emplace("start", 1000000000);
    secs.fields.emplace("took", 1000000);
    auto q = compile_pipeline("session host gap 30s", {}, &secs);
    REQUIRE_MESSAGE(q.has_value(), (q ? std::string() : q.error().format()));
    const auto& t = std::get<duql::PipelineSession>(q->stages[0]);
    CHECK(t.gap == 30);
    CHECK(t.span == 0);
    CHECK(t.name == "session");
    CHECK(duql::term_text(*t.end) == "start + (took * 0.001)");

    duql::Roles untimed;
    untimed.schema = "web";
    auto none = compile_pipeline("session gap 1", {}, &untimed);
    REQUIRE_FALSE(none.has_value());
    CHECK(has(none.error().message, "time role"));
    CHECK(has(none.error().message, "'web'"));
    auto neg = compile_pipeline("session gap -1", {}, &us);
    REQUIRE_FALSE(neg.has_value());
    CHECK(has(neg.error().message, "not negative"));
    auto zero_max = compile_pipeline("session gap 1 max 0", {}, &us);
    REQUIRE_FALSE(zero_max.has_value());
}

TEST_CASE("lower - parameters") {
    duql::Params params;
    params.emplace("app", duql::LiteralValue{std::string("laghos")});
    auto node = duql::parse("app == $app", params);
    REQUIRE(node.has_value());
    const auto& cmp = std::get<duql::CompareNode>((*node)->data);
    CHECK(cmp.field.path == "app");
    CHECK(cmp.op == duql::CompareOp::EQ);
    REQUIRE(std::holds_alternative<std::string>(cmp.value.value));
    CHECK(std::get<std::string>(cmp.value.value) == "laghos");

    auto unbound = duql::parse("app == $app");
    REQUIRE_FALSE(unbound.has_value());
    CHECK(has(unbound.error().message, "$app"));
}

TEST_CASE("lower - printed form matches the old parser") {
    CHECK(lowered("x in [1, 2]") == "x in [1, 2]");
    CHECK(lowered("not x == 1") == "not (x == 1)");
    CHECK(lowered(R"(x like "a%")") == R"(x like "a%")");
    CHECK(lowered(R"(x ~* "a")") == R"(x ~* "a")");
    CHECK(lowered(R"(any(tags) == "y")") == R"(any(tags) == "y")");
}

TEST_CASE("lower - expression leaves print text that parses back to them") {
    const char* forms[] = {
        "a > b",
        "(dur / 1000) > (size // 4096)",
        "(x % 3) == -2",
        "-x < 0",
        "x between 1 and (y + 2)",
        "x not between 1 and 2",
        "x is null",
        "x is not missing",
        "(a ?? b) == 2",
        "x in [a, 1, \"s\"]",
        "x not in [a]",
        "lower(name) like \"read%\"",
        "trim(name) ~* \"^a\"",
        "exists(args.retry)",
        "tags[-1] == \"y\"",
        "a.b[-2].c == 1",
        "if(x > 1, \"a\", \"b\") == \"a\"",
        "case(x < 1, 1, x < 2, 2, 3) == 2",
        "json(o) == '{\"a\":1}'",
        "not (lower(x) == \"a\")",
        "concat(a, \"-\", b) != \"\"",
        "flag",
        "coalesce(a, b, 2.5) >= 1.0",
        "any(hosts, .up and (^.cat == \"io\"))",
        "all(h, . > 0)",
        "not all(h[0], .[1] == 2)",
        "any(sizes) >= 3",
        "all(tags) != \"x\"",
        "\"ab\" in any(tags)",
    };
    for (const char* text : forms) {
        CAPTURE(std::string(text));
        const std::string once = lowered(text);
        CHECK(lowered(once.c_str()) == once);
    }
}

TEST_CASE("pipeline - reshaping stages") {
    auto compiled = [](const char* text) {
        auto p = compile_pipeline(
            text, {{"n", duql::LiteralValue{std::int64_t{2}}}}, nullptr);
        REQUIRE_MESSAGE(p.has_value(), (p ? "" : p.error().format()));
        return std::move(*p);
    };
    const auto w = compiled(
        "window pid, tid sort -ts { g = ts - lag(ts, $n), r = row_number() }");
    const auto& win = std::get<duql::PipelineWindow>(w.stages[0]);
    REQUIRE(win.keys.size() == 2);
    CHECK(win.order[0].descending);
    REQUIRE(win.calls.size() == 2);
    CHECK(win.calls[0].fn == duql::WinFn::LAG);
    CHECK(win.calls[0].offset == 2);
    CHECK(win.calls[1].fn == duql::WinFn::ROW_NUMBER);
    CHECK(win.items[0].name == "g");
    CHECK(win.items[0].text == "ts - lag(ts, $n)");

    const auto e = compiled("expand io.ops as x with_index i keep_empty");
    const auto& ex = std::get<duql::PipelineExpand>(e.stages[0]);
    CHECK(ex.path == "io.ops");
    CHECK(ex.name == "x");
    CHECK(ex.index == "i");
    CHECK(ex.keep_empty);
    CHECK(std::get<duql::PipelineExpand>(compiled("expand a.hosts").stages[0])
              .name == "hosts");

    const auto pv = compiled("pivot metric in [\"a\", 2, null] { s = sum(v) }");
    const auto& piv = std::get<duql::PipelinePivot>(pv.stages[0]);
    CHECK(piv.fixed);
    CHECK(piv.values.size() == 3);
    CHECK(piv.aggs[0].fn == duql::AggFn::SUM);

    const auto g = compiled(
        "group k { d = count_distinct(x), c = collect(x), a = arg_max(x, y),"
        " b = arg_min(x, y), s = sketch(x), m = merge(s),"
        " q = quantile(merge(s), 0.9) }");
    const auto& grp = std::get<duql::PipelineGroup>(g.stages[0]);
    REQUIRE(grp.aggs.size() == 7);
    CHECK(grp.aggs[0].fn == duql::AggFn::COUNT_DISTINCT);
    CHECK(grp.aggs[1].fn == duql::AggFn::COLLECT);
    CHECK(grp.aggs[2].fn == duql::AggFn::ARGMAX);
    CHECK(grp.aggs[2].text == "arg_max(x, y)");
    REQUIRE(grp.aggs[3].by);
    CHECK(grp.aggs[4].fn == duql::AggFn::SKETCH);
    CHECK(grp.aggs[5].fn == duql::AggFn::MERGE);
    CHECK(grp.aggs[6].fn == duql::AggFn::QUANTILE);
    CHECK(grp.aggs[6].merged);
    CHECK(grp.aggs[6].q == 0.9);
    CHECK(grp.aggs[6].text == "quantile(merge(s), 0.9)");

    const auto u = compiled("unpivot a, b.c as k, v");
    const auto& un = std::get<duql::PipelineUnpivot>(u.stages[0]);
    CHECK(un.fields == std::vector<std::string>{"a", "b.c"});
    CHECK(un.key == "k");
    CHECK(un.value == "v");
}

TEST_CASE("pipeline - reshaping errors name the construct") {
    const std::vector<std::pair<const char*, const char*>> cases = {
        {"derive r = row_number()", "'window' block"},
        {"derive r = lag(dur)", "'window' block"},
        {"window name { d = dur * 2 }", "window function"},
        {"window name { s = quantile(dur, 2) }", "quantile level"},
        {"window name { s = count_if(dur > 1) }", "does not run"},
        {"window name { l = lag(dur, -1) }", "distance"},
        {"window name { l = lag(sum(dur)) }", "argument"},
        {"pivot k { n = count() } | take 1", "in [...]"},
        {"pivot k { n = dur }", "'pivot' block"},
        {"unpivot a, b as k, k", "key and value"},
        {"expand tags as i with_index i", "index"},
        {"expand tags[-1]", "negative"},
        {"where .x > 1", "stand inside any"},
        {"derive y = ^.x", "stand inside any"},
        {"where any(a, any(.b, . > 1))", "inside another"},
        {"where any(a)", "compares with a value"},
        {"derive s = slice(a)", "Wrong number of arguments to 'slice'"},
        {"group k { a = arg_max(x) }",
         "Wrong number of arguments to 'arg_max'"},
        {"group k { q = quantile(merge(s, t), 0.5) }", "'merge'"},
        {"derive m = merge(s)", "aggregate"},
        {"where x > 1 | union (from t)", "not a let or a row set"},
        {"where x > 1 | call myplug.sessions(gap = 5)", "12h"},
    };
    for (const auto& [text, want] : cases) {
        CAPTURE(std::string(text));
        auto p = compile_pipeline(text, {}, nullptr);
        REQUIRE_FALSE(p.has_value());
        CAPTURE(p.error().message);
        CHECK(has(p.error().message, want));
    }
}

TEST_CASE("macros expand before lowering") {
    auto filter = [](std::string_view text) { return lowered(text); };
    CHECK(filter("def slow(t) = dur > t; slow(250)") == lowered("dur > 250"));
    CHECK(filter("def f(a, b) = a > b; def g(x) = f(x, 1); g(dur)") ==
          lowered("dur > 1"));
    CHECK(filter("def one() = 1; x == one()") == lowered("x == 1"));
    const std::vector<std::pair<const char*, const char*>> errors = {
        {"def a(x) = b(x); def b(x) = a(x); a(1) > 0", "a -> b -> a"},
        {"def f(x) = x > 1; f(1, 2)", "takes 1 argument(s), got 2"},
        {"def len(x) = x; len(a) > 1", "built-in"},
        {"def f(x) = x; def f(y) = y; f(1) > 0", "defined twice"},
        {"def f(x) = x > 1; f(x = 2)", "named argument"},
    };
    for (const auto& [text, want] : errors) {
        CAPTURE(std::string(text));
        auto node = duql::parse(text, {});
        REQUIRE_FALSE(node.has_value());
        CAPTURE(node.error().message);
        CHECK(has(node.error().message, want));
    }
}

TEST_CASE("a query cannot declare a source") {
    auto p = duql::compile_program("source s { a = where x > 1 } from a", {},
                                   nullptr);
    REQUIRE_FALSE(p.has_value());
    CHECK(has(p.error().message, "belongs to a record schema"));
}

TEST_CASE("source row sets are sides and its macros expand") {
    auto p = duql::compile_program(
        "from files | where big(path)", {}, nullptr,
        "files = where name == \"FH\" | select fhash = args.value, "
        "path = args.name; def big(p) = len(p) > 3; def args_fallback = true");
    REQUIRE_MESSAGE(p.has_value(), (p ? "" : p.error().format()));
    REQUIRE(p->sides.size() == 1);
    CHECK(p->sides[0].name == "files");
    CHECK(p->sides[0].rowset);
    CHECK(p->args_fallback);
}
