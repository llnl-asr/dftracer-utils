#include <dftracer/utils/duql/syntax/lexer.h>

#include <cctype>
#include <string>

namespace dftracer::utils::duql::syntax {

namespace {

bool name_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool digit(char c) { return std::isdigit(static_cast<unsigned char>(c)); }

constexpr std::string_view DURATION_UNITS[] = {"ns", "us", "ms", "s",
                                               "m",  "h",  "d"};

}  // namespace

duql::DuqlError make_error(std::string_view source, std::size_t offset,
                           std::size_t length, std::string message) {
    if (offset > source.size()) offset = source.size();
    std::size_t line = 1;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < offset; ++i)
        if (source[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    std::size_t line_end = source.find('\n', line_start);
    if (line_end == std::string_view::npos) line_end = source.size();
    const std::size_t column = offset - line_start;
    std::string indicator(column, ' ');
    indicator += '^';
    for (std::size_t i = 1; i < length; ++i) indicator += '~';
    return duql::DuqlError{
        std::move(message), column,
        std::string(source.substr(line_start, line_end - line_start)),
        std::move(indicator), line};
}

dftracer::utils::expected<std::vector<Token>, duql::DuqlError> lex(
    std::string_view src) {
    std::vector<Token> out;
    std::size_t pos = 0;
    bool adjacent = false;
    auto push = [&](Tok kind, std::size_t start, std::size_t len) {
        out.push_back({kind, src.substr(start, len), start, adjacent});
        pos = start + len;
        adjacent = true;
    };
    auto at = [&](std::size_t i) { return i < src.size() ? src[i] : '\0'; };

    while (true) {
        while (pos < src.size()) {
            if (std::isspace(static_cast<unsigned char>(src[pos]))) {
                ++pos;
                adjacent = false;
            } else if (src[pos] == '#') {
                while (pos < src.size() && src[pos] != '\n') ++pos;
                adjacent = false;
            } else {
                break;
            }
        }
        if (pos >= src.size()) break;
        const std::size_t start = pos;
        const char c = src[pos];
        const char n = at(pos + 1);

        if (c == '"' || c == '\'') {
            std::size_t i = pos + 1;
            while (i < src.size() && src[i] != c) i += src[i] == '\\' ? 2 : 1;
            if (i >= src.size())
                return dftracer::utils::unexpected(
                    make_error(src, start, src.size() - start,
                               "Unterminated string literal"));
            out.push_back({Tok::STRING, src.substr(start + 1, i - start - 1),
                           start, adjacent});
            pos = i + 1;
            adjacent = true;
            continue;
        }
        if (c == '`') {
            const std::size_t close = src.find('`', pos + 1);
            if (close == std::string_view::npos || close == pos + 1)
                return dftracer::utils::unexpected(make_error(
                    src, start, 1,
                    close == std::string_view::npos ? "Unterminated quoted key"
                                                    : "Empty quoted key"));
            out.push_back({Tok::QNAME, src.substr(start + 1, close - start - 1),
                           start, adjacent});
            pos = close + 1;
            adjacent = true;
            continue;
        }
        if (digit(c)) {
            std::size_t i = pos;
            bool is_float = false;
            while (digit(at(i))) ++i;
            if (at(i) == '.' && digit(at(i + 1))) {
                is_float = true;
                ++i;
                while (digit(at(i))) ++i;
            }
            if ((at(i) == 'e' || at(i) == 'E') &&
                (digit(at(i + 1)) || ((at(i + 1) == '+' || at(i + 1) == '-') &&
                                      digit(at(i + 2))))) {
                is_float = true;
                i += 2;
                while (digit(at(i))) ++i;
            }
            std::size_t j = i;
            while (name_char(at(j))) ++j;
            if (j == i) {
                push(is_float ? Tok::FLOAT : Tok::INT, start, i - start);
                continue;
            }
            const std::string_view suffix = src.substr(i, j - i);
            bool unit = false;
            for (auto u : DURATION_UNITS) unit |= suffix == u;
            if (!unit)
                return dftracer::utils::unexpected(
                    make_error(src, start, j - start, "Invalid number"));
            push(Tok::DURATION, start, j - start);
            continue;
        }
        if (name_start(c)) {
            std::size_t i = pos;
            while (name_char(at(i))) ++i;
            push(Tok::NAME, start, i - start);
            continue;
        }
        if (c == '$' && name_start(n)) {
            std::size_t i = pos + 1;
            while (name_char(at(i))) ++i;
            out.push_back({Tok::PARAM, src.substr(start + 1, i - start - 1),
                           start, adjacent});
            pos = i;
            adjacent = true;
            continue;
        }

        switch (c) {
            case '=':
                push(n == '=' ? Tok::EQ : Tok::ASSIGN, start, n == '=' ? 2 : 1);
                continue;
            case '!':
                if (n == '=') {
                    push(Tok::NE, start, 2);
                    continue;
                }
                if (n == '~') {
                    const bool i = at(pos + 2) == '*';
                    push(i ? Tok::NIREGEX : Tok::NREGEX, start, i ? 3 : 2);
                    continue;
                }
                break;
            case '<':
                push(n == '=' ? Tok::LE : Tok::LT, start, n == '=' ? 2 : 1);
                continue;
            case '>':
                push(n == '=' ? Tok::GE : Tok::GT, start, n == '=' ? 2 : 1);
                continue;
            case '~':
                push(n == '*' ? Tok::IREGEX : Tok::REGEX, start,
                     n == '*' ? 2 : 1);
                continue;
            case '+':
                push(Tok::PLUS, start, 1);
                continue;
            case '-':
                push(n == '>' ? Tok::ARROW : Tok::MINUS, start,
                     n == '>' ? 2 : 1);
                continue;
            case '*':
                push(Tok::STAR, start, 1);
                continue;
            case '/':
                push(n == '/' ? Tok::SLASH2 : Tok::SLASH, start,
                     n == '/' ? 2 : 1);
                continue;
            case '%':
                push(Tok::PERCENT, start, 1);
                continue;
            case '?':
                if (n == '?') {
                    push(Tok::COALESCE, start, 2);
                    continue;
                }
                break;
            case '.':
                push(n == '.' ? Tok::DOTDOT : Tok::DOT, start,
                     n == '.' ? 2 : 1);
                continue;
            case '^':
                push(Tok::CARET, start, 1);
                continue;
            case '|':
                push(Tok::PIPE, start, 1);
                continue;
            case ',':
                push(Tok::COMMA, start, 1);
                continue;
            case ';':
                push(Tok::SEMI, start, 1);
                continue;
            case '(':
                push(Tok::LPAREN, start, 1);
                continue;
            case ')':
                push(Tok::RPAREN, start, 1);
                continue;
            case '[':
                push(Tok::LBRACKET, start, 1);
                continue;
            case ']':
                push(Tok::RBRACKET, start, 1);
                continue;
            case '{':
                push(Tok::LBRACE, start, 1);
                continue;
            case '}':
                push(Tok::RBRACE, start, 1);
                continue;
            default:
                break;
        }
        return dftracer::utils::unexpected(make_error(
            src, start, 1, std::string("Unexpected character '") + c + "'"));
    }
    out.push_back({Tok::END, src.substr(src.size(), 0), src.size(), adjacent});
    return out;
}

}  // namespace dftracer::utils::duql::syntax
