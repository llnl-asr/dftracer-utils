#include <dftracer/utils/duql/syntax/lexer.h>
#include <dftracer/utils/duql/syntax/parser.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>

namespace dftracer::utils::duql::syntax {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](unsigned char x, unsigned char y) {
                          return std::tolower(x) == std::tolower(y);
                      });
}

constexpr std::string_view RESERVED[] = {"and",    "or",    "not",     "in",
                                         "like",   "ilike", "between", "is",
                                         "escape", "null",  "true",    "false"};

constexpr std::string_view STAGES[] = {
    "where", "derive",     "select",    "drop",   "rename",  "distinct",
    "group", "agg",        "window",    "pivot",  "unpivot", "sort",
    "take",  "skip",       "sample",    "expand", "lookup",  "union",
    "call",  "time_range", "call_tree", "bucket", "session"};

bool reserved(std::string_view word) {
    for (auto r : RESERVED)
        if (iequals(word, r)) return true;
    return false;
}

bool stage_word(std::string_view word) {
    for (auto s : STAGES)
        if (iequals(word, s)) return true;
    return false;
}

// Precedence levels, lowest first, matching the printer.
enum Level { OR_LEVEL, AND_LEVEL, CMP_LEVEL, ADD_LEVEL, MUL_LEVEL };

struct Failure {
    duql::DuqlError error;
};

class Parser {
   public:
    Parser(std::string_view src, std::vector<Token> tokens)
        : src_(src), toks_(std::move(tokens)) {}

    Program program() {
        Program q;
        if (word("duql") && peek(1).kind == Tok::INT) {
            advance();
            const Token& v = advance();
            int version = 0;
            std::from_chars(v.text.data(), v.text.data() + v.text.size(),
                            version);
            if (version != DUQL_VERSION)
                fail(v, "Unsupported duql version " + std::string(v.text) +
                            "; this build reads version " +
                            std::to_string(DUQL_VERSION));
            q.version = version;
        }
        while (true) {
            if (word("let") && peek(1).kind == Tok::NAME &&
                peek(2).kind == Tok::ASSIGN) {
                advance();
                Let let;
                let.name = std::string(advance().text);
                advance();
                let.pipeline = pipeline();
                expect(Tok::SEMI, "';' after a let");
                q.decls.emplace_back(std::move(let));
            } else if (word("def") && peek(1).kind == Tok::NAME) {
                advance();
                Def def = definition();
                expect(Tok::SEMI, "';' after a def");
                q.decls.emplace_back(std::move(def));
            } else if (word("source") && peek(1).kind == Tok::NAME &&
                       peek(2).kind == Tok::LBRACE) {
                advance();
                q.decls.emplace_back(source_decl());
            } else {
                break;
            }
        }
        if (cur().kind != Tok::END) q.pipeline = pipeline();
        if (cur().kind != Tok::END)
            fail(cur(), cur().kind == Tok::PIPE
                            ? "Expected a stage after '|'"
                            : "Expected '|', an operator or the end of the "
                              "query, got '" +
                                  std::string(cur().text) + "'");
        return q;
    }

   private:
    std::string_view src_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;

    const Token& cur() const { return toks_[pos_]; }
    const Token& peek(std::size_t k) const {
        return toks_[std::min(pos_ + k, toks_.size() - 1)];
    }
    const Token& advance() {
        return toks_[pos_ == toks_.size() - 1 ? pos_ : pos_++];
    }
    bool at(Tok k) const { return cur().kind == k; }
    bool accept(Tok k) {
        if (!at(k)) return false;
        advance();
        return true;
    }
    bool word(std::string_view w) const {
        return at(Tok::NAME) && iequals(cur().text, w);
    }
    bool accept_word(std::string_view w) {
        if (!word(w)) return false;
        advance();
        return true;
    }

