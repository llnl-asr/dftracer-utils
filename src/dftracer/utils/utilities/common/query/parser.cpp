#include <dftracer/utils/utilities/common/query/parser.h>
#include <dftracer/utils/utilities/common/query/pattern.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <memory>
#include <regex>
#include <sstream>
#include <string>

namespace dftracer::utils::utilities::common::query {

std::string QueryError::format() const {
    std::ostringstream os;
    os << "Query parse error at column " << column << ":\n";
    os << "  " << source << '\n';
    os << "  " << indicator << '\n';
    os << "  " << message << '\n';
    return os.str();
}

QueryParseError::QueryParseError(QueryError err)
    : DFTUtilsException(ErrorCode::QUERY, err.format()), err_(std::move(err)) {}

namespace {

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
                      [](unsigned char ca, unsigned char cb) {
                          return std::tolower(ca) == std::tolower(cb);
                      });
}

QueryError make_error(std::string_view source, std::size_t col, std::size_t len,
                      std::string msg) {
    std::string ind(col, ' ');
    if (len > 0) {
        ind += '^';
        for (std::size_t i = 1; i < len; ++i) ind += '~';
    } else {
        ind += '^';
    }
    return QueryError{std::move(msg), col, std::string(source), std::move(ind)};
}

}  // namespace

