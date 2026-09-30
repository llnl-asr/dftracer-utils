#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/duql/lower.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/string_literal.h>
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
    duql::PluginCatalog plugins;
    plugins.kind = [](std::string_view name) {
        const dftu_op_desc* op = dftu_op_find(std::string(name).c_str());
        if (!op) return duql::PluginKind::NONE;
        if (dftu_op_kind_of(op->sig) == DFTU_OP_KIND_SERIES)
            return duql::PluginKind::COLUMN;
        if (dftu_op_kind_of(op->sig) == DFTU_OP_KIND_FRAME)
            return duql::PluginKind::TABLE;
        if (dftu_op_kind_of(op->sig) == DFTU_OP_KIND_AGGREGATE)
            return duql::PluginKind::AGGREGATE;
        return duql::PluginKind::OTHER;
    };
    for (std::uint32_t k = 0; k < dftu_op_count(); ++k) {
        const std::string_view op = dftu_op_at(k)->name;
        if (op.starts_with("pa.") || op.starts_with("pb."))
            plugins.namespaces.emplace_back(op.substr(0, 2));
    }
    std::sort(plugins.namespaces.begin(), plugins.namespaces.end());
    plugins.namespaces.erase(
        std::unique(plugins.namespaces.begin(), plugins.namespaces.end()),
        plugins.namespaces.end());
    auto p = duql::compile_program(text, params, roles, {}, &plugins);
    if (!p) return dftracer::utils::unexpected(p.error());
    return std::move(p->main);
}

std::string signature(const duql::Pipeline& p) {
    std::string out = "filter=" + p.filter_text;
    for (const auto& stage : p.stages) {
        out += " | " + std::to_string(stage.index());
        std::visit(
            [&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, duql::PipelineWhere>) {
                    out += " " + n.text;
                } else if constexpr (std::is_same_v<T, duql::PipelineBucket>) {
                    out += " " + std::to_string(n.width) + " " + n.text;
                } else if constexpr (std::is_same_v<T, duql::PipelineGroup>) {
                    for (const auto& k : n.keys) out += " k:" + k.text;
                    for (const auto& a : n.aggs)
                        out += " a:" + a.name + "=" + a.text;
                } else if constexpr (std::is_same_v<T, duql::PipelineSort>) {
                    for (const auto& k : n.keys)
                        out += " " + k.key.text + (k.descending ? " d" : " a");
                } else if constexpr (std::is_same_v<T, duql::PipelineTake>) {
                    out += " " + std::to_string(n.count);
                }
            },
            stage);
    }
    return out;
}

duql::Roles time_roles() {
    duql::Roles roles;
    roles.time = "ts";
    roles.duration = "dur";
    roles.schema = "dftracer";
    roles.fields.emplace("ts", 1000);
    roles.fields.emplace("dur", 1000);
    return roles;
}

std::string compiled(std::string_view text) {
    const duql::Roles roles = time_roles();
    auto p = duql::compile_program(text, {}, &roles);
    if (!p) FAIL(p.error().format());
    std::string out = signature(p->main);
    for (const auto& side : p->sides)
        out += " side " + side.name + " (" + signature(side.pipeline) + ")";
    return out;
}

std::string compile_error(std::string_view text) {
    const duql::Roles roles = time_roles();
    auto p = compile_pipeline(text, {}, &roles);
    REQUIRE_FALSE(p.has_value());
    return p.error().message;
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

TEST_CASE("parse - a computed index round-trips and ends the path") {
    for (const char* q : {"xs[i]", "xs[$n]", "xs[i - 3]", "a.b[i]", "a[0].b[i]",
                          "xs[-1]", "xs[0]"}) {
        CAPTURE(q);
        const auto first = parsed(std::string("where ") + q + " > 1");
        const auto second = parsed(duql::syntax::to_text(first));
        CHECK(duql::syntax::to_text(second) == duql::syntax::to_text(first));
        CHECK(duql::syntax::equal(first, second));
    }
    const auto lit = parsed("where xs[-1] > 1");
    const auto& lb = std::get<duql::syntax::Binary>(where_condition(lit).node);
    const auto* path = std::get_if<duql::syntax::Path>(&lb.left->node);
    REQUIRE(path);
    CHECK(path->steps.back().index == -1);

    const auto q = parsed("where xs[$n] > 1");
    const auto& b = std::get<duql::syntax::Binary>(where_condition(q).node);
    const auto* idx = std::get_if<duql::syntax::Index>(&b.left->node);
    REQUIRE(idx);
    CHECK(std::holds_alternative<duql::syntax::Path>(idx->base->node));
    CHECK(std::holds_alternative<duql::syntax::Param>(idx->index->node));

    for (const char* bad : {"xs[i].b", "xs[i][0]"}) {
        CAPTURE(bad);
        auto r = duql::syntax::parse(std::string("where ") + bad + " == 1");
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().format().find("ends the path") != std::string::npos);
    }
    CHECK_FALSE(duql::syntax::parse("where xs[] == 1").has_value());
}

TEST_CASE("parse - case-insensitive keywords") {
    CHECK(lowered("a == 1 AND b == 2 Or c == 3") ==
          lowered("a == 1 and b == 2 or c == 3"));
    CHECK(lowered("x NOT IN [1, 2]") == lowered("x not in [1, 2]"));
    CHECK(lowered("x == TRUE and y == False") ==
          lowered("x == true and y == false"));
}