    [[noreturn]] void fail(const Token& t, std::string msg) const {
        throw Failure{make_error(src_, t.offset,
                                 t.text.empty() ? 1 : t.text.size(),
                                 std::move(msg))};
    }
    const Token& expect(Tok k, std::string_view what) {
        if (!at(k))
            fail(cur(), "Expected " + std::string(what) + ", got '" +
                            std::string(cur().text) + "'");
        return advance();
    }
    void expect_word(std::string_view w) {
        if (!accept_word(w))
            fail(cur(), "Expected '" + std::string(w) + "', got '" +
                            std::string(cur().text) + "'");
    }
    std::string name(std::string_view what) {
        if (!at(Tok::NAME) || reserved(cur().text))
            fail(cur(), "Expected " + std::string(what) + ", got '" +
                            std::string(cur().text) + "'");
        return std::string(advance().text);
    }
    std::string int_text(std::string_view what) {
        return std::string(expect(Tok::INT, what).text);
    }
    // A row count: an integer, or a parameter kept as `$name`.
    std::string count_text() {
        if (at(Tok::PARAM)) return "$" + std::string(advance().text);
        return int_text("a row count or a parameter");
    }
    std::pair<ExprPtr, ExprPtr> join_key() {
        ExprPtr left = level(ADD_LEVEL);
        ExprPtr right;
        if (accept(Tok::EQ)) right = level(ADD_LEVEL);
        return {std::move(left), std::move(right)};
    }
    Span span_from(const Token& first) const {
        const Token& last = toks_[pos_ == 0 ? 0 : pos_ - 1];
        const std::size_t end = last.offset + last.text.size();
        return {static_cast<std::uint32_t>(first.offset),
                static_cast<std::uint32_t>(
                    end > first.offset ? end - first.offset : 0)};
    }
    template <class Node>
    ExprPtr make(Node node, const Token& first) {
        auto e = std::make_unique<Expr>();
        e->node = std::move(node);
        e->span = span_from(first);
        return e;
    }

    Def definition() {
        Def def;
        def.name = name("a macro name");
        if (accept(Tok::LPAREN)) {
            if (!at(Tok::RPAREN)) {
                do {
                    def.params.push_back(name("a parameter name"));
                } while (accept(Tok::COMMA));
            }
            expect(Tok::RPAREN, "')'");
        }
        expect(Tok::ASSIGN, "'=' in a def");
        def.body = expr();
        return def;
    }

    SourceDecl source_decl() {
        SourceDecl s;
        s.name = std::string(advance().text);
        expect(Tok::LBRACE, "'{'");
        while (!at(Tok::RBRACE)) {
            if (word("def") && peek(1).kind == Tok::NAME) {
                advance();
                s.defs.push_back(definition());
            } else {
                RowSet r;
                r.name = name("a row set name or 'def'");
                expect(Tok::ASSIGN, "'=' after a row set name");
                r.pipeline = pipeline();
                s.rowsets.push_back(std::move(r));
            }
            if (!accept(Tok::SEMI) && !at(Tok::RBRACE))
                fail(cur(), "Expected ';' between source members, got '" +
                                std::string(cur().text) + "'");
        }
        advance();
        return s;
    }

    // At the start of a pipeline a stage word is a stage; a field with that
    // name is written with backticks.
    bool stage_start() const { return at(Tok::NAME) && stage_word(cur().text); }

    PipelinePtr pipeline() {
        auto p = std::make_unique<Pipeline>();
        if (word("from")) {
            advance();
            do {
                From f;
                if (at(Tok::STRING)) {
                    f.name = std::string(advance().text);
                    f.quoted = true;
                } else if (at(Tok::PARAM)) {
                    f.name = Param{std::string(advance().text)};
                } else {
                    f.name = name("a file, a row set or a parameter");
                }
                p->sources.push_back(std::move(f));
            } while (accept(Tok::COMMA));
        } else if (stage_start()) {
            p->stages.push_back(stage());
        } else {
            const Token& first = cur();
            Stage s;
            s.node = Where{expr()};
            s.span = span_from(first);
            p->stages.push_back(std::move(s));
        }
        while (accept(Tok::PIPE)) {
            if (!at(Tok::NAME) || !stage_word(cur().text))
                fail(cur(), "Expected a stage after '|', got '" +
                                std::string(cur().text) + "'");
            p->stages.push_back(stage());
        }
        return p;
    }

