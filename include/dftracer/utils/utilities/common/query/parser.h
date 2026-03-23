#ifndef DFTRACER_UTILS_UTILITIES_COMMON_QUERY_PARSER_H
#define DFTRACER_UTILS_UTILITIES_COMMON_QUERY_PARSER_H

#include <dftracer/utils/core/common/expected.h>
#include <dftracer/utils/utilities/common/query/ast.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::common::query {

/// Structured parse error with source location.
struct QueryError {
    std::string message;     ///< Error description.
    std::size_t column = 0;  ///< Column position (0-indexed).
    std::string source;      ///< Original query string.
    std::string indicator;   ///< Caret/tilde indicator line.

    /// Format as multi-line error message with source and indicator.
    std::string format() const;
};

/// Exception wrapping a QueryError.
class QueryParseError : public std::runtime_error {
   public:
    explicit QueryParseError(QueryError err);
    const QueryError& error() const { return err_; }

   private:
    QueryError err_;
};

enum class TokenKind {
    IDENT,
    STRING,
    INT,
    FLOAT,
    BOOL,
    OP_EQ,
    OP_NE,
    OP_GT,
    OP_LT,
    OP_GE,
    OP_LE,
    KW_AND,
    KW_OR,
    KW_NOT,
    KW_IN,
    KW_TRUE,
    KW_FALSE,
    LPAREN,
    RPAREN,
    LBRACKET,
    RBRACKET,
    COMMA,
    END,
};

struct Token {
    TokenKind kind;
    std::string_view text;
    std::size_t column;
};

/// Tokenize a query string. Keywords are case-insensitive.
dftracer::utils::expected<std::vector<Token>, QueryError> tokenize(
    std::string_view input);

/// Parse a pre-tokenized stream into an AST.
dftracer::utils::expected<QueryNodePtr, QueryError> parse_tokens(
    const std::vector<Token>& tokens, std::string_view source);

/// Tokenize and parse a query string into an AST.
dftracer::utils::expected<QueryNodePtr, QueryError> parse(
    std::string_view input);

}  // namespace dftracer::utils::utilities::common::query

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_QUERY_PARSER_H
