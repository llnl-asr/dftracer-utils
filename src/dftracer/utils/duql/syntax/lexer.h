#ifndef DFTRACER_UTILS_DUQL_SYNTAX_LEXER_H
#define DFTRACER_UTILS_DUQL_SYNTAX_LEXER_H

#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/duql/parser.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace dftracer::utils::duql::syntax {

enum class Tok : std::uint8_t {
    Name,      ///< identifier; keywords are names the parser recognizes
    Qname,     ///< backtick-quoted key, text without the backticks
    Int,       ///< digits
    Float,     ///< digits with a fraction or an exponent
    Duration,  ///< a number directly followed by ns, us, ms, s, m or h
    String,    ///< text between the quotes, escapes kept as written
    Param,     ///< `$name`, text without the `$`
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Regex,
    Iregex,
    Nregex,
    Niregex,
    Plus,
    Minus,
    Star,
    Slash,
    Slash2,
    Percent,
    Coalesce,
    Arrow,
    Dot,
    Dotdot,
    Caret,
    Assign,
    Fatarrow,
    Pipe,
    Comma,
    Semi,
    Lparen,
    Rparen,
    Lbracket,
    Rbracket,
    Lbrace,
    Rbrace,
    End,
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
dftracer::utils::dstd::Expected<std::vector<Token>, duql::DuqlError> lex(
    std::string_view source);

/// A DuqlError for `length` bytes at `offset` of `source`, with line,
/// column and a caret line.
duql::DuqlError make_error(std::string_view source, std::size_t offset,
                           std::size_t length, std::string message);

}  // namespace dftracer::utils::duql::syntax

#endif  // DFTRACER_UTILS_DUQL_SYNTAX_LEXER_H