    std::vector<Assign> block() {
        expect(Tok::LBRACE, "'{'");
        std::vector<Assign> out;
        if (!at(Tok::RBRACE)) {
            do {
                out.push_back(assign());
            } while (accept(Tok::COMMA));
        }
        expect(Tok::RBRACE, "'}'");
        return out;
    }

    Assign assign() {
        Assign a;
        a.name = name("a field name");
        expect(Tok::ASSIGN, "'='");
        a.value = expr();
        return a;
    }

    Item item() {
        Item it;
        if (at(Tok::NAME) && peek(1).kind == Tok::ASSIGN) {
            it.name = name("a field name");
            advance();
            it.value = expr();
        } else {
            it.value = expr();
            if (accept_word("as")) it.name = name("a field name");
        }
        return it;
    }

    std::vector<SortKey> sort_keys() {
        std::vector<SortKey> keys;
        do {
            SortKey k;
            if (accept(Tok::MINUS)) {
                k.descending = true;
                k.value = expr();
            } else {
                k.value = expr();
            }
            if (accept_word("nulls")) {
                if (accept_word("first"))
                    k.nulls = NullsOrder::FIRST;
                else if (accept_word("last"))
                    k.nulls = NullsOrder::LAST;
                else
                    fail(cur(), "Expected 'first' or 'last' after 'nulls'");
            }
            keys.push_back(std::move(k));
        } while (accept(Tok::COMMA));
        return keys;
    }

    std::vector<ExprPtr> expr_list() {
        std::vector<ExprPtr> out;
        do {
            out.push_back(expr());
        } while (accept(Tok::COMMA));
        return out;
    }

    PipelinePtr from_pipeline(std::string_view what) {
        expect(Tok::LPAREN, "'('");
        if (!word("from"))
            fail(cur(), std::string(what) + " starts with 'from'");
        auto p = pipeline();
        expect(Tok::RPAREN, "')'");
        return p;
    }

    bool pipeline_end() const {
        return at(Tok::PIPE) || at(Tok::END) || at(Tok::SEMI) ||
               at(Tok::RPAREN) || at(Tok::RBRACE);
    }