dftracer::utils::expected<std::vector<Token>, QueryError> tokenize(
    std::string_view input) {
    std::vector<Token> tokens;
    std::size_t pos = 0;

    auto skip_ws = [&]() {
        while (pos < input.size() &&
               std::isspace(static_cast<unsigned char>(input[pos]))) {
            ++pos;
        }
    };

    while (true) {
        skip_ws();
        if (pos >= input.size()) break;

        std::size_t start = pos;
        char c = input[pos];

        // Operators
        if (c == '=' && pos + 1 < input.size() && input[pos + 1] == '=') {
            tokens.push_back({TokenKind::OP_EQ, input.substr(start, 2), start});
            pos += 2;
            continue;
        }
        if (c == '!' && pos + 1 < input.size() && input[pos + 1] == '=') {
            tokens.push_back({TokenKind::OP_NE, input.substr(start, 2), start});
            pos += 2;
            continue;
        }
        if (c == '!' && pos + 1 < input.size() && input[pos + 1] == '~') {
            if (pos + 2 < input.size() && input[pos + 2] == '*') {
                tokens.push_back(
                    {TokenKind::OP_NIREGEX, input.substr(start, 3), start});
                pos += 3;
            } else {
                tokens.push_back(
                    {TokenKind::OP_NREGEX, input.substr(start, 2), start});
                pos += 2;
            }
            continue;
        }
        if (c == '>' && pos + 1 < input.size() && input[pos + 1] == '=') {
            tokens.push_back({TokenKind::OP_GE, input.substr(start, 2), start});
            pos += 2;
            continue;
        }
        if (c == '<' && pos + 1 < input.size() && input[pos + 1] == '=') {
            tokens.push_back({TokenKind::OP_LE, input.substr(start, 2), start});
            pos += 2;
            continue;
        }
        if (c == '>') {
            tokens.push_back({TokenKind::OP_GT, input.substr(start, 1), start});
            ++pos;
            continue;
        }
        if (c == '<') {
            tokens.push_back({TokenKind::OP_LT, input.substr(start, 1), start});
            ++pos;
            continue;
        }
        if (c == '~') {
            if (pos + 1 < input.size() && input[pos + 1] == '*') {
                tokens.push_back(
                    {TokenKind::OP_IREGEX, input.substr(start, 2), start});
                pos += 2;
            } else {
                tokens.push_back(
                    {TokenKind::OP_REGEX, input.substr(start, 1), start});
                ++pos;
            }
            continue;
        }

        // Punctuation
        if (c == '(') {
            tokens.push_back(
                {TokenKind::LPAREN, input.substr(start, 1), start});
            ++pos;
            continue;
        }
        if (c == ')') {
            tokens.push_back(
                {TokenKind::RPAREN, input.substr(start, 1), start});
            ++pos;
            continue;
        }
        if (c == '[') {
            tokens.push_back(
                {TokenKind::LBRACKET, input.substr(start, 1), start});
            ++pos;
            continue;
        }
        if (c == ']') {
            tokens.push_back(
                {TokenKind::RBRACKET, input.substr(start, 1), start});
            ++pos;
            continue;
        }
        if (c == ',') {
            tokens.push_back({TokenKind::COMMA, input.substr(start, 1), start});
            ++pos;
            continue;
        }

        // Strings
        if (c == '"' || c == '\'') {
            char quote = c;
            ++pos;
            while (pos < input.size() && input[pos] != quote) {
                if (input[pos] == '\\' && pos + 1 < input.size()) {
                    pos += 2;
                } else {
                    ++pos;
                }
            }
            if (pos >= input.size()) {
                return dftracer::utils::unexpected(make_error(
                    input, start, pos - start, "Unterminated string literal"));
            }
            ++pos;  // consume closing quote
            tokens.push_back({TokenKind::STRING,
                              input.substr(start + 1, pos - start - 2), start});
            continue;
        }

        // Numbers (including negative)
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '-' && pos + 1 < input.size() &&
             std::isdigit(static_cast<unsigned char>(input[pos + 1])))) {
            bool is_float = false;
            ++pos;
            while (pos < input.size() &&
                   (std::isdigit(static_cast<unsigned char>(input[pos])) ||
                    input[pos] == '.' || input[pos] == 'e' ||
                    input[pos] == 'E' || input[pos] == '+' ||
                    input[pos] == '-')) {
                if (input[pos] == '.' || input[pos] == 'e' ||
                    input[pos] == 'E') {
                    is_float = true;
                }
                ++pos;
            }
            auto text = input.substr(start, pos - start);
            tokens.push_back(
                {is_float ? TokenKind::FLOAT : TokenKind::INT, text, start});
            continue;
        }

        // Identifiers and keywords
        if (is_ident_start(c)) {
            ++pos;
            while (pos < input.size() && is_ident_char(input[pos])) {
                ++pos;
            }
            auto text = input.substr(start, pos - start);
            TokenKind kind = TokenKind::IDENT;
            if (iequals(text, "and"))
                kind = TokenKind::KW_AND;
            else if (iequals(text, "or"))
                kind = TokenKind::KW_OR;
            else if (iequals(text, "not"))
                kind = TokenKind::KW_NOT;
            else if (iequals(text, "in"))
                kind = TokenKind::KW_IN;
            else if (iequals(text, "like"))
                kind = TokenKind::KW_LIKE;
            else if (iequals(text, "ilike"))
                kind = TokenKind::KW_ILIKE;
            else if (iequals(text, "true"))
                kind = TokenKind::KW_TRUE;
            else if (iequals(text, "false"))
                kind = TokenKind::KW_FALSE;
            tokens.push_back({kind, text, start});
            continue;
        }

        return dftracer::utils::unexpected(make_error(
            input, start, 1, std::string("Unexpected character '") + c + "'"));
    }

    tokens.push_back(
        {TokenKind::END, input.substr(input.size(), 0), input.size()});
    return tokens;
}

// ============================================================
// Recursive Descent Parser
// ============================================================

namespace {

void append_escaped(std::string& out, char c) {
    switch (c) {
        case '.':
        case '\\':
        case '+':
        case '*':
        case '?':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '^':
        case '$':
        case '|':
            out += '\\';
            [[fallthrough]];
        default:
            out += c;
    }
}

// Translate a SQL LIKE pattern to an anchored ECMAScript regex: '%' matches any
// run, '_' matches one char, everything else is literal.
std::string like_to_regex(const std::string& like) {
    std::string out = "^";
    for (char c : like) {
        if (c == '%') {
            out += ".*";
        } else if (c == '_') {
            out += '.';
        } else {
            append_escaped(out, c);
        }
    }
    out += '$';
    return out;
}

// Treat the whole literal as a substring to search for (unanchored).
std::string contains_to_regex(const std::string& sub) {
    std::string out;
    for (char c : sub) append_escaped(out, c);
    return out;
}

dftracer::utils::expected<std::shared_ptr<CompiledPattern>, std::string>
compile_pattern(MatchOp op, const std::string& pattern) {
    std::string regex_src;
    auto flags = std::regex::ECMAScript;
    switch (op) {
        case MatchOp::LIKE:
            regex_src = like_to_regex(pattern);
            break;
        case MatchOp::ILIKE:
            regex_src = like_to_regex(pattern);
            flags |= std::regex::icase;
            break;
        case MatchOp::REGEX:
            regex_src = pattern;
            break;
        case MatchOp::IREGEX:
            regex_src = pattern;
            flags |= std::regex::icase;
            break;
        case MatchOp::ICONTAINS:
            regex_src = contains_to_regex(pattern);
            flags |= std::regex::icase;
            break;
    }
    try {
        auto cp = std::make_shared<CompiledPattern>();
        cp->re = std::regex(regex_src, flags);
        return cp;
    } catch (const std::regex_error& e) {
        return dftracer::utils::unexpected(std::string(e.what()));
    }
}

class Parser {
   public:
    Parser(const std::vector<Token>& tokens, std::string_view source)
        : tokens_(tokens), source_(source) {}

