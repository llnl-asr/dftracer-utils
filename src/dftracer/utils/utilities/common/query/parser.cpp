#include <dftracer/utils/utilities/common/query/parser.h>

#include <algorithm>
#include <cctype>
#include <charconv>
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

        if (current().kind != TokenKind::IDENT) {
            return dftracer::utils::unexpected(
                error("Expected field name or '(', got '" +
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
            // Look ahead for "in"
            if (pos_ + 1 < tokens_.size() &&
                tokens_[pos_ + 1].kind == TokenKind::KW_IN) {
                advance();  // consume "not"
                advance();  // consume "in"
                auto arr = parse_array();
                if (!arr) return dftracer::utils::unexpected(arr.error());
                return make_node(NotInNode{std::move(field), std::move(*arr)});
            }
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