    Stage stage() {
        const Token& first = cur();
        const std::string kw = [&] {
            std::string w(advance().text);
            for (auto& c : w)
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            return w;
        }();
        Stage s;
        if (kw == "where") {
            s.node = Where{expr()};
        } else if (kw == "derive") {
            Derive d;
            do {
                d.fields.push_back(assign());
            } while (accept(Tok::COMMA));
            s.node = std::move(d);
        } else if (kw == "select") {
            Select sel;
            do {
                sel.items.push_back(item());
            } while (accept(Tok::COMMA));
            s.node = std::move(sel);
        } else if (kw == "drop") {
            Drop d;
            do {
                d.paths.push_back(path());
            } while (accept(Tok::COMMA));
            s.node = std::move(d);
        } else if (kw == "rename") {
            Rename r;
            do {
                std::string n = name("a field name");
                expect(Tok::ASSIGN, "'='");
                r.pairs.emplace_back(std::move(n), path());
            } while (accept(Tok::COMMA));
            s.node = std::move(r);
        } else if (kw == "distinct") {
            Distinct d;
            if (!pipeline_end()) d.keys = expr_list();
            s.node = std::move(d);
        } else if (kw == "group") {
            Group g;
            if (!at(Tok::LBRACE)) {
                do {
                    g.keys.push_back(item());
                } while (accept(Tok::COMMA));
            }
            g.aggregates = block();
            s.node = std::move(g);
        } else if (kw == "agg") {
            s.node = Agg{block()};
        } else if (kw == "window") {
            Window w;
            if (!at(Tok::LBRACE) && !word("sort")) {
                do {
                    w.partition.push_back(item());
                } while (accept(Tok::COMMA));
            }
            if (accept_word("sort")) w.order = sort_keys();
            w.fields = block();
            s.node = std::move(w);
        } else if (kw == "pivot") {
            Pivot p;
            p.key = level(ADD_LEVEL);
            if (accept_word("in")) {
                expect(Tok::LBRACKET, "'['");
                if (!at(Tok::RBRACKET)) p.values = expr_list();
                expect(Tok::RBRACKET, "']'");
            }
            p.aggregates = block();
            s.node = std::move(p);
        } else if (kw == "unpivot") {
            Unpivot u;
            do {
                u.paths.push_back(path());
            } while (accept(Tok::COMMA));
            expect_word("as");
            u.key_name = name("a field name");
            expect(Tok::COMMA, "','");
            u.value_name = name("a field name");
            s.node = std::move(u);
        } else if (kw == "sort") {
            s.node = Sort{sort_keys()};
        } else if (kw == "take") {
            Take t;
            t.count = count_text();
            if (accept_word("by")) {
                do {
                    t.by.push_back(expr());
                } while (accept(Tok::COMMA));
                if (accept_word("sort")) t.order = sort_keys();
            }
            s.node = std::move(t);
        } else if (kw == "skip") {
            s.node = Skip{count_text()};
        } else if (kw == "sample") {
            Sample sm;
            if (at(Tok::INT) || at(Tok::FLOAT))
                sm.amount = std::string(advance().text);
            else
                fail(cur(), "Expected a row count or a percentage");
            sm.percent = accept(Tok::PERCENT);
            if (accept_word("seed")) sm.seed = int_text("a seed");
            s.node = std::move(sm);
        } else if (kw == "expand") {
            Expand e;
            e.path = path();
            if (accept_word("as")) e.as = name("a field name");
            if (accept_word("with_index")) e.with_index = name("a field name");
            e.keep_empty = accept_word("keep_empty");
            s.node = std::move(e);
        } else if (kw == "lookup") {
            Lookup l;
            l.rowset = name("a row set");
            expect_word("on");
            do {
                l.keys.push_back(join_key());
            } while (accept(Tok::COMMA));
            if (accept_word("overlap")) {
                OverlapLookup o;
                o.rowset = std::move(l.rowset);
                o.keys = std::move(l.keys);
                if (accept_word("into")) o.into = name("a field name");
                s.node = std::move(o);
            } else if (accept_word("asof")) {
                AsofLookup a;
                a.rowset = std::move(l.rowset);
                a.keys = std::move(l.keys);
                a.time = join_key();
                if (accept_word("backward"))
                    a.direction = AsofDirection::BACKWARD;
                else if (accept_word("forward"))
                    a.direction = AsofDirection::FORWARD;
                else if (accept_word("nearest"))
                    a.direction = AsofDirection::NEAREST;
                if (accept_word("within")) a.within = count_text();
                if (word("into"))
                    fail(cur(), "'asof' and 'into' cannot be combined");
                s.node = std::move(a);
            } else {
                if (accept_word("into")) l.into = name("a field name");
                s.node = std::move(l);
            }
        } else if (kw == "union") {
            s.node = Union{from_pipeline("A union pipeline")};
        } else if (kw == "call") {
            const Token& at_name = cur();
            std::string n = name("a function name");
            while (at(Tok::DOT) && peek(1).kind == Tok::NAME) {
                advance();
                n += '.';
                n += advance().text;
            }
            if (!at(Tok::LPAREN)) fail(at_name, "Expected '(' after the name");
            s.node = CallStage{call(std::move(n))};
        } else if (kw == "time_range") {
            TimeRange t;
            t.low = expr();
            expect(Tok::DOTDOT, "'..'");
            t.high = expr();
            t.overlap = accept_word("overlap");
            s.node = std::move(t);
        } else if (kw == "call_tree") {
            s.node = CallTree{};
        } else if (kw == "bucket") {
            Bucket b;
            b.width = expr();
            b.fill = accept_word("fill");
            s.node = std::move(b);
        } else if (kw == "session") {
            Session ss;
            if (!word("gap")) {
                do {
                    ss.keys.push_back(item());
                } while (accept(Tok::COMMA));
            }
            expect_word("gap");
            ss.gap = expr();
            if (accept_word("max")) ss.max = expr();
            if (accept_word("as")) ss.as = name("a field name");
            s.node = std::move(ss);
        } else {
            fail(first, "Unknown stage '" + std::string(first.text) + "'");
        }
        s.span = span_from(first);
        return s;
    }