TEST_CASE("parse - string escapes decode and print back") {
    auto value = [](std::string_view text) {
        const auto q = parsed(std::string("where x == ") + std::string(text));
        const auto& b = std::get<duql::syntax::Binary>(where_condition(q).node);
        return std::get<std::string>(
            std::get<duql::syntax::Literal>(b.right->node).value);
    };
    CHECK(value(R"("a\"b")") == "a\"b");
    CHECK(value(R"('it\'s')") == "it's");
    CHECK(value(R"("a\\b")") == "a\\b");
    CHECK(value(R"("\d+\.x")") == "\\d+\\.x");
    CHECK(value(R"("\\d")") == "\\d");
    CHECK(value(R"("l1\nl2\t\r")") == "l1\nl2\t\r");
    CHECK(value(R"("\u00e9\ud83d\ude00")") == "\xc3\xa9\xf0\x9f\x98\x80");
    for (const char* bad :
         {R"("\u12")", R"("\ude00")", R"("\ud83d")", R"("\ud83dx")"}) {
        CAPTURE(bad);
        CHECK_FALSE(
            duql::syntax::parse(std::string("where x == ") + bad).has_value());
    }
    const std::string all = "q\" s' b\\ n\n t\t c\x01 u\xc3\xa9";
    const std::string printed =
        duql::syntax::to_text(parsed("where x == " + duql::quote_string(all)));
    CHECK(value(printed.substr(
              printed.find("== ") + 3,
              printed.rfind('\n') - printed.find("== ") - 3)) == all);
}

TEST_CASE("parse - named distinct, pivot labels and take ranges print back") {
    for (const char* q : {"distinct c = cat, dur // 10 as d",
                          R"(pivot k in ["a" as x, "b"] { n = count() })",
                          "take 3..5", "take $a..$b", "bucket 1ms fill forward",
                          "bucket 1ms fill linear from 0 to 6ms as b",
                          "bucket 1ms fill from $lo to $hi",
                          "bucket 1ms fill from $t0 - 1s to ($t0 + 2) * 3 / 2",
                          "bucket 1ms fill as b", "bucket 1ms fill",
                          "bucket 5s every 1s", "bucket 2ms at 1ms",
                          "bucket 5s every 1s at $t0 fill forward from 0 to "
                          "10s as w",
                          "bucket $w every $e"}) {
        CAPTURE(q);
        const auto a = parsed(q);
        const auto b = parsed(duql::syntax::to_text(a));
        CHECK(duql::syntax::equal(a, b));
    }
}

TEST_CASE("parse - bucket fill modes and ranges") {
    auto q = duql::syntax::parse("bucket 1ms fill linear from 0 to 6ms as b");
    REQUIRE(q.has_value());
    const auto& b = std::get<duql::syntax::Bucket>(q->pipeline->stages[0].node);
    CHECK(b.fill);
    CHECK(b.fill_mode == duql::syntax::FillMode::LINEAR);
    CHECK(b.low);
    CHECK(b.high);
    CHECK(b.as == "b");
    auto z = duql::syntax::parse("bucket 1ms fill | take 1");
    REQUIRE(z.has_value());
    CHECK(
        std::get<duql::syntax::Bucket>(z->pipeline->stages[0].node).fill_mode ==
        duql::syntax::FillMode::ZERO);
    auto bad = duql::syntax::parse("bucket 1ms from 0 to 1ms");
    REQUIRE_FALSE(bad.has_value());
    CHECK(has(bad.error().message, "sets the range of 'fill'"));
    CHECK_FALSE(duql::syntax::parse("bucket 1ms fill from 0").has_value());
}

TEST_CASE("parse - bucket every and at") {
    auto q = duql::syntax::parse("bucket 5s every 1s at 2s fill as w");
    REQUIRE(q.has_value());
    const auto& b = std::get<duql::syntax::Bucket>(q->pipeline->stages[0].node);
    CHECK(b.every);
    CHECK(b.at);
    CHECK(b.fill);
    CHECK(b.as == "w");
    CHECK_FALSE(duql::syntax::parse("bucket 5s every").has_value());
    CHECK_FALSE(duql::syntax::parse("bucket 5s at").has_value());
    CHECK_FALSE(duql::syntax::parse("bucket 5s fill every 1s").has_value());
    CHECK_FALSE(duql::syntax::parse("bucket 5s at 1s every 1s").has_value());
}

TEST_CASE("pipeline - bucket every and at") {
    duql::Roles roles;
    roles.time = "ts";
    roles.fields.emplace("ts", 1000);
    auto p =
        compile_pipeline("bucket 2ms at 1ms | agg { n = count() }", {}, &roles);
    REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().format()));
    const auto& b = std::get<duql::PipelineBucket>(p->stages[0]);
    CHECK(duql::term_text(*b.key) == "(((ts - 1000) // 2000) * 2000) + 1000");
    CHECK(b.text == "(((ts - 1000) // 2000) * 2000) + 1000");
    CHECK(b.origin == 1000);
    CHECK_FALSE(b.every);

    auto h = compile_pipeline("bucket 5ms every 1ms | agg { n = count() }", {},
                              &roles);
    REQUIRE_MESSAGE(h.has_value(), (h ? std::string() : h.error().format()));
    const auto& hb = std::get<duql::PipelineBucket>(h->stages[0]);
    REQUIRE(hb.every);
    CHECK(*hb.every == 1000);
    CHECK(hb.width == 5000);
    CHECK(hb.origin == 0);
    CHECK(hb.text == "(ts // 5000) * 5000");

    auto plain =
        compile_pipeline("bucket 2ms | agg { n = count() }", {}, &roles);
    REQUIRE(plain.has_value());
    CHECK(std::get<duql::PipelineBucket>(plain->stages[0]).text ==
          "(ts // 2000) * 2000");

    for (const char* bad : {"bucket 5ms every 0s | agg { n = count() }",
                            "bucket 5ms every -1ms | agg { n = count() }"}) {
        CAPTURE(bad);
        auto r = compile_pipeline(bad, {}, &roles);
        REQUIRE_FALSE(r.has_value());
        CHECK(
            has(r.error().message, "a bucket step ('every') must be positive"));
    }
    auto w = compile_pipeline("bucket 0s every 1ms | agg { n = count() }", {},
                              &roles);
    REQUIRE_FALSE(w.has_value());
    CHECK(has(w.error().message, "a bucket width must be positive"));
}

