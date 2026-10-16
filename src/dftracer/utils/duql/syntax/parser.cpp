#include <dftracer/utils/duql/syntax/lexer.h>
#include <dftracer/utils/duql/syntax/parser.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <system_error>

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
    "where",    "derive", "parse",      "select",    "drop",   "rename",
    "distinct", "group",  "agg",        "window",    "pivot",  "unpivot",
    "sort",     "take",   "skip",       "sample",    "expand", "lookup",
    "union",    "call",   "time_range", "call_tree", "bucket", "session"};

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
enum Level { OrLevel, AndLevel, CmpLevel, AddLevel, MulLevel };

struct Failure {
    duql::DuqlError error;
};

class Parser {
   public:
    Parser(std::string_view src, std::vector<Token> tokens)
        : src_(src), toks_(std::move(tokens)) {}

    Program program() {
        Program q;
        if (word("duql") &&
            (peek(1).kind == Tok::Int || peek(1).kind == Tok::Float)) {
            advance();
            const Token& v = advance();
            const char* end = v.text.data() + v.text.size();
            int major = 0;
            int minor = 0;
            auto [p, ec] = std::from_chars(v.text.data(), end, major);
            bool ok = ec == std::errc{};
            if (ok && p != end) {
                ok = *p == '.';
                if (ok) {
                    auto [p2, ec2] = std::from_chars(p + 1, end, minor);
                    ok = ec2 == std::errc{} && p2 == end;
                }
            }
            if (!ok)
                fail(v, "duql version must be MAJOR or MAJOR.MINOR, not " +
                            std::string(v.text));
            if (major != DUQL_VERSION_MAJOR || minor > DUQL_VERSION_MINOR)
                fail(v, "Unsupported duql version " + std::string(v.text) +
                            "; this build reads duql " +
                            std::to_string(DUQL_VERSION_MAJOR) + "." +
                            std::to_string(DUQL_VERSION_MINOR));
        }
        while (true) {
            if (word("let") && peek(1).kind == Tok::Name &&
                peek(2).kind == Tok::Assign) {
                advance();
                Let let;
                let.name = std::string(advance().text);
                advance();
                let.pipeline = pipeline();
                expect(Tok::Semi, "';' after a let");
                q.decls.emplace_back(std::move(let));
            } else if (word("def") && peek(1).kind == Tok::Name) {
                advance();
                Def def = definition();
                expect(Tok::Semi, "';' after a def");
                q.decls.emplace_back(std::move(def));
            } else if (word("source") && peek(1).kind == Tok::Name &&
                       peek(2).kind == Tok::Lbrace) {
                advance();
                q.decls.emplace_back(source_decl());
            } else {
                break;
            }
        }
        if (cur().kind != Tok::End) q.pipeline = pipeline();
        if (cur().kind != Tok::End)
            fail(cur(), cur().kind == Tok::Pipe
                            ? "Expected a stage after '|'"
                            : "Expected '|', an operator or the end of the "
                              "query, got '" +
                                  shown(cur()) + "'");
        return q;
    }

