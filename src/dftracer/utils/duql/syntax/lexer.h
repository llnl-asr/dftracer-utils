#ifndef DFTRACER_UTILS_DUQL_SYNTAX_LEXER_H
#define DFTRACER_UTILS_DUQL_SYNTAX_LEXER_H

#include <dftracer/utils/core/common/expected.h>
#include <dftracer/utils/duql/parser.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace dftracer::utils::duql::syntax {

enum class Tok : std::uint8_t {
    NAME,      ///< identifier; keywords are names the parser recognizes
    QNAME,     ///< backtick-quoted key, text without the backticks
    INT,       ///< digits
    FLOAT,     ///< digits with a fraction or an exponent
    DURATION,  ///< a number directly followed by ns, us, ms, s, m or h
    STRING,    ///< text between the quotes, escapes kept as written
    PARAM,     ///< `$name`, text without the `$`
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE,
    REGEX,
    IREGEX,
    NREGEX,
    NIREGEX,
    PLUS,
    MINUS,
    STAR,
    SLASH,
    SLASH2,
    PERCENT,
    COALESCE,
    ARROW,
    DOT,
    DOTDOT,
    CARET,
    ASSIGN,
    FATARROW,
    PIPE,
    COMMA,
    SEMI,
    LPAREN,
    RPAREN,
    LBRACKET,
    RBRACKET,
    LBRACE,
    RBRACE,
    END,
};

struct Token {
    Tok kind;
    std::string_view text;
    std::size_t offset;
    /// No whitespace or comment between this token and the previous one.
    bool adjacent;
};

/// The tokens of `source`, ending with END. `#` comments and whitespace are
/// skipped. Fails on an unterminated string or quoted key, or a character
/// that starts no token.
dftracer::utils::expected<std::vector<Token>, duql::DuqlError> lex(
    std::string_view source);

/// A DuqlError for `length` bytes at `offset` of `source`, with line,
/// column and a caret line.
duql::DuqlError make_error(std::string_view source, std::size_t offset,
                           std::size_t length, std::string message);

}  // namespace dftracer::utils::duql::syntax

#endif  // DFTRACER_UTILS_DUQL_SYNTAX_LEXER_H