    ExprPtr expr() { return level(OR_LEVEL); }

    ExprPtr level(int lv) {
        switch (lv) {
            case OR_LEVEL:
            case AND_LEVEL: {
                const Token& first = cur();
                const std::string_view op = lv == OR_LEVEL ? "or" : "and";
                ExprPtr left = lv == OR_LEVEL ? level(AND_LEVEL) : not_expr();
                while (word(op)) {
                    advance();
                    ExprPtr right =
                        lv == OR_LEVEL ? level(AND_LEVEL) : not_expr();
                    left = make(
                        Binary{lv == OR_LEVEL ? BinaryOp::OR : BinaryOp::AND,
                               std::move(left), std::move(right)},
                        first);
                }
                return left;
            }
            case CMP_LEVEL:
                return comparison();
            case ADD_LEVEL:
            case MUL_LEVEL: {
                const Token& first = cur();
                ExprPtr left = lv == ADD_LEVEL ? level(MUL_LEVEL) : unary();
                while (true) {
                    BinaryOp op;
                    if (lv == ADD_LEVEL && at(Tok::PLUS))
                        op = BinaryOp::ADD;
                    else if (lv == ADD_LEVEL && at(Tok::MINUS))
                        op = BinaryOp::SUB;
                    else if (lv == MUL_LEVEL && at(Tok::STAR))
                        op = BinaryOp::MUL;
                    else if (lv == MUL_LEVEL && at(Tok::SLASH))
                        op = BinaryOp::DIV;
                    else if (lv == MUL_LEVEL && at(Tok::SLASH2))
                        op = BinaryOp::IDIV;
                    else if (lv == MUL_LEVEL && at(Tok::PERCENT))
                        op = BinaryOp::MOD;
                    else
                        break;
                    advance();
                    ExprPtr right =
                        lv == ADD_LEVEL ? level(MUL_LEVEL) : unary();
                    left = make(Binary{op, std::move(left), std::move(right)},
                                first);
                }
                return left;
            }
            default:
                return unary();
        }
    }

    ExprPtr not_expr() {
        const Token& first = cur();
        if (accept_word("not"))
            return make(Unary{UnaryOp::NOT, not_expr()}, first);
        return comparison();
    }

    // The legacy `"text" [not] in path` test: a string, `in`, then a path.
    bool legacy_contains() const {
        if (!at(Tok::STRING)) return false;
        std::size_t k = 1;
        if (peek(k).kind == Tok::NAME && iequals(peek(k).text, "not")) ++k;
        if (peek(k).kind != Tok::NAME || !iequals(peek(k).text, "in"))
            return false;
        const Tok after = peek(k + 1).kind;
        return after == Tok::NAME || after == Tok::QNAME;
    }