TEST_CASE("parse - unnamed block entries are named from their text") {
    const std::vector<std::pair<const char*, const char*>> cases = {
        {"group k { count() }", "group k { count = count() }"},
        {"group k { sum(dur) }", "group k { sum_dur = sum(dur) }"},
        {"agg { quantile(dur, 0.99) }",
         "agg { quantile_dur_0_99 = quantile(dur, 0.99) }"},
        {"agg { sum(args.size), n = count() }",
         "agg { sum_args_size = sum(args.size), n = count() }"},
        {"window { sum(-x) }", "window { sum_x = sum(-x) }"},
    };
    for (const auto& [text, named] : cases) {
        CAPTURE(std::string(text));
        CHECK(duql::syntax::equal(parsed(text), parsed(named)));
    }
}

TEST_CASE("parse - over frames print back") {
    for (const char* q :
         {"window tid sort ts { m = max(dur) over 10 rows, b = sum(size) over "
          "1s, c = count() over $w, d = sum(x) over $n rows }",
          "derive r = mean(size) over 1s / 2", "derive r = f(x) over 2.5 + 1",
          "derive r = a ?? sum(x) over 3 rows"}) {
        CAPTURE(q);
        const auto a = parsed(q);
        const auto b = parsed(duql::syntax::to_text(a));
        CHECK(duql::syntax::equal(a, b));
    }
    CHECK_FALSE(duql::syntax::equal(parsed("derive r = sum(x) over 3 rows"),
                                    parsed("derive r = sum(x) over 3")));
    CHECK_FALSE(duql::syntax::equal(parsed("derive r = sum(x) over 3 rows"),
                                    parsed("derive r = sum(x) over 4 rows")));
}

TEST_CASE("parse - over binds tighter than a binary operator") {
    const auto q = parsed("derive r = mean(size) over 1s / 2");
    const auto& d = std::get<duql::syntax::Derive>(q.pipeline->stages[0].node);
    const auto& div = std::get<duql::syntax::Binary>(d.fields[0].value->node);
    const auto& over = std::get<duql::syntax::Over>(div.left->node);
    CHECK_FALSE(over.rows);
    CHECK(std::holds_alternative<duql::syntax::Duration>(over.width->node));
}

TEST_CASE("parse - over stays a field name outside a call") {
    const auto q = parsed("where over > 1");
    const auto& gt = std::get<duql::syntax::Binary>(where_condition(q).node);
    CHECK(std::get<duql::syntax::Path>(gt.left->node).steps[0].key == "over");
}

TEST_CASE("parse - malformed over is refused") {
    CHECK_FALSE(
        duql::syntax::parse("window { m = max(dur) over }").has_value());
    CHECK_FALSE(
        duql::syntax::parse("window { m = max(dur) over 10 row }").has_value());
    CHECK_FALSE(
        duql::syntax::parse("window { m = max(dur) over rows }").has_value());
}

TEST_CASE("parse - case block is the positional case") {
    CHECK(duql::syntax::equal(
        parsed(
            R"(derive s = case { x > 1 => "a", x > 0 => "b", else => "c" })"),
        parsed(R"(derive s = case(x > 1, "a", x > 0, "b", "c"))")));
    CHECK(duql::syntax::equal(parsed(R"(derive s = CASE { x > 1 => "a" })"),
                              parsed(R"(derive s = case(x > 1, "a", null))")));
    CHECK(kinds("a => b")[1] == Tok::FATARROW);
    const std::vector<std::pair<const char*, const char*>> errors = {
        {"derive s = case { }", "pair"},
        {R"(derive s = case { else => 1, x > 1 => 2 })", "last"},
        {"derive s = case { x > 1 }", "'=>'"},
    };
    for (const auto& [text, want] : errors) {
        CAPTURE(std::string(text));
        auto q = duql::syntax::parse(text);
        REQUIRE_FALSE(q.has_value());
        CAPTURE(q.error().message);
        CHECK(has(q.error().message, want));
    }
}