    dftracer::utils::expected<QueryNodePtr, QueryError> parse_query() {
        auto result = parse_or_expr();
        if (!result) return result;
        if (current().kind != TokenKind::END) {
            return dftracer::utils::unexpected(
                error("Expected 'and', 'or', or end of query, got '" +
                      std::string(current().text) + "'"));
        }
        return result;
    }

   private:
    const std::vector<Token>& tokens_;
    std::string_view source_;
    std::size_t pos_ = 0;

    const Token& current() const { return tokens_[pos_]; }

    const Token& advance() { return tokens_[pos_++]; }

    bool match(TokenKind kind) {
        if (current().kind == kind) {
            ++pos_;
            return true;
        }
        return false;
    }

    QueryError error(std::string msg) const {
        auto& tok = current();
        std::size_t len = tok.text.empty() ? 1 : tok.text.size();
        return make_error(source_, tok.column, len, std::move(msg));
    }

    dftracer::utils::expected<QueryNodePtr, QueryError> parse_or_expr() {
        auto left = parse_and_expr();
        if (!left) return left;
        while (current().kind == TokenKind::KW_OR) {
            advance();
            auto right = parse_and_expr();
            if (!right) return right;
            left = make_node(OrNode{std::move(*left), std::move(*right)});
        }
        return left;
    }

    dftracer::utils::expected<QueryNodePtr, QueryError> parse_and_expr() {
        auto left = parse_not_expr();
        if (!left) return left;
        while (current().kind == TokenKind::KW_AND) {
            advance();
            auto right = parse_not_expr();
            if (!right) return right;
            left = make_node(AndNode{std::move(*left), std::move(*right)});
        }
        return left;
    }

    dftracer::utils::expected<QueryNodePtr, QueryError> parse_not_expr() {
        if (current().kind == TokenKind::KW_NOT) {
            advance();
            auto operand = parse_not_expr();
            if (!operand) return operand;
            return make_node(NotNode{std::move(*operand)});
        }
        return parse_primary();
    }