    ExprPtr comparison() {
        const Token& first = cur();
        if (legacy_contains()) {
            Contains c;
            c.text = std::string(advance().text);
            c.negated = accept_word("not");
            expect_word("in");
            if (word("any") && peek(1).kind == Tok::LPAREN &&
                peek(1).adjacent) {
                advance();
                advance();
                c.path = path();
                c.any = true;
                expect(Tok::RPAREN, "')' after any(path");
            } else {
                c.path = path();
            }
            return make(std::move(c), first);
        }
        ExprPtr left = level(ADD_LEVEL);
        BinaryOp op;
        switch (cur().kind) {
            case Tok::EQ:
                op = BinaryOp::EQ;
                break;
            case Tok::NE:
                op = BinaryOp::NE;
                break;
            case Tok::LT:
                op = BinaryOp::LT;
                break;
            case Tok::LE:
                op = BinaryOp::LE;
                break;
            case Tok::GT:
                op = BinaryOp::GT;
                break;
            case Tok::GE:
                op = BinaryOp::GE;
                break;
            case Tok::REGEX:
                op = BinaryOp::REGEX;
                break;
            case Tok::IREGEX:
                op = BinaryOp::IREGEX;
                break;
            case Tok::NREGEX:
                op = BinaryOp::NREGEX;
                break;
            case Tok::NIREGEX:
                op = BinaryOp::NIREGEX;
                break;
            default:
                return keyword_comparison(std::move(left), first);
        }
        advance();
        ExprPtr right = level(ADD_LEVEL);
        return make(Binary{op, std::move(left), std::move(right)}, first);
    }

    ExprPtr keyword_comparison(ExprPtr left, const Token& first) {
        const bool negated =
            word("not") && peek(1).kind == Tok::NAME &&
            (iequals(peek(1).text, "in") || iequals(peek(1).text, "between") ||
             iequals(peek(1).text, "like") || iequals(peek(1).text, "ilike"));
        if (negated) advance();
        if (accept_word("in")) {
            In in;
            in.subject = std::move(left);
            in.negated = negated;
            if (at(Tok::LBRACKET)) {
                advance();
                if (!at(Tok::RBRACKET)) in.list = expr_list();
                expect(Tok::RBRACKET, "']' or ','");
            } else if (at(Tok::LPAREN) && peek(1).kind == Tok::NAME &&
                       iequals(peek(1).text, "from")) {
                in.subquery = from_pipeline("A sub-query");
            } else {
                fail(cur(), "Expected '[' or '(from' after 'in'");
            }
            return make(std::move(in), first);
        }
        if (accept_word("between")) {
            Between b;
            b.subject = std::move(left);
            b.negated = negated;
            b.low = level(ADD_LEVEL);
            expect_word("and");
            b.high = level(ADD_LEVEL);
            return make(std::move(b), first);
        }
        if (word("like") || word("ilike")) {
            Like l;
            l.icase = iequals(advance().text, "ilike");
            l.subject = std::move(left);
            l.negated = negated;
            l.pattern =
                std::string(expect(Tok::STRING, "a string pattern").text);
            if (accept_word("escape"))
                l.escape =
                    std::string(expect(Tok::STRING, "an escape string").text);
            return make(std::move(l), first);
        }
        if (negated) fail(cur(), "Expected 'in', 'between', 'like' or 'ilike'");
        if (accept_word("is")) {
            Is is;
            is.subject = std::move(left);
            is.negated = accept_word("not");
            if (accept_word("missing"))
                is.missing = true;
            else
                expect_word("null");
            return make(std::move(is), first);
        }
        return left;
    }

    ExprPtr unary() {
        const Token& first = cur();
        if (accept(Tok::MINUS))
            return make(Unary{UnaryOp::NEG, unary()}, first);
        return coalesce();
    }

    ExprPtr coalesce() {
        const Token& first = cur();
        ExprPtr left = postfix();
        while (accept(Tok::COALESCE))
            left = make(Binary{BinaryOp::COALESCE, std::move(left), postfix()},
                        first);
        return left;
    }

    ExprPtr postfix() {
        const Token& first = cur();
        ExprPtr e = primary();
        while (accept(Tok::ARROW)) {
            Arrow a;
            a.key = std::move(e);
            a.rowset = name("a row set name after '->'");
            if (at(Tok::LPAREN) && cur().adjacent) {
                advance();
                a.target_key = path();
                expect(Tok::RPAREN, "')'");
            }
            expect(Tok::DOT, "'.' and a path after the row set");
            a.path = path_steps(Path{});
            e = make(std::move(a), first);
        }
        return e;
    }

