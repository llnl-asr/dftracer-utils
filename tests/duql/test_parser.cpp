#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/duql/parser.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::duql;

TEST_CASE("parse - simple comparison") {
    auto result = parse(R"(cat == "POSIX")");
    REQUIRE(result.has_value());
    auto& node = **result;
    CHECK(std::holds_alternative<CompareNode>(node.data));
    auto& cmp = std::get<CompareNode>(node.data);
    CHECK(cmp.field.path == "cat");
    CHECK(cmp.op == CompareOp::EQ);
    CHECK(std::get<std::string>(cmp.value.value) == "POSIX");
}

TEST_CASE("parse - and expression") {
    auto result = parse(R"(cat == "POSIX" and dur > 1000)");
    REQUIRE(result.has_value());
    CHECK(std::holds_alternative<AndNode>((*result)->data));
}

TEST_CASE("parse - or expression") {
    auto result = parse(R"(cat == "POSIX" or cat == "STDIO")");
    REQUIRE(result.has_value());
    CHECK(std::holds_alternative<OrNode>((*result)->data));
}

TEST_CASE("parse - not expression") {
    auto result = parse(R"(not cat == "POSIX")");
    REQUIRE(result.has_value());
    CHECK(std::holds_alternative<NotNode>((*result)->data));
}

TEST_CASE("parse - in expression") {
    auto result = parse(R"(cat in ["POSIX", "STDIO"])");
    REQUIRE(result.has_value());
    auto& node = **result;
    CHECK(std::holds_alternative<InNode>(node.data));
    auto& in = std::get<InNode>(node.data);
    CHECK(in.field.path == "cat");
    CHECK(in.values.elements.size() == 2);
}

TEST_CASE("parse - not in expression") {
    auto result = parse(R"(cat not in ["POSIX"])");
    REQUIRE(result.has_value());
    CHECK(std::holds_alternative<NotInNode>((*result)->data));
}

TEST_CASE("parse - precedence: and binds tighter than or") {
    auto result = parse(R"(a == 1 or b == 2 and c == 3)");
    REQUIRE(result.has_value());
    // Should parse as: a == 1 or (b == 2 and c == 3)
    CHECK(std::holds_alternative<OrNode>((*result)->data));
    auto& or_node = std::get<OrNode>((*result)->data);
    CHECK(std::holds_alternative<CompareNode>(or_node.left->data));
    CHECK(std::holds_alternative<AndNode>(or_node.right->data));
}

TEST_CASE("parse - parenthesized grouping") {
    auto result = parse(R"((a == 1 or b == 2) and c == 3)");
    REQUIRE(result.has_value());
    CHECK(std::holds_alternative<AndNode>((*result)->data));
    auto& and_node = std::get<AndNode>((*result)->data);
    CHECK(std::holds_alternative<OrNode>(and_node.left->data));
}

TEST_CASE("parse - case-insensitive keywords") {
    auto result = parse(R"(a == 1 AND b == 2 OR c == 3)");
    REQUIRE(result.has_value());
    CHECK(std::holds_alternative<OrNode>((*result)->data));
}

TEST_CASE("parse - error reporting") {
    auto result = parse(R"(cat ==)");
    REQUIRE_FALSE(result.has_value());
    auto& err = result.error();
    CHECK(err.column == 6);
    auto formatted = err.format();
    CHECK(formatted.find("column 6") != std::string::npos);
}

TEST_CASE("to_string round-trip") {
    auto result = parse(R"(cat == "POSIX" and dur > 1000)");
    REQUIRE(result.has_value());
    auto str = to_string(**result);
    CHECK(str == R"((cat == "POSIX" and dur > 1000))");
}

TEST_CASE("parse - like / ilike / regex nodes") {
    for (const char* q :
         {R"(name like "%Send%")", R"(name ilike "%send%")", R"(name ~ "Send")",
          R"(name ~* "send")", R"(name !~ "Send")", R"(name not like "%x%")"}) {
        auto result = parse(q);
        REQUIRE_MESSAGE(result.has_value(), q);
        CHECK(std::holds_alternative<MatchNode>((*result)->data));
    }
}

TEST_CASE("parse - substring 'sub' in field") {
    auto result = parse(R"('send' in name)");
    REQUIRE(result.has_value());
    auto& node = **result;
    REQUIRE(std::holds_alternative<MatchNode>(node.data));
    auto& m = std::get<MatchNode>(node.data);
    CHECK(m.field.path == "name");
    CHECK(m.op == MatchOp::ICONTAINS);
    CHECK_FALSE(m.negated);
    CHECK(m.pattern == "send");
}

TEST_CASE("parse - substring 'sub' not in field") {
    auto result = parse(R"('send' not in name)");
    REQUIRE(result.has_value());
    auto& m = std::get<MatchNode>((*result)->data);
    CHECK(m.op == MatchOp::ICONTAINS);
    CHECK(m.negated);
}

TEST_CASE("parse - field in [array] still membership") {
    auto result = parse(R"(name in ["a", "b"])");
    REQUIRE(result.has_value());
    CHECK(std::holds_alternative<InNode>((*result)->data));
}

TEST_CASE("parse - invalid regex reports error") {
    auto result = parse(R"(name ~ "(unclosed")");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("Invalid pattern") != std::string::npos);
}

TEST_CASE("to_string round-trip - patterns") {
    for (const char* q :
         {R"(name like "%Send%")", R"(name ilike "%s%")", R"(name ~ "Send")",
          R"(name ~* "s")", R"(name !~ "Send")", R"(name not like "%x%")",
          R"("send" in name)", R"("send" not in name)"}) {
        auto result = parse(q);
        REQUIRE_MESSAGE(result.has_value(), q);
        CHECK(to_string(**result) == q);
    }
}

TEST_CASE("parse - any(path) with every field operator round-trips") {
    for (const char* text :
         {R"(any(tags) == "a")", R"(any(tags) != "a")", "any(sizes) > 10",
          "any(sizes) <= 10", R"(any(tags) in ["a", "b"])",
          R"(any(tags) not in ["a", "b"])", R"(any(tags) like "a%")",
          R"(any(tags) ~ "^a")", R"("sub" in any(tags))",
          R"((any(tags) == "a" and not (x == 1)))"}) {
        auto ast = parse(text);
        INFO(std::string(text));
        CHECK_MESSAGE(ast.has_value(),
                      (ast ? std::string() : ast.error().format()));
        if (ast) CHECK(to_string(**ast) == text);
    }
    auto ast = parse(R"(any(args.tags) == "a")");
    REQUIRE(ast.has_value());
    const auto& cmp = std::get<CompareNode>((*ast)->data);
    CHECK(cmp.field.any);
    CHECK(cmp.field.path == "args.tags");
}

TEST_CASE("parse - a field named any still parses") {
    auto ast = parse("any == 1");
    REQUIRE(ast.has_value());
    const auto& cmp = std::get<CompareNode>((*ast)->data);
    CHECK_FALSE(cmp.field.any);
    CHECK(cmp.field.path == "any");
    CHECK_FALSE(parse("any( == 1").has_value());
    CHECK_FALSE(parse("any(tags == 1").has_value());
}

TEST_CASE("field_node reads the any form") {
    CHECK(field_node("any(tags)").any);
    CHECK(field_node("any(tags)").path == "tags");
    CHECK_FALSE(field_node("tags").any);
    CHECK_FALSE(field_node("any()").any);
    CHECK(field_text(field_node("any(a.b)")) == "any(a.b)");
}
