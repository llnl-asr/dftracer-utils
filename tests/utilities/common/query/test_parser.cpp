#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/query/ast.h>
#include <dftracer/utils/utilities/common/query/parser.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::common::query;

TEST_CASE("tokenize - simple comparison") {
    auto result = tokenize(R"(cat == "POSIX")");
    REQUIRE(result.has_value());
    auto& tokens = *result;
    REQUIRE(tokens.size() == 4);  // IDENT, OP_EQ, STRING, END
    CHECK(tokens[0].kind == TokenKind::IDENT);
    CHECK(tokens[0].text == "cat");
    CHECK(tokens[1].kind == TokenKind::OP_EQ);
    CHECK(tokens[2].kind == TokenKind::STRING);
    CHECK(tokens[2].text == "POSIX");
    CHECK(tokens[3].kind == TokenKind::END);
}

TEST_CASE("tokenize - all operators") {
    auto result =
        tokenize("a == 1 and b != 2 and c > 3 and d < 4 and e >= 5 and f <= 6");
    REQUIRE(result.has_value());
    auto& tokens = *result;
    // 6 comparisons * 3 tokens + 5 "and" + END = 24
    CHECK(tokens.size() == 24);
}

TEST_CASE("tokenize - case-insensitive keywords") {
    SUBCASE("lowercase") {
        auto result = tokenize("a == 1 and b == 2");
        REQUIRE(result.has_value());
        CHECK((*result)[3].kind == TokenKind::KW_AND);
    }
    SUBCASE("uppercase") {
        auto result = tokenize("a == 1 AND b == 2");
        REQUIRE(result.has_value());
        CHECK((*result)[3].kind == TokenKind::KW_AND);
    }
    SUBCASE("mixed case") {
        auto result = tokenize("a == 1 And b == 2 Or c == 3");
        REQUIRE(result.has_value());
        CHECK((*result)[3].kind == TokenKind::KW_AND);
        CHECK((*result)[7].kind == TokenKind::KW_OR);
    }
    SUBCASE("NOT and IN") {
        auto result = tokenize("x NOT IN [1, 2]");
        REQUIRE(result.has_value());
        CHECK((*result)[1].kind == TokenKind::KW_NOT);
        CHECK((*result)[2].kind == TokenKind::KW_IN);
    }
    SUBCASE("TRUE and FALSE") {
        auto result = tokenize("x == TRUE and y == False");
        REQUIRE(result.has_value());
        CHECK((*result)[2].kind == TokenKind::KW_TRUE);
        CHECK((*result)[6].kind == TokenKind::KW_FALSE);
    }
}

TEST_CASE("tokenize - numbers") {
    SUBCASE("integer") {
        auto result = tokenize("dur > 1000");
        REQUIRE(result.has_value());
        CHECK((*result)[2].kind == TokenKind::INT);
        CHECK((*result)[2].text == "1000");
    }
    SUBCASE("negative integer") {
        auto result = tokenize("x == -42");
        REQUIRE(result.has_value());
        CHECK((*result)[2].kind == TokenKind::INT);
        CHECK((*result)[2].text == "-42");
    }
    SUBCASE("float") {
        auto result = tokenize("x > 3.14");
        REQUIRE(result.has_value());
        CHECK((*result)[2].kind == TokenKind::FLOAT);
    }
    SUBCASE("scientific notation") {
        auto result = tokenize("x > 1e10");
        REQUIRE(result.has_value());
        CHECK((*result)[2].kind == TokenKind::FLOAT);
    }
}

TEST_CASE("tokenize - strings") {
    SUBCASE("double quoted") {
        auto result = tokenize(R"(name == "hello world")");
        REQUIRE(result.has_value());
        CHECK((*result)[2].kind == TokenKind::STRING);
        CHECK((*result)[2].text == "hello world");
    }
    SUBCASE("single quoted") {
        auto result = tokenize("name == 'hello'");
        REQUIRE(result.has_value());
        CHECK((*result)[2].kind == TokenKind::STRING);
        CHECK((*result)[2].text == "hello");
    }
}

TEST_CASE("tokenize - dotted field names") {
    auto result = tokenize("args.level == \"DEBUG\"");
    REQUIRE(result.has_value());
    CHECK((*result)[0].kind == TokenKind::IDENT);
    CHECK((*result)[0].text == "args.level");
}

TEST_CASE("tokenize - error on unterminated string") {
    auto result = tokenize(R"(name == "hello)");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("Unterminated") != std::string::npos);
}

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