TEST_CASE("lower - regex_replace compile errors") {
    duql::Params params;
    params.emplace("pat", duql::LiteralValue{std::string("(a)")});
    params.emplace("to", duql::LiteralValue{std::string("$1$1")});
    params.emplace("bad", duql::LiteralValue{std::string("$3")});
    CHECK(duql::parse(R"re(regex_replace(s, $pat, $to) == "aa")re", params)
              .has_value());
    auto ok = duql::parse(R"re(regex_replace(s, "(a)", "${1}x$$") == "a")re");
    CHECK_MESSAGE(ok.has_value(), (ok ? "" : ok.error().message));
    const std::vector<std::pair<const char*, const char*>> errors = {
        {R"re(regex_replace(s, "(a)", "$2") == "x")re", "group 2"},
        {R"re(regex_replace(s, "(a)", "${nope}") == "x")re", "nope"},
        {R"re(regex_replace(s, "(a)", "a$") == "x")re", "offset 1"},
        {R"re(regex_replace(s, "(a)", "$x") == "x")re", "Invalid replacement"},
        {R"re(regex_replace(s, "(a", "x") == "x")re", "Invalid pattern"},
        {R"re(regex_replace(s, "(a)\\1", "x") == "x")re", "Invalid pattern"},
        {R"re(regex_replace(s, "a", t) == "x")re", "string replacement"},
        {R"re(regex_replace(s, t, "x") == "x")re", "string pattern"},
        {R"re(regex_replace(s, "a", 5) == "x")re", "string replacement"},
        {R"re(regex_replace(s, "(a)", $bad) == "x")re", "group 3"},
        {R"re(regex_replace(s, "a") == "x")re", "regex_replace"},
    };
    for (const auto& [text, want] : errors) {
        CAPTURE(std::string(text));
        auto q = duql::parse(text, params);
        REQUIRE_FALSE(q.has_value());
        CAPTURE(q.error().message);
        CHECK(has(q.error().message, want));
    }
}

TEST_CASE("lower - an array index parameter folds to the literal step") {
    duql::Params params;
    params.emplace("n", duql::LiteralValue{std::int64_t{1}});
    params.emplace("bad", duql::LiteralValue{std::string("1")});
    auto folded = compile_pipeline(R"(where xs[$n] == "a")", params, nullptr);
    auto literal = compile_pipeline(R"(where xs[1] == "a")", {}, nullptr);
    REQUIRE(folded.has_value());
    REQUIRE(literal.has_value());
    CHECK(signature(*folded) == signature(*literal));
    CHECK(!folded->filter_text.empty());
    auto bad = compile_pipeline("where xs[$bad] == 1", params, nullptr);
    REQUIRE_FALSE(bad.has_value());
    CHECK(
        has(bad.error().message, "an array index parameter holds an integer"));
}

TEST_CASE("lower - computed index, list literal and join separator") {
    CHECK(compile_pipeline("derive v = xs[i - 1]", {}, nullptr).has_value());
    CHECK(compile_pipeline(R"(derive t = [cat, name], k = len([1, 2, 3])
                              | where contains(["read", "write"], name))",
                           {}, nullptr)
              .has_value());
    CHECK(compile_pipeline("derive v = first(sort(xs))", {}, nullptr)
              .has_value());
    CHECK(compile_pipeline("where sort(xs) is null | derive u = unique(xs)", {},
                           nullptr)
              .has_value());
    auto bad = compile_pipeline("derive v = join(xs, sep)", {}, nullptr);
    REQUIRE_FALSE(bad.has_value());
    CHECK(has(bad.error().message, "join() takes a string separator"));
}