    // Steps after a root: keys joined by '.', each with adjacent indexes.
    Path path_steps(Path p) {
        while (true) {
            if (at(Tok::NAME)) {
                if (reserved(cur().text))
                    fail(cur(), "'" + std::string(cur().text) +
                                    "' is a keyword; quote the key as `" +
                                    std::string(cur().text) + "`");
                p.steps.push_back({std::string(advance().text), false, {}});
            } else if (at(Tok::QNAME)) {
                p.steps.push_back({std::string(advance().text), true, {}});
            } else if (!p.steps.empty() && at(Tok::INT)) {
                p.steps.push_back({std::string(advance().text), false, {}});
            } else if (!p.steps.empty() && at(Tok::FLOAT) &&
                       cur().text.find_first_not_of("0123456789.") ==
                           std::string_view::npos) {
                // `a.0.5` lexes 0.5 as one float: two numeric keys.
                const std::string_view f = advance().text;
                const auto dot = f.find('.');
                p.steps.push_back({std::string(f.substr(0, dot)), false, {}});
                p.steps.push_back({std::string(f.substr(dot + 1)), false, {}});
            } else {
                fail(cur(),
                     "Expected a key, got '" + std::string(cur().text) + "'");
            }
            indexes(p);
            if (!(at(Tok::DOT) &&
                  (peek(1).kind == Tok::NAME || peek(1).kind == Tok::QNAME ||
                   peek(1).kind == Tok::INT || peek(1).kind == Tok::FLOAT)))
                return p;
            advance();
        }
    }

    void indexes(Path& p) {
        while (at(Tok::LBRACKET) && cur().adjacent) {
            advance();
            const bool negative = accept(Tok::MINUS);
            const Token& n = expect(Tok::INT, "an array index");
            std::int64_t v = 0;
            const auto r = std::from_chars(n.text.data(),
                                           n.text.data() + n.text.size(), v);
            if (r.ec != std::errc{}) fail(n, "Array index out of range");
            expect(Tok::RBRACKET, "']'");
            PathStep step;
            step.index = negative ? -v : v;
            p.steps.push_back(std::move(step));
        }
    }

    Path path() {
        if (at(Tok::CARET)) {
            advance();
            expect(Tok::DOT, "'.' after '^'");
            return path_steps(Path{PathRoot::ENCLOSING, {}});
        }
        if (at(Tok::DOT)) {
            advance();
            Path p{PathRoot::CURRENT, {}};
            if ((at(Tok::NAME) && !reserved(cur().text)) || at(Tok::QNAME))
                return path_steps(std::move(p));
            indexes(p);
            return p;
        }
        return path_steps(Path{});
    }

    Call call(std::string n) {
        Call c;
        c.name = std::move(n);
        expect(Tok::LPAREN, "'('");
        if (!at(Tok::RPAREN)) {
            do {
                Arg a;
                if (at(Tok::NAME) && peek(1).kind == Tok::ASSIGN) {
                    a.name = name("an argument name");
                    advance();
                }
                a.value = expr();
                c.args.push_back(std::move(a));
            } while (accept(Tok::COMMA));
        }
        expect(Tok::RPAREN, "')'");
        return c;
    }

    ExprPtr number(const Token& t) {
        Literal lit;
        lit.number_text = std::string(t.text);
        if (t.kind == Tok::INT) {
            std::int64_t v = 0;
            const auto r = std::from_chars(t.text.data(),
                                           t.text.data() + t.text.size(), v);
            if (r.ec != std::errc{})
                fail(t, "Invalid integer: '" + std::string(t.text) + "'");
            lit.value = static_cast<std::uint64_t>(v);
        } else {
            try {
                lit.value = std::stod(std::string(t.text));
            } catch (...) {
                fail(t, "Invalid float: '" + std::string(t.text) + "'");
            }
        }
        advance();
        return make(std::move(lit), t);
    }