    dftracer::utils::expected<QueryNodePtr, QueryError> parse_primary() {
        if (match(TokenKind::LPAREN)) {
            auto expr = parse_or_expr();
            if (!expr) return expr;
            if (!match(TokenKind::RPAREN)) {
                return dftracer::utils::unexpected(error("Expected ')'"));
            }
            return expr;
        }

        // Python-style substring: 'sub' in field / 'sub' not in field
        // (case-insensitive). Distinguished from "field in [array]" by the
        // string literal on the left.
        if (current().kind == TokenKind::STRING) {
            return parse_contains();
        }

        if (current().kind != TokenKind::IDENT) {
            return dftracer::utils::unexpected(
                error("Expected field name, string, or '(', got '" +
                      std::string(current().text) + "'"));
        }

        auto field = parse_field();

        // Check for "in" or "not in"
        if (current().kind == TokenKind::KW_IN) {
            advance();
            auto arr = parse_array();
            if (!arr) return dftracer::utils::unexpected(arr.error());
            return make_node(InNode{std::move(field), std::move(*arr)});
        }
        if (current().kind == TokenKind::KW_NOT) {
            // Look ahead for "in", "like", or "ilike".
            if (pos_ + 1 < tokens_.size()) {
                auto next = tokens_[pos_ + 1].kind;
                if (next == TokenKind::KW_IN) {
                    advance();  // consume "not"
                    advance();  // consume "in"
                    auto arr = parse_array();
                    if (!arr) return dftracer::utils::unexpected(arr.error());
                    return make_node(
                        NotInNode{std::move(field), std::move(*arr)});
                }
                if (next == TokenKind::KW_LIKE || next == TokenKind::KW_ILIKE) {
                    advance();  // consume "not"
                    advance();  // consume "like"/"ilike"
                    return parse_match(std::move(field),
                                       next == TokenKind::KW_LIKE
                                           ? MatchOp::LIKE
                                           : MatchOp::ILIKE,
                                       /*negated=*/true);
                }
            }
        }

        // Pattern match: like / ilike / ~ / ~* / !~ / !~*
        switch (current().kind) {
            case TokenKind::KW_LIKE:
                advance();
                return parse_match(std::move(field), MatchOp::LIKE, false);
            case TokenKind::KW_ILIKE:
                advance();
                return parse_match(std::move(field), MatchOp::ILIKE, false);
            case TokenKind::OP_REGEX:
                advance();
                return parse_match(std::move(field), MatchOp::REGEX, false);
            case TokenKind::OP_IREGEX:
                advance();
                return parse_match(std::move(field), MatchOp::IREGEX, false);
            case TokenKind::OP_NREGEX:
                advance();
                return parse_match(std::move(field), MatchOp::REGEX, true);
            case TokenKind::OP_NIREGEX:
                advance();
                return parse_match(std::move(field), MatchOp::IREGEX, true);
            default:
                break;
        }

        // Comparison
        auto op = parse_comp_op();
        if (!op) return dftracer::utils::unexpected(op.error());
        auto val = parse_value();
        if (!val) return dftracer::utils::unexpected(val.error());
        return make_node(CompareNode{std::move(field), *op, std::move(*val)});
    }

    FieldNode parse_field() {
        auto& tok = advance();
        return FieldNode{std::string(tok.text)};
    }

    dftracer::utils::expected<QueryNodePtr, QueryError> parse_match(
        FieldNode field, MatchOp op, bool negated) {
        if (current().kind != TokenKind::STRING) {
            return dftracer::utils::unexpected(
                error("Expected string pattern, got '" +
                      std::string(current().text) + "'"));
        }
        auto& tok = advance();
        std::string pattern(tok.text);
        auto compiled = compile_pattern(op, pattern);
        if (!compiled) {
            return dftracer::utils::unexpected(make_error(
                source_, tok.column, tok.text.empty() ? 1 : tok.text.size(),
                "Invalid pattern: " + compiled.error()));
        }
        MatchNode node;
        node.field = std::move(field);
        node.op = op;
        node.pattern = std::move(pattern);
        node.negated = negated;
        node.compiled = std::move(*compiled);
        return make_node(std::move(node));
    }

    // 'literal' in field / 'literal' not in field (case-insensitive substring).
    dftracer::utils::expected<QueryNodePtr, QueryError> parse_contains() {
        auto& str_tok = advance();  // the string literal
        std::string literal(str_tok.text);

        bool negated = false;
        if (current().kind == TokenKind::KW_NOT) {
            if (pos_ + 1 < tokens_.size() &&
                tokens_[pos_ + 1].kind == TokenKind::KW_IN) {
                advance();  // "not"
                advance();  // "in"
                negated = true;
            } else {
                return dftracer::utils::unexpected(
                    error("Expected 'in' after 'not'"));
            }
        } else if (current().kind == TokenKind::KW_IN) {
            advance();  // "in"
        } else {
            return dftracer::utils::unexpected(
                error("Expected 'in' after string literal, got '" +
                      std::string(current().text) + "'"));
        }

        if (current().kind != TokenKind::IDENT) {
            return dftracer::utils::unexpected(
                error("Expected field name after 'in', got '" +
                      std::string(current().text) + "'"));
        }
        auto field = parse_field();

        auto compiled = compile_pattern(MatchOp::ICONTAINS, literal);
        if (!compiled) {
            return dftracer::utils::unexpected(
                make_error(source_, str_tok.column,
                           str_tok.text.empty() ? 1 : str_tok.text.size(),
                           "Invalid pattern: " + compiled.error()));
        }
        MatchNode node;
        node.field = std::move(field);
        node.op = MatchOp::ICONTAINS;
        node.pattern = std::move(literal);
        node.negated = negated;
        node.compiled = std::move(*compiled);
        return make_node(std::move(node));
    }