   private:
    std::string_view src_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    const Token* wild_start_ = nullptr;

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
        return at(Tok::Name) && iequals(cur().text, w);
    }
    bool accept_word(std::string_view w) {
        if (!word(w)) return false;
        advance();
        return true;
    }

    // The value of the string token `t`: `\\`, `\"`, `\'`, `\n`, `\t`,
    // `\r` and `\uXXXX` (a surrogate pair for one code point) decode; any
    // other backslash stays, so a regex such as "\d+" keeps its escape.
    std::string string_value(const Token& t) const {
        const std::string_view s = t.text;
        std::string out;
        out.reserve(s.size());
        auto hex4 = [&](std::size_t at) -> long {
            if (at + 4 > s.size()) return -1;
            long v = 0;
            for (std::size_t k = at; k < at + 4; ++k) {
                const char h = s[k];
                const int d = h >= '0' && h <= '9'   ? h - '0'
                              : h >= 'a' && h <= 'f' ? h - 'a' + 10
                              : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                     : -1;
                if (d < 0) return -1;
                v = v * 16 + d;
            }
            return v;
        };
        auto put_utf8 = [&](unsigned long cp) {
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        };
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '\\' || i + 1 >= s.size()) {
                out += s[i];
                continue;
            }
            const char e = s[++i];
            switch (e) {
                case '\\':
                case '"':
                case '\'':
                    out += e;
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 'u': {
                    long cp = hex4(i + 1);
                    if (cp < 0) fail(t, "'\\u' takes four hex digits");
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        const long lo = i + 2 < s.size() && s[i + 1] == '\\' &&
                                                s[i + 2] == 'u'
                                            ? hex4(i + 3)
                                            : -1;
                        if (lo < 0xDC00 || lo > 0xDFFF)
                            fail(t, "a '\\u' high surrogate needs a low one");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail(t, "a '\\u' low surrogate needs a high one first");
                    }
                    put_utf8(static_cast<unsigned long>(cp));
                    break;
                }
                default:
                    out += '\\';
                    out += e;
            }
        }
        return out;
    }

    [[noreturn]] void fail(const Token& t, std::string msg) const {
        throw Failure{make_error(src_, t.offset,
                                 t.text.empty() ? 1 : t.text.size(),
                                 std::move(msg))};
    }
    // The token as written, for a message: a parameter keeps its `$`.
    static std::string shown(const Token& t) {
        return t.kind == Tok::Param ? "$" + std::string(t.text)
                                    : std::string(t.text);
    }
    const Token& expect(Tok k, std::string_view what) {
        if (!at(k))
            fail(cur(), "Expected " + std::string(what) + ", got '" +
                            shown(cur()) + "'");
        return advance();
    }
    void expect_word(std::string_view w) {
        if (!accept_word(w))
            fail(cur(), "Expected '" + std::string(w) + "', got '" +
                            shown(cur()) + "'");
    }
    std::string name(std::string_view what) {
        if (!at(Tok::Name) || reserved(cur().text))
            fail(cur(), "Expected " + std::string(what) + ", got '" +
                            shown(cur()) + "'");
        return std::string(advance().text);
    }
    std::string int_text(std::string_view what) {
        return std::string(expect(Tok::Int, what).text);
    }
    // A row count: an integer, or a parameter kept as `$name`.
    std::string count_text() {
        if (at(Tok::Param)) return "$" + std::string(advance().text);
        return int_text("a row count or a parameter");
    }
    std::pair<ExprPtr, ExprPtr> join_key() {
        ExprPtr left = level(AddLevel);
        ExprPtr right;
        if (accept(Tok::Eq)) right = level(AddLevel);
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
        if (accept(Tok::Lparen)) {
            if (!at(Tok::Rparen)) {
                do {
                    def.params.push_back(name("a parameter name"));
                } while (accept(Tok::Comma));
            }
            expect(Tok::Rparen, "')'");
        }
        expect(Tok::Assign, "'=' in a def");
        if (word("from"))
            fail(cur(),
                 "A macro body starts with a stage or an expression, not "
                 "'from'");
        if (stage_start()) {
            def.body = pipeline();
        } else {
            const Token& first = cur();
            ExprPtr e = expr();
            if (at(Tok::Pipe)) {
                auto p = std::make_unique<Pipeline>();
                p->stages.push_back(leading_stage(std::move(e), first));
                def.body = pipeline_tail(std::move(p));
            } else {
                def.body = std::move(e);
            }
        }
        return def;
    }

    SourceDecl source_decl() {
        SourceDecl s;
        s.name = std::string(advance().text);
        expect(Tok::Lbrace, "'{'");
        while (!at(Tok::Rbrace)) {
            if (word("def") && peek(1).kind == Tok::Name) {
                advance();
                s.defs.push_back(definition());
            } else {
                RowSet r;
                r.name = name("a row set name or 'def'");
                expect(Tok::Assign, "'=' after a row set name");
                r.pipeline = pipeline();
                s.rowsets.push_back(std::move(r));
            }
            if (!accept(Tok::Semi) && !at(Tok::Rbrace))
                fail(cur(), "Expected ';' between source members, got '" +
                                std::string(cur().text) + "'");
        }
        advance();
        return s;
    }

    // At the start of a pipeline a stage word is a stage; a field with that
    // name is written with backticks.
    bool stage_start() const { return at(Tok::Name) && stage_word(cur().text); }

    PipelinePtr pipeline() {
        auto p = std::make_unique<Pipeline>();
        if (word("from")) {
            advance();
            do {
                From f;
                if (at(Tok::String)) {
                    f.name = string_value(advance());
                    f.quoted = true;
                } else if (at(Tok::Param)) {
                    f.name = Param{std::string(advance().text)};
                } else {
                    f.name = name("a file, a row set or a parameter");
                }
                p->sources.push_back(std::move(f));
            } while (accept(Tok::Comma));
        } else if (stage_start()) {
            p->stages.push_back(stage());
        } else {
            const Token& first = cur();
            p->stages.push_back(leading_stage(expr(), first));
        }
        return pipeline_tail(std::move(p));
    }

    Stage leading_stage(ExprPtr e, const Token& first) {
        Stage s;
        if (auto* c = std::get_if<Call>(&e->node);
            c && c->name.find('.') == std::string::npos &&
            first.kind == Tok::Name && first.text != "case")
            s.node = Use{std::move(*c)};
        else
            s.node = Where{std::move(e)};
        s.span = span_from(first);
        return s;
    }

    PipelinePtr pipeline_tail(PipelinePtr p) {
        while (accept(Tok::Pipe)) {
            if (at(Tok::Name) && !stage_word(cur().text) &&
                peek(1).kind == Tok::Lparen) {
                const Token& first = cur();
                Stage s;
                std::string n(advance().text);
                s.node = Use{call(std::move(n))};
                s.span = span_from(first);
                p->stages.push_back(std::move(s));
                continue;
            }
            if (!at(Tok::Name) || !stage_word(cur().text))
                fail(cur(), "Expected a stage after '|', got '" +
                                std::string(cur().text) + "'");
            p->stages.push_back(stage());
        }
        return p;
    }

    std::vector<Assign> block() {
        expect(Tok::Lbrace, "'{'");
        std::vector<Assign> out;
        if (!at(Tok::Rbrace)) {
            do {
                if (at(Tok::Name) && peek(1).kind == Tok::Assign) {
                    out.push_back(assign());
                    continue;
                }
                const Token& first = cur();
                Assign a;
                a.value = expr();
                a.name = implicit_name(to_text(*a.value));
                if (a.name.empty())
                    fail(first, "Name this entry: 'name = ...'");
                out.push_back(std::move(a));
            } while (accept(Tok::Comma));
        }
        expect(Tok::Rbrace, "'}'");
        return out;
    }

    // Each run of characters other than letters, digits and '_' becomes
    // one '_', trimmed at both ends; a leading digit or a keyword gets '_'.
    static std::string implicit_name(std::string_view text) {
        std::string out;
        for (unsigned char c : text) {
            if (std::isalnum(c) || c == '_')
                out += static_cast<char>(c);
            else if (!out.empty() && out.back() != '_')
                out += '_';
        }
        const auto first = out.find_first_not_of('_');
        if (first == std::string::npos) return {};
        out = out.substr(first, out.find_last_not_of('_') - first + 1);
        if (std::isdigit(static_cast<unsigned char>(out[0])) || reserved(out))
            out.insert(out.begin(), '_');
        return out;
    }

    Assign assign() {
        Assign a;
        a.name = name("a field name");
        expect(Tok::Assign, "'='");
        a.value = expr();
        return a;
    }

    Item item() {
        Item it;
        if (at(Tok::Name) && peek(1).kind == Tok::Assign) {
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
            if (accept(Tok::Minus)) {
                k.descending = true;
                k.value = expr();
            } else {
                k.value = expr();
            }
            if (accept_word("nulls")) {
                if (accept_word("first"))
                    k.nulls = NullsOrder::First;
                else if (accept_word("last"))
                    k.nulls = NullsOrder::Last;
                else
                    fail(cur(), "Expected 'first' or 'last' after 'nulls'");
            }
            keys.push_back(std::move(k));
        } while (accept(Tok::Comma));
        return keys;
    }

    std::vector<ExprPtr> expr_list() {
        std::vector<ExprPtr> out;
        do {
            out.push_back(expr());
        } while (accept(Tok::Comma));
        return out;
    }

    PipelinePtr from_pipeline(std::string_view what) {
        expect(Tok::Lparen, "'('");
        if (!word("from"))
            fail(cur(), std::string(what) + " starts with 'from'");
        auto p = pipeline();
        expect(Tok::Rparen, "')'");
        return p;
    }

    bool pipeline_end() const {
        return at(Tok::Pipe) || at(Tok::End) || at(Tok::Semi) ||
               at(Tok::Rparen) || at(Tok::Rbrace);
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
            } while (accept(Tok::Comma));
            s.node = std::move(d);
        } else if (kw == "parse") {
            Parse p;
            p.column = path();
            if (!at(Tok::Regex))
                fail(cur(), "Expected '~' after the column of 'parse', got '" +
                                shown(cur()) + "'");
            advance();
            if (!at(Tok::String) && !at(Tok::Param))
                fail(cur(), "Expected a pattern string or a parameter, got '" +
                                shown(cur()) + "'");
            p.pattern = primary();
            s.node = std::move(p);
        } else if (kw == "select") {
            Select sel;
            do {
                wild_start_ = &cur();
                sel.items.push_back(item());
                const auto* wp =
                    std::get_if<Path>(&sel.items.back().value->node);
                if (wp && has_wildcard(*wp) && !sel.items.back().name.empty())
                    fail(cur(), "A wildcard path cannot be renamed");
            } while (accept(Tok::Comma));
            s.node = std::move(sel);
        } else if (kw == "drop") {
            Drop d;
            do {
                wild_start_ = &cur();
                d.paths.push_back(path());
            } while (accept(Tok::Comma));
            s.node = std::move(d);
        } else if (kw == "rename") {
            Rename r;
            do {
                std::string n = name("a field name");
                expect(Tok::Assign, "'='");
                r.pairs.emplace_back(std::move(n), path());
            } while (accept(Tok::Comma));
            s.node = std::move(r);
        } else if (kw == "distinct") {
            Distinct d;
            if (!pipeline_end()) {
                do {
                    d.keys.push_back(item());
                } while (accept(Tok::Comma));
            }
            s.node = std::move(d);
        } else if (kw == "group") {
            Group g;
            if (!at(Tok::Lbrace)) {
                do {
                    g.keys.push_back(item());
                } while (accept(Tok::Comma));
            }
            g.aggregates = block();
            s.node = std::move(g);
        } else if (kw == "agg") {
            s.node = Agg{block()};
        } else if (kw == "window") {
            Window w;
            if (!at(Tok::Lbrace) && !word("sort")) {
                do {
                    w.partition.push_back(item());
                } while (accept(Tok::Comma));
            }
            if (accept_word("sort")) w.order = sort_keys();
            w.fields = block();
            s.node = std::move(w);
        } else if (kw == "pivot") {
            Pivot p;
            p.key = level(AddLevel);
            if (accept_word("in")) {
                expect(Tok::Lbracket, "'['");
                if (!at(Tok::Rbracket)) {
                    do {
                        p.values.push_back(expr());
                        p.labels.push_back(accept_word("as")
                                               ? name("a column name")
                                               : std::string());
                    } while (accept(Tok::Comma));
                }
                expect(Tok::Rbracket, "']'");
            }
            p.aggregates = block();
            s.node = std::move(p);
        } else if (kw == "unpivot") {
            Unpivot u;
            do {
                wild_start_ = &cur();
                u.paths.push_back(path());
            } while (accept(Tok::Comma));
            expect_word("as");
            u.key_name = name("a field name");
            expect(Tok::Comma, "','");
            u.value_name = name("a field name");
            s.node = std::move(u);
        } else if (kw == "sort") {
            s.node = Sort{sort_keys()};
        } else if (kw == "take") {
            Take t;
            t.count = count_text();
            if (accept(Tok::Dotdot)) {
                t.last = count_text();
                if (word("by")) fail(cur(), "'take a..b' takes no 'by'");
            }
            if (accept_word("by")) {
                do {
                    t.by.push_back(expr());
                } while (accept(Tok::Comma));
                if (accept_word("sort")) t.order = sort_keys();
            }
            s.node = std::move(t);
        } else if (kw == "skip") {
            s.node = Skip{count_text()};
        } else if (kw == "sample") {
            Sample sm;
            if (at(Tok::Int) || at(Tok::Float))
                sm.amount = std::string(advance().text);
            else if (at(Tok::Param))
                sm.amount = "$" + std::string(advance().text);
            else
                fail(cur(),
                     "Expected a row count, a percentage or a "
                     "parameter, got '" +
                         shown(cur()) + "'");
            sm.percent = accept(Tok::Percent);
            if (accept_word("seed")) sm.seed = count_text();
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
            if (at(Tok::Lparen))
                l.side = from_pipeline("A lookup side");
            else
                l.rowset = name("a row set");
            expect_word("on");
            do {
                l.keys.push_back(join_key());
            } while (accept(Tok::Comma));
            if (word("inner") || word("anti")) {
                l.kind = word("inner") ? LookupKind::Inner : LookupKind::Anti;
                advance();
                if (word("asof") || word("overlap"))
                    fail(cur(),
                         "'inner' and 'anti' do not combine with 'asof' or "
                         "'overlap'");
                if (l.kind == LookupKind::Anti && word("into"))
                    fail(cur(), "'anti' adds no column, so it takes no 'into'");
            }
            if (accept_word("overlap")) {
                OverlapLookup o;
                o.rowset = std::move(l.rowset);
                o.side = std::move(l.side);
                o.keys = std::move(l.keys);
                if (accept_word("into")) o.into = name("a field name");
                s.node = std::move(o);
            } else if (accept_word("asof")) {
                AsofLookup a;
                a.rowset = std::move(l.rowset);
                a.side = std::move(l.side);
                a.keys = std::move(l.keys);
                a.time = join_key();
                if (accept_word("backward"))
                    a.direction = AsofDirection::Backward;
                else if (accept_word("forward"))
                    a.direction = AsofDirection::Forward;
                else if (accept_word("nearest"))
                    a.direction = AsofDirection::Nearest;
                if (accept_word("within")) a.within = level(AddLevel);
                if (word("into"))
                    fail(cur(), "'asof' and 'into' cannot be combined");
                s.node = std::move(a);
            } else {
                if (accept_word("into")) l.into = name("a field name");
                s.node = std::move(l);
            }
        } else if (kw == "union") {
            if (at(Tok::Name) || at(Tok::String)) {
                auto other = std::make_unique<Pipeline>();
                From f;
                f.quoted = at(Tok::String);
                f.name = f.quoted ? string_value(advance())
                                  : name("a file, a row set or a let");
                other->sources.push_back(std::move(f));
                s.node = Union{std::move(other)};
            } else {
                s.node = Union{from_pipeline("A union pipeline")};
            }
        } else if (kw == "call") {
            const Token& at_name = cur();
            std::string n = name("a function name");
            while (at(Tok::Dot) && peek(1).kind == Tok::Name) {
                advance();
                n += '.';
                n += advance().text;
            }
            if (!at(Tok::Lparen)) fail(at_name, "Expected '(' after the name");
            s.node = CallStage{call(std::move(n))};
        } else if (kw == "time_range") {
            TimeRange t;
            if (!at(Tok::Dotdot)) t.low = expr();
            expect(Tok::Dotdot, "'..'");
            if (!at(Tok::Pipe) && !at(Tok::End) && !word("overlap"))
                t.high = expr();
            if (!t.low && !t.high)
                fail(cur(), "'time_range' needs a low or a high bound");
            t.overlap = accept_word("overlap");
            s.node = std::move(t);
        } else if (kw == "call_tree") {
            s.node = CallTree{};
        } else if (kw == "bucket") {
            Bucket b;
            b.width = expr();
            if (accept_word("every")) b.every = expr();
            if (accept_word("at")) b.at = expr();
            b.fill = accept_word("fill");
            if (b.fill) {
                if (accept_word("forward"))
                    b.fill_mode = FillMode::Forward;
                else if (accept_word("linear"))
                    b.fill_mode = FillMode::Linear;
            }
            if (word("from")) {
                if (!b.fill)
                    fail(cur(), "'from .. to' sets the range of 'fill'");
                advance();
                b.low = expr();
                expect_word("to");
                b.high = expr();
            }
            if (accept_word("as")) b.as = name("a field name");
            s.node = std::move(b);
        } else if (kw == "session") {
            Session ss;
            if (!word("gap")) {
                do {
                    ss.keys.push_back(item());
                } while (accept(Tok::Comma));
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

    ExprPtr expr() { return level(OrLevel); }

    ExprPtr level(int lv) {
        switch (lv) {
            case OrLevel:
            case AndLevel: {
                const Token& first = cur();
                const std::string_view op = lv == OrLevel ? "or" : "and";
                ExprPtr left = lv == OrLevel ? level(AndLevel) : not_expr();
                while (word(op)) {
                    advance();
                    ExprPtr right =
                        lv == OrLevel ? level(AndLevel) : not_expr();
                    left = make(
                        Binary{lv == OrLevel ? BinaryOp::Or : BinaryOp::And,
                               std::move(left), std::move(right)},
                        first);
                }
                return left;
            }
            case CmpLevel:
                return comparison();
            case AddLevel:
            case MulLevel: {
                const Token& first = cur();
                ExprPtr left = lv == AddLevel ? level(MulLevel) : unary();
                while (true) {
                    BinaryOp op;
                    if (lv == AddLevel && at(Tok::Plus))
                        op = BinaryOp::Add;
                    else if (lv == AddLevel && at(Tok::Minus))
                        op = BinaryOp::Sub;
                    else if (lv == MulLevel && at(Tok::Star))
                        op = BinaryOp::Mul;
                    else if (lv == MulLevel && at(Tok::Slash))
                        op = BinaryOp::Div;
                    else if (lv == MulLevel && at(Tok::Slash2))
                        op = BinaryOp::Idiv;
                    else if (lv == MulLevel && at(Tok::Percent))
                        op = BinaryOp::Mod;
                    else
                        break;
                    advance();
                    ExprPtr right = lv == AddLevel ? level(MulLevel) : unary();
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
            return make(Unary{UnaryOp::Not, not_expr()}, first);
        return comparison();
    }

    // The legacy `"text" [not] in path` test: a string, `in`, then a path.
    bool legacy_contains() const {
        if (!at(Tok::String)) return false;
        std::size_t k = 1;
        if (peek(k).kind == Tok::Name && iequals(peek(k).text, "not")) ++k;
        if (peek(k).kind != Tok::Name || !iequals(peek(k).text, "in"))
            return false;
        const Tok after = peek(k + 1).kind;
        return after == Tok::Name || after == Tok::Qname;
    }

    ExprPtr comparison() {
        const Token& first = cur();
        if (legacy_contains()) {
            Contains c;
            c.text = string_value(advance());
            c.negated = accept_word("not");
            expect_word("in");
            if (word("any") && peek(1).kind == Tok::Lparen &&
                peek(1).adjacent) {
                advance();
                advance();
                c.path = path();
                c.any = true;
                expect(Tok::Rparen, "')' after any(path");
            } else {
                c.path = path();
            }
            return make(std::move(c), first);
        }
        ExprPtr left = level(AddLevel);
        BinaryOp op;
        switch (cur().kind) {
            case Tok::Eq:
                op = BinaryOp::Eq;
                break;
            case Tok::Ne:
                op = BinaryOp::Ne;
                break;
            case Tok::Lt:
                op = BinaryOp::Lt;
                break;
            case Tok::Le:
                op = BinaryOp::Le;
                break;
            case Tok::Gt:
                op = BinaryOp::Gt;
                break;
            case Tok::Ge:
                op = BinaryOp::Ge;
                break;
            case Tok::Regex:
                op = BinaryOp::Regex;
                break;
            case Tok::Iregex:
                op = BinaryOp::Iregex;
                break;
            case Tok::Nregex:
                op = BinaryOp::Nregex;
                break;
            case Tok::Niregex:
                op = BinaryOp::Niregex;
                break;
            default:
                return keyword_comparison(std::move(left), first);
        }
        advance();
        ExprPtr right = level(AddLevel);
        return make(Binary{op, std::move(left), std::move(right)}, first);
    }

    ExprPtr keyword_comparison(ExprPtr left, const Token& first) {
        const bool negated =
            word("not") && peek(1).kind == Tok::Name &&
            (iequals(peek(1).text, "in") || iequals(peek(1).text, "between") ||
             iequals(peek(1).text, "like") || iequals(peek(1).text, "ilike"));
        if (negated) advance();
        if (accept_word("in")) {
            In in;
            in.subject = std::move(left);
            in.negated = negated;
            if (at(Tok::Lbracket)) {
                advance();
                if (!at(Tok::Rbracket)) in.list = expr_list();
                expect(Tok::Rbracket, "']' or ','");
            } else if (at(Tok::Lparen) && peek(1).kind == Tok::Name &&
                       iequals(peek(1).text, "from")) {
                in.subquery = from_pipeline("A sub-query");
            } else if (at(Tok::Param)) {
                in.list_param = std::string(advance().text);
            } else {
                fail(cur(), "Expected '[', '(from' or a parameter after 'in'");
            }
            return make(std::move(in), first);
        }
        if (accept_word("between")) {
            Between b;
            b.subject = std::move(left);
            b.negated = negated;
            b.low = level(AddLevel);
            expect_word("and");
            b.high = level(AddLevel);
            return make(std::move(b), first);
        }
        if (word("like") || word("ilike")) {
            Like l;
            l.icase = iequals(advance().text, "ilike");
            l.subject = std::move(left);
            l.negated = negated;
            if (at(Tok::Param))
                l.pattern_param = std::string(advance().text);
            else
                l.pattern =
                    string_value(expect(Tok::String, "a string pattern"));
            if (accept_word("escape"))
                l.escape =
                    string_value(expect(Tok::String, "an escape string"));
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
        if (accept(Tok::Minus))
            return make(Unary{UnaryOp::Neg, unary()}, first);
        return coalesce();
    }

    ExprPtr coalesce() {
        const Token& first = cur();
        ExprPtr left = postfix();
        while (accept(Tok::Coalesce))
            left = make(Binary{BinaryOp::Coalesce, std::move(left), postfix()},
                        first);
        return left;
    }

    ExprPtr postfix() {
        const Token& first = cur();
        ExprPtr e = primary();
        if (std::holds_alternative<Call>(e->node) && word("over")) {
            advance();
            Over o;
            o.call = std::move(e);
            if (!at(Tok::Int) && !at(Tok::Float) && !at(Tok::Duration) &&
                !at(Tok::Param))
                fail(cur(),
                     "Expected a number, a duration or a parameter "
                     "after 'over'");
            o.width = primary();
            o.rows = accept_word("rows");
            e = make(std::move(o), first);
        }
        while (accept(Tok::Arrow)) {
            Arrow a;
            a.key = std::move(e);
            a.rowset = name("a row set name after '->'");
            if (at(Tok::Lparen) && cur().adjacent) {
                advance();
                a.target_key = path();
                expect(Tok::Rparen, "')'");
            }
            expect(Tok::Dot, "'.' and a path after the row set");
            a.path = path_steps(Path{}, cur(), nullptr);
            e = make(std::move(a), first);
        }
        return e;
    }

    // Steps after a root: keys joined by '.', each with adjacent indexes. A
    // computed index ends the path; `out` receives it, and is null where
    // only a plain path may stand.
    Path path_steps(Path p, const Token& first, ExprPtr* out) {
        while (true) {
            if (at(Tok::Star) && !p.steps.empty() && &first == wild_start_) {
                advance();
                p.steps.push_back({"*", false, {}});
            } else if (at(Tok::Name)) {
                if (reserved(cur().text))
                    fail(cur(), "'" + std::string(cur().text) +
                                    "' is a keyword; quote the key as `" +
                                    std::string(cur().text) + "`");
                p.steps.push_back({std::string(advance().text), false, {}});
            } else if (at(Tok::Qname)) {
                p.steps.push_back({std::string(advance().text), true, {}});
            } else if (!p.steps.empty() && at(Tok::Int)) {
                p.steps.push_back({std::string(advance().text), false, {}});
            } else if (!p.steps.empty() && at(Tok::Float) &&
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
            if (indexes(p, first, out)) return p;
            if (!(at(Tok::Dot) &&
                  (peek(1).kind == Tok::Name || peek(1).kind == Tok::Qname ||
                   peek(1).kind == Tok::Int || peek(1).kind == Tok::Float ||
                   (peek(1).kind == Tok::Star && &first == wild_start_))))
                return p;
            advance();
        }
    }

    bool literal_index() const {
        const std::size_t k = peek(1).kind == Tok::Minus ? 2 : 1;
        return peek(k).kind == Tok::Int && peek(k + 1).kind == Tok::Rbracket;
    }

    // True when a computed index ended the path and went to `out`.
    bool indexes(Path& p, const Token& first, ExprPtr* out) {
        while (at(Tok::Lbracket) && cur().adjacent) {
            if (!literal_index()) {
                const Token& open = cur();
                if (!out) fail(open, "A computed index is not allowed here");
                advance();
                ExprPtr index = expr();
                expect(Tok::Rbracket, "']'");
                if ((at(Tok::Dot) && cur().adjacent) ||
                    (at(Tok::Lbracket) && cur().adjacent))
                    fail(cur(), "A computed index ends the path");
                *out = make(Index{make(std::move(p), first), std::move(index)},
                            first);
                return true;
            }
            advance();
            const bool negative = accept(Tok::Minus);
            const Token& n = expect(Tok::Int, "an array index");
            std::int64_t v = 0;
            const auto r = std::from_chars(n.text.data(),
                                           n.text.data() + n.text.size(), v);
            if (r.ec != std::errc{}) fail(n, "Array index out of range");
            expect(Tok::Rbracket, "']'");
            PathStep step;
            step.index = negative ? -v : v;
            p.steps.push_back(std::move(step));
        }
        return false;
    }

    static bool has_wildcard(const Path& p) {
        for (const PathStep& st : p.steps)
            if (st.wildcard()) return true;
        return false;
    }

    Path path(ExprPtr* out = nullptr) {
        Path p = path_root(out);
        if (has_wildcard(p)) {
            const Tok k = cur().kind;
            if (k != Tok::Comma && k != Tok::Pipe && k != Tok::Semi &&
                k != Tok::End && k != Tok::Rparen && k != Tok::Rbrace &&
                k != Tok::Name)
                fail(cur(),
                     "A wildcard path stands alone in select, drop, unpivot "
                     "or any()/all()");
        }
        return p;
    }

    Path path_root(ExprPtr* out) {
        const Token& first = cur();
        if (at(Tok::Caret)) {
            advance();
            expect(Tok::Dot, "'.' after '^'");
            return path_steps(Path{PathRoot::Enclosing, {}}, first, out);
        }
        if (at(Tok::Dot)) {
            advance();
            Path p{PathRoot::Current, {}};
            if ((at(Tok::Name) && !reserved(cur().text)) || at(Tok::Qname))
                return path_steps(std::move(p), first, out);
            indexes(p, first, out);
            return p;
        }
        return path_steps(Path{}, first, out);
    }

    ExprPtr path_expr(const Token& t) {
        ExprPtr computed;
        Path p = path(&computed);
        return computed ? std::move(computed) : make(std::move(p), t);
    }

    Call call(std::string n) {
        Call c;
        c.name = std::move(n);
        expect(Tok::Lparen, "'('");
        if (c.name == "any" || c.name == "all") wild_start_ = &cur();
        if (!at(Tok::Rparen)) {
            do {
                Arg a;
                if (at(Tok::Name) && peek(1).kind == Tok::Assign) {
                    a.name = name("an argument name");
                    advance();
                }
                a.value = expr();
                c.args.push_back(std::move(a));
            } while (accept(Tok::Comma));
        }
        expect(Tok::Rparen, "')'");
        return c;
    }

    // `case { c => a, ..., else => d }`: the call `case(c, a, ..., d)`.
    Call case_block() {
        advance();
        const Token& open = advance();
        Call c;
        c.name = "case";
        if (at(Tok::Rbrace)) fail(open, "A case block needs a 'c => v' pair");
        do {
            if (word("else") && peek(1).kind == Tok::Fatarrow) {
                advance();
                advance();
                c.args.push_back({{}, expr()});
                if (!at(Tok::Rbrace))
                    fail(cur(),
                         "'else' must be the last entry of a case block");
                break;
            }
            c.args.push_back({{}, expr()});
            expect(Tok::Fatarrow, "'=>'");
            c.args.push_back({{}, expr()});
        } while (accept(Tok::Comma));
        if (c.args.size() % 2 == 0)
            c.args.push_back({{}, make(Literal{Null{}, {}}, cur())});
        expect(Tok::Rbrace, "'}'");
        return c;
    }

    ExprPtr number(const Token& t) {
        Literal lit;
        lit.number_text = std::string(t.text);
        if (t.kind == Tok::Int) {
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
            case Tok::Int:
            case Tok::Float:
                return number(t);
            case Tok::Duration: {
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
            case Tok::String:
                advance();
                return make(Literal{string_value(t), {}}, t);
            case Tok::Param:
                advance();
                return make(Param{std::string(t.text)}, t);
            case Tok::Lparen: {
                if (peek(1).kind == Tok::Name && iequals(peek(1).text, "from"))
                    return make(Subquery{from_pipeline("A sub-query")}, t);
                advance();
                ExprPtr e = expr();
                if (at(Tok::Comma)) {
                    Tuple tu;
                    tu.items.push_back(std::move(e));
                    while (accept(Tok::Comma)) tu.items.push_back(expr());
                    expect(Tok::Rparen, "')'");
                    return make(std::move(tu), t);
                }
                expect(Tok::Rparen, "')'");
                return e;
            }
            case Tok::Lbracket: {
                advance();
                List l;
                if (!at(Tok::Rbracket)) l.items = expr_list();
                expect(Tok::Rbracket, "']' or ','");
                return make(std::move(l), t);
            }
            case Tok::Dot:
            case Tok::Caret:
            case Tok::Qname:
                return path_expr(t);
            case Tok::Name: {
                if (iequals(t.text, "null")) {
                    advance();
                    return make(Literal{Null{}, {}}, t);
                }
                if (iequals(t.text, "true") || iequals(t.text, "false")) {
                    advance();
                    return make(Literal{iequals(t.text, "true"), {}}, t);
                }
                if (iequals(t.text, "case") && peek(1).kind == Tok::Lbrace)
                    return make(case_block(), t);
                if (reserved(t.text))
                    fail(t, "Expected a value, got the keyword '" +
                                std::string(t.text) + "'");
                // A dotted name directly followed by '(' is a call.
                std::size_t k = 0;
                while (peek(k).kind == Tok::Name &&
                       peek(k + 1).kind == Tok::Dot &&
                       peek(k + 2).kind == Tok::Name && peek(k + 1).adjacent &&
                       peek(k + 2).adjacent)
                    k += 2;
                if (peek(k).kind == Tok::Name &&
                    peek(k + 1).kind == Tok::Lparen && peek(k + 1).adjacent) {
                    std::string n(advance().text);
                    for (std::size_t i = 0; i < k; i += 2) {
                        advance();
                        n += '.';
                        n += advance().text;
                    }
                    return make(call(std::move(n)), t);
                }
                return path_expr(t);
            }
            default:
                fail(t, t.kind == Tok::End ? "Unexpected end of query"
                                           : "Expected a value, got '" +
                                                 std::string(t.text) + "'");
        }
    }
};

}  // namespace

bool is_stage_word(std::string_view word) { return stage_word(word); }

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