    ExprPtr primary() {
        const Token& t = cur();
        switch (t.kind) {
            case Tok::INT:
            case Tok::FLOAT:
                return number(t);
            case Tok::DURATION: {
                std::size_t i = 0;
                while (i < t.text.size() &&
                       (std::isdigit(static_cast<unsigned char>(t.text[i])) ||
                        t.text[i] == '.' || t.text[i] == 'e' ||
                        t.text[i] == 'E' ||
                        ((t.text[i] == '+' || t.text[i] == '-') && i > 0 &&
                         (t.text[i - 1] == 'e' || t.text[i - 1] == 'E'))))
                    ++i;
                advance();
                return make(Duration{std::string(t.text.substr(0, i)),
                                     std::string(t.text.substr(i))},
                            t);
            }
            case Tok::STRING:
                advance();
                return make(Literal{std::string(t.text), {}}, t);
            case Tok::PARAM:
                advance();
                return make(Param{std::string(t.text)}, t);
            case Tok::LPAREN: {
                if (peek(1).kind == Tok::NAME && iequals(peek(1).text, "from"))
                    return make(Subquery{from_pipeline("A sub-query")}, t);
                advance();
                ExprPtr e = expr();
                if (at(Tok::COMMA)) {
                    Tuple tu;
                    tu.items.push_back(std::move(e));
                    while (accept(Tok::COMMA)) tu.items.push_back(expr());
                    expect(Tok::RPAREN, "')'");
                    return make(std::move(tu), t);
                }
                expect(Tok::RPAREN, "')'");
                return e;
            }
            case Tok::LBRACKET: {
                advance();
                List l;
                if (!at(Tok::RBRACKET)) l.items = expr_list();
                expect(Tok::RBRACKET, "']' or ','");
                return make(std::move(l), t);
            }
            case Tok::DOT:
            case Tok::CARET:
            case Tok::QNAME:
                return make(path(), t);
            case Tok::NAME: {
                if (iequals(t.text, "null")) {
                    advance();
                    return make(Literal{Null{}, {}}, t);
                }
                if (iequals(t.text, "true") || iequals(t.text, "false")) {
                    advance();
                    return make(Literal{iequals(t.text, "true"), {}}, t);
                }
                if (reserved(t.text))
                    fail(t, "Expected a value, got the keyword '" +
                                std::string(t.text) + "'");
                // A dotted name directly followed by '(' is a call.
                std::size_t k = 0;
                while (peek(k).kind == Tok::NAME &&
                       peek(k + 1).kind == Tok::DOT &&
                       peek(k + 2).kind == Tok::NAME && peek(k + 1).adjacent &&
                       peek(k + 2).adjacent)
                    k += 2;
                if (peek(k).kind == Tok::NAME &&
                    peek(k + 1).kind == Tok::LPAREN && peek(k + 1).adjacent) {
                    std::string n(advance().text);
                    for (std::size_t i = 0; i < k; i += 2) {
                        advance();
                        n += '.';
                        n += advance().text;
                    }
                    return make(call(std::move(n)), t);
                }
                return make(path(), t);
            }
            default:
                fail(t, t.kind == Tok::END ? "Unexpected end of query"
                                           : "Expected a value, got '" +
                                                 std::string(t.text) + "'");
        }
    }
};

}  // namespace

dftracer::utils::expected<Program, duql::DuqlError> parse(
    std::string_view source) {
    auto tokens = lex(source);
    if (!tokens) return dftracer::utils::unexpected(tokens.error());
    try {
        return Parser(source, std::move(*tokens)).program();
    } catch (const Failure& f) {
        return dftracer::utils::unexpected(f.error);
    }
}

}  // namespace dftracer::utils::duql::syntax