    dftracer::utils::expected<CompareOp, QueryError> parse_comp_op() {
        auto& tok = current();
        switch (tok.kind) {
            case TokenKind::OP_EQ:
                advance();
                return CompareOp::EQ;
            case TokenKind::OP_NE:
                advance();
                return CompareOp::NE;
            case TokenKind::OP_GT:
                advance();
                return CompareOp::GT;
            case TokenKind::OP_LT:
                advance();
                return CompareOp::LT;
            case TokenKind::OP_GE:
                advance();
                return CompareOp::GE;
            case TokenKind::OP_LE:
                advance();
                return CompareOp::LE;
            default:
                return dftracer::utils::unexpected(
                    error("Expected comparison operator (==, !=, >, "
                          "<, >=, <=), got '" +
                          std::string(tok.text) + "'"));
        }
    }

    dftracer::utils::expected<LiteralNode, QueryError> parse_value() {
        auto& tok = current();
        switch (tok.kind) {
            case TokenKind::STRING: {
                auto text = std::string(tok.text);
                advance();
                return LiteralNode{std::move(text)};
            }
            case TokenKind::INT: {
                int64_t val = 0;
                auto [ptr, ec] = std::from_chars(
                    tok.text.data(), tok.text.data() + tok.text.size(), val);
                if (ec != std::errc{}) {
                    return dftracer::utils::unexpected(error(
                        "Invalid integer: '" + std::string(tok.text) + "'"));
                }
                advance();
                if (val >= 0) {
                    return LiteralNode{static_cast<uint64_t>(val)};
                }
                return LiteralNode{val};
            }
            case TokenKind::FLOAT: {
                double val = 0;
                auto sv = tok.text;
                // from_chars for double not available on all
                // compilers; use stod
                try {
                    val = std::stod(std::string(sv));
                } catch (...) {
                    return dftracer::utils::unexpected(
                        error("Invalid float: '" + std::string(sv) + "'"));
                }
                advance();
                return LiteralNode{val};
            }
            case TokenKind::KW_TRUE:
                advance();
                return LiteralNode{true};
            case TokenKind::KW_FALSE:
                advance();
                return LiteralNode{false};
            default:
                return dftracer::utils::unexpected(
                    error("Expected value (string, number, or bool), "
                          "got '" +
                          std::string(tok.text) + "'"));
        }
    }

    dftracer::utils::expected<ArrayNode, QueryError> parse_array() {
        if (!match(TokenKind::LBRACKET)) {
            return dftracer::utils::unexpected(error("Expected '['"));
        }
        ArrayNode arr;
        if (current().kind != TokenKind::RBRACKET) {
            auto val = parse_value();
            if (!val) return dftracer::utils::unexpected(val.error());
            arr.elements.push_back(std::move(*val));
            while (match(TokenKind::COMMA)) {
                val = parse_value();
                if (!val) return dftracer::utils::unexpected(val.error());
                arr.elements.push_back(std::move(*val));
            }
        }
        if (!match(TokenKind::RBRACKET)) {
            return dftracer::utils::unexpected(error("Expected ']' or ','"));
        }
        return arr;
    }
};

}  // namespace

dftracer::utils::expected<QueryNodePtr, QueryError> parse_tokens(
    const std::vector<Token>& tokens, std::string_view source) {
    Parser parser(tokens, source);
    return parser.parse_query();
}

dftracer::utils::expected<QueryNodePtr, QueryError> parse(
    std::string_view input) {
    auto tokens = tokenize(input);
    if (!tokens) return dftracer::utils::unexpected(tokens.error());
    return parse_tokens(*tokens, input);
}

}  // namespace dftracer::utils::utilities::common::query