TEST_CASE("parse - list and pattern parameters print back") {
    for (const char* q :
         {"where x in $names", "where x not in $names", "where x like $p",
          "where x not ilike $p escape \"!\""}) {
        CAPTURE(q);
        const auto a = parsed(q);
        const auto b = parsed(duql::syntax::to_text(a));
        CHECK(duql::syntax::equal(a, b));
        CHECK(duql::syntax::to_text(a).find("$") != std::string::npos);
    }
    auto list = duql::parse_param(R"(["a", 2, -3, 1.5, true])");
    REQUIRE(list.has_value());
    CHECK(std::get<std::vector<duql::LiteralValue>>(*list).size() == 5);
    auto scalar = duql::parse_param("7");
    REQUIRE(scalar.has_value());
    CHECK(std::holds_alternative<duql::LiteralValue>(*scalar));
    CHECK_FALSE(duql::parse_param("[a]").has_value());
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
        R"re(parse name ~ "(?<a>x)")re",
        "from data | io_rate(1ms)",
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

TEST_CASE("pipeline macros round trip") {
    for (
        const char* q :
        {R"re(def io_rate(d) = where cat == "POSIX" | bucket d | agg { b = sum(size) }; from data | io_rate(1ms) | sort b)re",
         R"re(def pos = where cat == "POSIX"; from data | pos())re",
         R"re(def rate(d) = pos() | bucket d | agg { n = count() }; from data | rate(1ms))re",
         "source s { data = where x > 1; def f(a) = where x > a | take 3 } "
         "from data | f(1)",
         "from data | io_rate(1ms) | sort b", "io_rate(1ms) | sort b",
         "exists(x) | sort ts"}) {
        CAPTURE(q);
        const auto a = parsed(q);
        const auto printed = duql::syntax::to_text(a);
        CAPTURE(printed);
        CHECK(duql::syntax::equal(a, parsed(printed)));
    }
    const auto leading = parsed("exists(x) | sort ts");
    REQUIRE(leading.pipeline);
    CHECK(std::holds_alternative<duql::syntax::Use>(
        leading.pipeline->stages.front().node));
    CHECK(duql::syntax::to_text(*leading.pipeline) == "exists(x) | sort ts");
    CHECK(std::holds_alternative<duql::syntax::Where>(
        parsed("x.y(1)").pipeline->stages.front().node));
}

TEST_CASE("pipeline macros reject bad syntax") {
    const std::vector<std::pair<const char*, const char*>> errors = {
        {"def m = from data | where x; where y", "not 'from'"},
        {"where x | foo", "Expected a stage after '|'"},
        {"where x | a.b(1)", "Expected a stage after '|'"},
    };
    for (const auto& [text, want] : errors) {
        CAPTURE(std::string(text));
        auto p = duql::syntax::parse(text);
        REQUIRE_FALSE(p.has_value());
        CHECK(has(p.error().message, want));
    }
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
    REQUIRE(a->within);
    CHECK(duql::syntax::to_text(*a->within) == "$w");

    const auto d = parsed("lookup s on pid asof ts");
    const auto* b =
        std::get_if<duql::syntax::AsofLookup>(&d.pipeline->stages[0].node);
    REQUIRE(b);
    CHECK(b->direction == duql::syntax::AsofDirection::BACKWARD);
    CHECK_FALSE(b->within);

    const auto both = duql::syntax::parse("lookup s on pid asof ts into m");
    REQUIRE_FALSE(both.has_value());
    CHECK(both.error().format().find("cannot be combined") !=
          std::string::npos);
    for (const char* w : {"5ms", "1.5", "$w", "-1"}) {
        const auto e =
            parsed(std::string("lookup s on pid asof ts within ") + w);
        const auto* x =
            std::get_if<duql::syntax::AsofLookup>(&e.pipeline->stages[0].node);
        REQUIRE(x);
        REQUIRE(x->within);
        CHECK(duql::syntax::to_text(*x->within) == w);
    }
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

TEST_CASE("parse - lookup kinds and inline sides print back") {
    for (const char* text :
         {"lookup runs on run inner", "lookup runs on run inner into r",
          "lookup runs on run anti", "lookup runs on run ANTI",
          "lookup (from runs | select run) on run",
          "lookup (from runs) on run inner into r",
          "lookup (from runs) on run anti",
          "lookup (from s) on pid asof ts forward",
          "lookup (from s) on pid overlap into m"}) {
        const auto a = parsed(text);
        const auto b = parsed(duql::syntax::to_text(a));
        CHECK(duql::syntax::equal(a, b));
    }
    const auto q = parsed("lookup (from runs) on run anti");
    const auto* l =
        std::get_if<duql::syntax::Lookup>(&q.pipeline->stages[0].node);
    REQUIRE(l);
    CHECK(l->kind == duql::syntax::LookupKind::ANTI);
    CHECK(l->rowset.empty());
    CHECK(l->side);
    CHECK_FALSE(duql::syntax::equal(parsed("lookup r on k inner"),
                                    parsed("lookup r on k anti")));
    CHECK_FALSE(duql::syntax::equal(parsed("lookup r on k"),
                                    parsed("lookup r on k inner")));
}

TEST_CASE("parse - lookup kind errors") {
    const auto message = [](const char* text) {
        const auto r = duql::syntax::parse(text);
        REQUIRE_FALSE(r.has_value());
        return r.error().format();
    };
    CHECK(message("lookup r on k anti into x")
              .find("'anti' adds no column, so it takes no 'into'") !=
          std::string::npos);
    for (const char* t :
         {"lookup r on k inner asof ts", "lookup r on k anti overlap"})
        CHECK(message(t).find("'inner' and 'anti' do not combine with "
                              "'asof' or 'overlap'") != std::string::npos);
}

TEST_CASE("parse - inner and anti stay field names") {
    CHECK(duql::syntax::equal(parsed("where inner == 1 and anti > 2"),
                              parsed("where inner == 1 and anti > 2")));
    const auto q = parsed("derive anti = inner + 1");
    CHECK(q.pipeline->stages.size() == 1);
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

TEST_CASE("parse - syntax, printing and errors") {
    for (const char* text :
         {R"re(parse name ~ "(?<op>\\w+)")re",
          R"re(parse args.fname ~ "(?<ext>[a-z]+)$")re", "parse name ~ $p"}) {
        INFO(text);
        const auto q = parsed(text);
        const std::string printed = duql::syntax::to_text(q);
        CHECK(printed == std::string("duql 1\n") + text + "\n");
        CHECK(duql::syntax::equal(q, parsed(printed)));
    }
    const auto dotted = parsed(R"re(parse args.fname ~ "(?<a>x)")re");
    const auto* p =
        std::get_if<duql::syntax::Parse>(&dotted.pipeline->stages[0].node);
    REQUIRE(p);
    CHECK(p->column.steps.size() == 2);

    const std::vector<std::pair<const char*, const char*>> errors = {
        {R"re(parse name ~* "(?<a>x)")re", "~"},
        {R"re(parse name !~ "(?<a>x)")re", "!~"},
        {R"re(parse name !~* "(?<a>x)")re", "!~*"},
        {R"re(parse name "(?<a>x)")re", "Expected '~'"},
        {"parse name", "Expected '~'"},
        {"parse name ~", "pattern"},
        {"parse name ~ 5", "pattern"},
    };
    for (const auto& [text, want] : errors) {
        CAPTURE(std::string(text));
        auto q = duql::syntax::parse(text);
        REQUIRE_FALSE(q.has_value());
        CAPTURE(q.error().message);
        CHECK(has(q.error().message, want));
    }
}

TEST_CASE("parse - lowers to a derive of extract per named group") {
    auto p = compile_pipeline(
        R"re(parse name ~ "(?<op>[a-z]+)(\d*)_(?<fd>\d+)")re", {}, nullptr);
    REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().format()));
    REQUIRE(p->stages.size() == 1);
    const auto& d = std::get<duql::PipelineDerive>(p->stages[0]);
    REQUIRE(d.items.size() == 2);
    CHECK(d.items[0].name == "op");
    CHECK(d.items[1].name == "fd");
    CHECK(has(d.items[0].text, "extract"));
    CHECK(has(d.items[1].text, "3"));

    duql::Params params;
    params.emplace("p", duql::LiteralValue{std::string("(?<a>x)")});
    CHECK(compile_pipeline("parse name ~ $p", params, nullptr).has_value());

    duql::Params number;
    number.emplace("p", duql::LiteralValue{std::uint64_t{5}});
    const std::vector<std::pair<const char*, const char*>> errors = {
        {R"re(parse name ~ "(\\w+)_(\\d+)")re", "named group"},
        {R"re(parse name ~ "(?<a>x")re", "Invalid pattern"},
        {"parse name ~ $p", "string pattern"},
    };
    for (const auto& [text, want] : errors) {
        CAPTURE(std::string(text));
        auto q = compile_pipeline(text, number, nullptr);
        REQUIRE_FALSE(q.has_value());
        CAPTURE(q.error().message);
        CHECK(has(q.error().message, want));
    }
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
        {"window name { s = sketch(dur) }", "does not run"},
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
        {"derive s = slice(a)", "'slice' takes 2 to 3 arguments, got 1"},
        {"group k { a = arg_max(x) }", "'arg_max' takes 2 arguments, got 1"},
        {"group k { q = quantile(merge(s, t), 0.5) }", "'merge'"},
        {"derive m = merge(s)", "aggregate"},
        {"where x > 1 | union (from t)", "not a let or a row set"},
        {"where x > 1 | call myplug.sessions(gap = 5)",
         "Unknown function 'myplug.sessions'; no plugin is loaded"},
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

TEST_CASE("pipeline macros splice at a stage") {
    const std::string rate =
        "def io_rate(d) = where cat == \"POSIX\" | bucket d | agg { b = "
        "sum(size) }; ";
    const std::string hand =
        "where cat == \"POSIX\" | bucket 1ms | agg { b = sum(size) } | sort b";
    CHECK(compiled(rate + "where dur > 0 | io_rate(1ms) | sort b") ==
          compiled("where dur > 0 | " + hand));
    CHECK(compiled(rate + "io_rate(1ms) | sort b") == compiled(hand));
    CHECK(
        compiled(
            "def top(n) = sort -dur | where dur > n; where dur > 1 | top(3)") ==
        compiled("where dur > 1 | sort -dur | where dur > 3"));
    CHECK(compiled("def a = where dur > 1; where x == 1 | a() | a()") ==
          compiled("where x == 1 | where dur > 1 | where dur > 1"));
}

TEST_CASE("a leading call that names no pipeline macro is a filter") {
    CHECK(compiled("def slow(t) = dur > t; slow(250) | group name { n = "
                   "count() }") ==
          compiled("where dur > 250 | group name { n = count() }"));
    CHECK(compiled("exists(x) | sort ts") ==
          compiled("where exists(x) | sort ts"));
}

TEST_CASE("pipeline macros nest") {
    CHECK(compiled("def pos = where cat == \"POSIX\"; def rate(d) = pos() | "
                   "bucket d | agg { n = count() }; where dur > 0 | "
                   "rate(1ms)") ==
          compiled("where dur > 0 | where cat == \"POSIX\" | bucket 1ms | agg "
                   "{ n = count() }"));
    CHECK(compiled("def pos = where cat == \"POSIX\"; def rate(d) = pos() | "
                   "bucket d | agg { n = count() }; rate(1ms)") ==
          compiled("where cat == \"POSIX\" | bucket 1ms | agg { n = count() "
                   "}"));
    CHECK(compiled("def slow(t) = dur > t; def f(t) = where slow(t) | take 1; "
                   "where x == 1 | f(5)") ==
          compiled("where x == 1 | where dur > 5 | take 1"));
}

TEST_CASE("pipeline macros expand in every pipeline") {
    CHECK(compiled("def pos = where cat == \"POSIX\"; let r = pos() | take 1; "
                   "where x == 1 | lookup (from r) on x") ==
          compiled("let r = where cat == \"POSIX\" | take 1; where x == 1 | "
                   "lookup (from r) on x"));
    CHECK(compiled("def pos = where cat == \"POSIX\"; let r = where y > 0; "
                   "where x == 1 | lookup (from r | pos() | take 1) on x") ==
          compiled("let r = where y > 0; where x == 1 | lookup (from r | where "
                   "cat == \"POSIX\" | take 1) on x"));
    CHECK(compiled("def pos = where cat == \"POSIX\"; let r = where y > 0; "
                   "where x == 1 | union (from r | pos())") ==
          compiled("let r = where y > 0; where x == 1 | union (from r | where "
                   "cat == \"POSIX\")"));
    CHECK(compiled("def pos = where cat == \"POSIX\"; let r = where y > 0; "
                   "where x in (from r | pos() | select y)") ==
          compiled("let r = where y > 0; where x in (from r | where cat == "
                   "\"POSIX\" | select y)"));
}

TEST_CASE("pipeline macro errors") {
    const std::string defs =
        "def slow(t) = dur > t; def io(d) = where dur > d | take 1; ";
    const std::vector<std::pair<std::string, const char*>> errors = {
        {defs + "where io(1)", "macro 'io' is a pipeline; call it as a stage"},
        {defs + "where x > 0 | derive y = io(1)",
         "macro 'io' is a pipeline; call it as a stage"},
        {defs + "from data | slow(250)",
         "macro 'slow' is an expression; write 'where slow(...)'"},
        {defs + "where x > 0 | slow(250)", "is an expression"},
        {defs + "where x > 0 | nothing(1)",
         "Unknown stage or pipeline macro 'nothing'"},
        {defs + "from data | io(1, 2)", "takes 1 argument(s), got 2"},
        {defs + "from data | io(d = 1)", "named argument"},
        {"def sort(x) = x > 1; where y > 0", "has the name of a stage"},
        {"def a(x) = b(x) | take 1; def b(x) = a(x) | take 1; where y > 0 | "
         "a(1)",
         "macros call each other: a -> b -> a"},
        {"def a(x) = where y > 0 | b(x); def b(x) = take 1 | a(x); from d | "
         "a(1)",
         "a -> b -> a"},
        {"def f(x) = sort x | slow2(x); def slow2(t) = dur > t; where y > 0 | "
         "f(1)",
         "is an expression"},
    };
    for (const auto& [text, want] : errors) {
        CAPTURE(text);
        const std::string message = compile_error(text);
        CAPTURE(message);
        CHECK(has(message, want));
    }
    auto from_body = duql::parse("def f = from data | take 1; where x > 0", {});
    REQUIRE_FALSE(from_body.has_value());
    CHECK(has(from_body.error().message, "not 'from'"));
}

TEST_CASE("pipeline macros print back to the same program") {
    for (const char* text :
         {"def io(d) = where dur > d | bucket d | agg { b = sum(size) }; "
          "where x > 0 | io(1ms) | sort b",
          "def pos = where cat == \"POSIX\"; def r(d) = pos() | where dur > d; "
          "r(3)"}) {
        CAPTURE(text);
        auto a = duql::syntax::parse(text);
        REQUIRE(a.has_value());
        const std::string printed = duql::syntax::to_text(*a);
        auto b = duql::syntax::parse(printed);
        REQUIRE_MESSAGE(b.has_value(), printed);
        CHECK(duql::syntax::to_text(*b) == printed);
    }
}

TEST_CASE("source pipeline macros expand in a query") {
    auto p =
        duql::compile_program("where dur > 0 | fast(5) | take 2", {}, nullptr,
                              "def fast(t) = where dur < t | sort -dur");
    REQUIRE_MESSAGE(p.has_value(), (p ? "" : p.error().format()));
    auto want = compile_pipeline(
        "where dur > 0 | where dur < 5 | sort -dur | "
        "take 2",
        {}, nullptr);
    REQUIRE(want.has_value());
    CHECK(signature(p->main) == signature(*want));
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

namespace {

dftu_series* plug_unary(const dftu_series*) { return nullptr; }
dftu_series* plug_binary(const dftu_series*, const dftu_series*) {
    return nullptr;
}
dftu_dataframe* plug_frame(const dftu_dataframe*, int64_t) { return nullptr; }
double plug_total(const dftu_series*, double) { return 0; }

struct PluginOps {
    PluginOps() {
        const dftu_op_desc ops[] = {
            {"pa.unary", DFTU_OP_SIG(SERIES, SERIES, NONE, NONE),
             reinterpret_cast<const void*>(&plug_unary)},
            {"pb.binary", DFTU_OP_SIG(SERIES, SERIES, SERIES, NONE),
             reinterpret_cast<const void*>(&plug_binary)},
            {"pa.frame", DFTU_OP_SIG(FRAME, FRAME, I64, NONE),
             reinterpret_cast<const void*>(&plug_frame)},
            {"pa.total", DFTU_OP_SIG(F64, SERIES, NONE, NONE),
             reinterpret_cast<const void*>(&plug_total)},
            {"pa.scaled", DFTU_OP_SIG(F64, SERIES, F64, NONE),
             reinterpret_cast<const void*>(&plug_total)}};
        for (const auto& op : ops) REQUIRE(dftu_op_register(&op) == 0);
    }
    ~PluginOps() {
        for (const char* n :
             {"pa.unary", "pb.binary", "pa.frame", "pa.total", "pa.scaled"})
            dftu_op_unregister(n);
    }
};

}  // namespace

TEST_CASE("pipeline - plugin reducers lower to plugin aggregates") {
    PluginOps ops;
    duql::Params params;
    params.emplace("k", duql::LiteralValue{std::uint64_t{7}});

    auto one = compile_pipeline("group cat { t = pa.total(dur) }", {}, nullptr);
    REQUIRE_MESSAGE(one.has_value(),
                    (one ? std::string() : one.error().format()));
    const auto& g = std::get<duql::PipelineGroup>(one->stages.back());
    REQUIRE(g.aggs.size() == 1);
    CHECK(g.aggs[0].name == "t");
    CHECK(g.aggs[0].fn == duql::AggFn::PLUGIN);
    CHECK(g.aggs[0].plugin == "pa.total");
    CHECK(g.aggs[0].params.empty());
    CHECK(g.aggs[0].text == "pa.total(dur)");

    auto ops2 = compile_pipeline(
        "agg { s = pa.scaled(dur, 2.5, $k), r = pa.total(dur) / count() }",
        params, nullptr);
    REQUIRE_MESSAGE(ops2.has_value(),
                    (ops2 ? std::string() : ops2.error().format()));
    const auto& a = std::get<duql::PipelineGroup>(ops2->stages[0]);
    REQUIRE(a.aggs.size() == 3);
    CHECK(a.aggs[0].plugin == "pa.scaled");
    CHECK(a.aggs[0].params.size() == 2);
    CHECK(a.aggs[1].fn == duql::AggFn::PLUGIN);
    CHECK(a.aggs[1].name == "r");
    CHECK(a.aggs[2].fn == duql::AggFn::COUNT);
    CHECK(std::holds_alternative<duql::PipelineDerive>(ops2->stages[1]));

    const std::pair<std::string_view, std::string_view> cases[] = {
        {"derive y = pa.total(x)", "'group' or 'agg'"},
        {"where pa.total(x) > 1", "'group' or 'agg'"},
        {"select y = pa.total(x)", "'group' or 'agg'"},
        {"window { w = pa.total(x) }", "'group' or 'agg'"},
        {"pivot cat { n = pa.total(x) }", "'group' or 'agg'"},
        {"agg { r = pa.total() }", "column as its first"},
        {"agg { r = pa.total(x, n = 1) }", "no named argument"},
        {"agg { r = pa.total(5ms) }", "duration"},
        {"agg { r = pa.scaled(x, y) }", "number literal or a parameter"},
        {"agg { r = pa.scaled(x, \"s\") }", "is a number"},
    };
    for (const auto& [text, want] : cases) {
        CAPTURE(std::string(text));
        auto r = compile_pipeline(text, {}, nullptr);
        REQUIRE_FALSE(r.has_value());
        CAPTURE(r.error().message);
        CHECK(has(r.error().message, want));
    }
}

TEST_CASE("pipeline - plugin calls lower to plugin stages") {
    auto p = compile_pipeline(
        "where x > 1 | derive a = pa.unary(x) + 1, b = pa.unary(a)", {},
        nullptr);
    REQUIRE_FALSE(p.has_value());
    CHECK(has(p.error().message, "Unknown function 'pa.unary'; no plugin"));

    PluginOps ops;
    auto d = compile_pipeline(
        "where x > 1 | derive a = pa.unary(x) + 1, b = pa.unary(a)", {},
        nullptr);
    REQUIRE_MESSAGE(d.has_value(), (d ? std::string() : d.error().format()));
    REQUIRE(d->stages.size() == 5);
    const auto& first = std::get<duql::PipelinePlugin>(d->stages[0]);
    CHECK(first.op == "pa.unary");
    CHECK(first.arg.name == "__duql_p_0");
    CHECK(first.arg.text == "x");
    CHECK(std::get<duql::PipelineDerive>(d->stages[1]).items[0].name == "a");
    const auto& second = std::get<duql::PipelinePlugin>(d->stages[2]);
    CHECK(second.arg.name == "__duql_p_1");
    CHECK(second.arg.text == "a");
    CHECK(std::get<duql::PipelineDerive>(d->stages[3]).items[0].name == "b");
    CHECK(std::get<duql::PipelineDrop>(d->stages[4]).names ==
          std::vector<std::string>{"__duql_p_0", "__duql_p_1"});

    auto operands = compile_pipeline(
        "select y = pb.binary(x, z), w = pa.unary(x)", {}, nullptr);
    REQUIRE_MESSAGE(operands.has_value(),
                    (operands ? std::string() : operands.error().format()));
    REQUIRE(operands->stages.size() == 3);
    const auto& bin = std::get<duql::PipelinePlugin>(operands->stages[0]);
    REQUIRE(bin.arg2.has_value());
    CHECK(bin.arg2->name == "__duql_p_0_b");
    CHECK(bin.arg2->text == "z");
    CHECK(std::holds_alternative<duql::PipelineSelect>(operands->stages[2]));

    duql::Params params;
    params.emplace("k", duql::LiteralValue{std::uint64_t{7}});
    auto lits = compile_pipeline("derive y = pa.unary(x, \"s\", 2, $k, -1.5)",
                                 params, nullptr);
    REQUIRE_FALSE(lits.has_value());
    CHECK(has(lits.error().message, "at most 2 number"));
    auto ok = compile_pipeline("derive y = pa.unary(x, \"s\", 2, $k)", params,
                               nullptr);
    REQUIRE_MESSAGE(ok.has_value(), (ok ? std::string() : ok.error().format()));
    const auto& lit = std::get<duql::PipelinePlugin>(ok->stages[0]);
    CHECK(lit.str == "s");
    CHECK(lit.scalars.size() == 2);

    auto call = compile_pipeline("where x > 1 | call pa.frame(5)", {}, nullptr);
    REQUIRE_MESSAGE(call.has_value(),
                    (call ? std::string() : call.error().format()));
    const auto& c = std::get<duql::PipelineCall>(call->stages.back());
    CHECK(c.op == "pa.frame");
    CHECK(c.args.size() == 1);

    const std::pair<std::string_view, std::string_view> cases[] = {
        {"where pa.unary(x) > 1", "'derive' or 'select'"},
        {"group pa.unary(x) { n = count() }", "'derive' or 'select'"},
        {"group k { n = sum(pa.unary(x)) }", "'derive' or 'select'"},
        {"derive y = pa.unary(pa.unary(x))", "'derive' or 'select'"},
        {"let s = derive y = pa.unary(x); from data | take 1", "sub-query"},
        {"where x in (from data | derive y = pa.unary(x) | select y)",
         "sub-query"},
        {"derive y = nope.f(x)", "loaded plugin namespaces: pa, pb"},
        {"derive y = pa.unary(x, 1, 2, 3)", "at most 2 number"},
        {"derive y = pa.unary(x, z, w)", "one second column"},
        {"derive y = pa.unary(x, \"a\", \"b\")", "one text argument"},
        {"derive y = pa.unary(x, n = 1)", "no named argument"},
        {"derive y = pa.unary(x, 5ms)", "duration"},
        {"derive y = pa.unary()", "column as its first"},
        {"derive y = pa.frame(x)", "not a column function"},
        {"call pa.unary(1)", "not a table function"},
        {"call pa.frame(x)", "literal or a parameter"},
        {"call pa.frame(5) | take 1", "after 'call'"},
        {"call pa.frame(5) | union (from data)", "after 'call'"},
        {"call nope.g(1)", "loaded plugin namespaces: pa, pb"},
    };
    for (const auto& [text, want] : cases) {
        CAPTURE(std::string(text));
        auto r = compile_pipeline(text, {}, nullptr);
        REQUIRE_FALSE(r.has_value());
        CAPTURE(r.error().message);
        CHECK(has(r.error().message, want));
    }
}
