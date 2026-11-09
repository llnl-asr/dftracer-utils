// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/syntax/lexer.h>
#include <dftracer/utils/duql/syntax/parser.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>

namespace dftracer::utils::duql::syntax {

namespace {

using ::dftracer::utils::common::detail::iequals;

constexpr dstd::StringView RESERVED[] = {"and",    "or",    "not",     "in",
                                         "like",   "ilike", "between", "is",
                                         "escape", "null",  "true",    "false"};

constexpr dstd::StringView STAGES[] = {
    "where",    "derive", "parse",      "select",    "drop",   "rename",
    "distinct", "group",  "agg",        "window",    "pivot",  "unpivot",
    "sort",     "take",   "skip",       "sample",    "expand", "lookup",
    "union",    "call",   "time_range", "call_tree", "bucket", "session"};

bool reserved(dstd::StringView word) {
    for (auto r : RESERVED)
        if (iequals(word, r)) return true;
    return false;
}

bool stage_word(dstd::StringView word) {
    for (auto s : STAGES)
        if (iequals(word, s)) return true;
    return false;
}

// Precedence levels, lowest first, matching the printer.
enum Level { OrLevel, AndLevel, CmpLevel, AddLevel, MulLevel };

template <class T>
using Res = dstd::Expected<T, duql::DuqlError>;
using Fail = dstd::Unexpected<duql::DuqlError>;

class Parser {
   public:
    Parser(dstd::StringView src, dstd::Vector<Token> tokens)
        : src_(src), toks_(std::move(tokens)) {}

    Res<Program> program() {
        Program q;
        if (word("duql") &&
            (peek(1).kind == Tok::Int || peek(1).kind == Tok::Float)) {
            advance();
            const Token& v = advance();
            const char* end = v.text.data() + v.text.size();
            int major = 0;
            int minor = 0;
            auto [p, ec] = dstd::from_chars(v.text.data(), end, major);
            bool ok = ec == std::errc{};
            if (ok && p != end) {
                ok = *p == '.';
                if (ok) {
                    auto [p2, ec2] = dstd::from_chars(p + 1, end, minor);
                    ok = ec2 == std::errc{} && p2 == end;
                }
            }
            if (!ok)
                return fail(v,
                            "duql version must be MAJOR or MAJOR.MINOR, not " +
                                dstd::String(v.text));
            if (major != DUQL_VERSION_MAJOR || minor > DUQL_VERSION_MINOR)
                return fail(v, "Unsupported duql version " +
                                   dstd::String(v.text) +
                                   "; this build reads duql " +
                                   dstd::int_text(DUQL_VERSION_MAJOR) + "." +
                                   dstd::int_text(DUQL_VERSION_MINOR));
        }
        while (true) {
            if (word("let") && peek(1).kind == Tok::Name &&
                peek(2).kind == Tok::Assign) {
                advance();
                Let let;
                let.name = dstd::String(advance().text);
                advance();
                auto body = pipeline();
                if (!body) return dstd::Unexpected(body.error());
                let.pipeline = std::move(*body);
                auto semi = expect(Tok::Semi, "';' after a let");
                if (!semi) return dstd::Unexpected(semi.error());
                q.decls.emplace_back(std::move(let));
            } else if (word("def") && peek(1).kind == Tok::Name) {
                advance();
                auto def = definition();
                if (!def) return dstd::Unexpected(def.error());
                auto semi = expect(Tok::Semi, "';' after a def");
                if (!semi) return dstd::Unexpected(semi.error());
                q.decls.emplace_back(std::move(*def));
            } else if (word("source") && peek(1).kind == Tok::Name &&
                       peek(2).kind == Tok::Lbrace) {
                advance();
                auto decl = source_decl();
                if (!decl) return dstd::Unexpected(decl.error());
                q.decls.emplace_back(std::move(*decl));
            } else {
                break;
            }
        }
        if (cur().kind != Tok::End) {
            auto body = pipeline();
            if (!body) return dstd::Unexpected(body.error());
            q.pipeline = std::move(*body);
        }
        if (cur().kind != Tok::End)
            return fail(cur(), cur().kind == Tok::Pipe
                                   ? "Expected a stage after '|'"
                                   : "Expected '|', an operator or the end of "
                                     "the query, got '" +
                                         shown(cur()) + "'");
        return q;
    }

   private:
    dstd::StringView src_;
    dstd::Vector<Token> toks_;
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
    bool word(dstd::StringView w) const {
        return at(Tok::Name) && iequals(cur().text, w);
    }
    bool accept_word(dstd::StringView w) {
        if (!word(w)) return false;
        advance();
        return true;
    }

    // The value of the string token `t`: `\\`, `\"`, `\'`, `\n`, `\t`,
    // `\r` and `\uXXXX` (a surrogate pair for one code point) decode; any
    // other backslash stays, so a regex such as "\d+" keeps its escape.
    Res<dstd::String> string_value(const Token& t) const {
        const dstd::StringView s = t.text;
        dstd::String out;
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
                    if (cp < 0) return fail(t, "'\\u' takes four hex digits");
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        const long lo = i + 2 < s.size() && s[i + 1] == '\\' &&
                                                s[i + 2] == 'u'
                                            ? hex4(i + 3)
                                            : -1;
                        if (lo < 0xDC00 || lo > 0xDFFF)
                            return fail(t,
                                        "a '\\u' high surrogate needs a low "
                                        "one");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail(
                            t, "a '\\u' low surrogate needs a high one first");
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

    Fail fail(const Token& t, dstd::String msg) const {
        return Fail(make_error(src_, t.offset,
                               t.text.empty() ? 1 : t.text.size(),
                               std::move(msg)));
    }
    // The token as written, for a message: a parameter keeps its `$`.
    static dstd::String shown(const Token& t) {
        return t.kind == Tok::Param ? "$" + dstd::String(t.text)
                                    : dstd::String(t.text);
    }
    Res<const Token*> expect_token(Tok k, dstd::StringView what) {
        if (!at(k))
            return fail(cur(), "Expected " + dstd::String(what) + ", got '" +
                                   shown(cur()) + "'");
        return &advance();
    }
    Res<void> expect(Tok k, dstd::StringView what) {
        auto t = expect_token(k, what);
        if (!t) return dstd::Unexpected(t.error());
        return {};
    }
    Res<void> expect_word(dstd::StringView w) {
        if (!accept_word(w))
            return fail(cur(), "Expected '" + dstd::String(w) + "', got '" +
                                   shown(cur()) + "'");
        return {};
    }
    Res<dstd::String> name(dstd::StringView what) {
        if (!at(Tok::Name) || reserved(cur().text))
            return fail(cur(), "Expected " + dstd::String(what) + ", got '" +
                                   shown(cur()) + "'");
        return dstd::String(advance().text);
    }
    Res<dstd::String> int_text(dstd::StringView what) {
        auto t = expect_token(Tok::Int, what);
        if (!t) return dstd::Unexpected(t.error());
        return dstd::String((*t)->text);
    }
    // A row count: an integer, or a parameter kept as `$name`.
    Res<dstd::String> count_text() {
        if (at(Tok::Param)) return "$" + dstd::String(advance().text);
        return int_text("a row count or a parameter");
    }
    Res<dstd::Pair<ExprPtr, ExprPtr>> join_key() {
        auto left = level(AddLevel);
        if (!left) return dstd::Unexpected(left.error());
        ExprPtr right;
        if (accept(Tok::Eq)) {
            auto r = level(AddLevel);
            if (!r) return dstd::Unexpected(r.error());
            right = std::move(*r);
        }
        return dstd::Pair<ExprPtr, ExprPtr>{std::move(*left), std::move(right)};
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
        auto e = dstd::make_unique<Expr>();
        e->node = std::move(node);
        e->span = span_from(first);
        return e;
    }

    Res<Def> definition() {
        Def def;
        auto n = name("a macro name");
        if (!n) return dstd::Unexpected(n.error());
        def.name = std::move(*n);
        if (accept(Tok::Lparen)) {
            if (!at(Tok::Rparen)) {
                do {
                    auto param = name("a parameter name");
                    if (!param) return dstd::Unexpected(param.error());
                    def.params.push_back(std::move(*param));
                } while (accept(Tok::Comma));
            }
            auto close = expect(Tok::Rparen, "')'");
            if (!close) return dstd::Unexpected(close.error());
        }
        auto assign_tok = expect(Tok::Assign, "'=' in a def");
        if (!assign_tok) return dstd::Unexpected(assign_tok.error());
        if (word("from"))
            return fail(cur(),
                        "A macro body starts with a stage or an expression, "
                        "not 'from'");
        if (stage_start()) {
            auto body = pipeline();
            if (!body) return dstd::Unexpected(body.error());
            def.body = std::move(*body);
        } else {
            const Token& first = cur();
            auto e = expr();
            if (!e) return dstd::Unexpected(e.error());
            if (at(Tok::Pipe)) {
                auto p = dstd::make_unique<Pipeline>();
                p->stages.push_back(leading_stage(std::move(*e), first));
                auto body = pipeline_tail(std::move(p));
                if (!body) return dstd::Unexpected(body.error());
                def.body = std::move(*body);
            } else {
                def.body = std::move(*e);
            }
        }
        return def;
    }

    Res<SourceDecl> source_decl() {
        SourceDecl s;
        s.name = dstd::String(advance().text);
        auto open = expect(Tok::Lbrace, "'{'");
        if (!open) return dstd::Unexpected(open.error());
        while (!at(Tok::Rbrace)) {
            if (word("def") && peek(1).kind == Tok::Name) {
                advance();
                auto def = definition();
                if (!def) return dstd::Unexpected(def.error());
                s.defs.push_back(std::move(*def));
            } else {
                RowSet r;
                auto n = name("a row set name or 'def'");
                if (!n) return dstd::Unexpected(n.error());
                r.name = std::move(*n);
                auto eq = expect(Tok::Assign, "'=' after a row set name");
                if (!eq) return dstd::Unexpected(eq.error());
                auto body = pipeline();
                if (!body) return dstd::Unexpected(body.error());
                r.pipeline = std::move(*body);
                s.rowsets.push_back(std::move(r));
            }
            if (!accept(Tok::Semi) && !at(Tok::Rbrace))
                return fail(cur(),
                            "Expected ';' between source members, got '" +
                                dstd::String(cur().text) + "'");
        }
        advance();
        return s;
    }

    // At the start of a pipeline a stage word is a stage; a field with that
    // name is written with backticks.
    bool stage_start() const { return at(Tok::Name) && stage_word(cur().text); }

    Res<PipelinePtr> pipeline() {
        auto p = dstd::make_unique<Pipeline>();
        if (word("from")) {
            advance();
            do {
                From f;
                if (at(Tok::String)) {
                    auto text = string_value(advance());
                    if (!text) return dstd::Unexpected(text.error());
                    f.name = std::move(*text);
                    f.quoted = true;
                } else if (at(Tok::Param)) {
                    f.name = Param{dstd::String(advance().text)};
                } else {
                    auto n = name("a file, a row set or a parameter");
                    if (!n) return dstd::Unexpected(n.error());
                    f.name = std::move(*n);
                }
                p->sources.push_back(std::move(f));
            } while (accept(Tok::Comma));
        } else if (stage_start()) {
            auto s = stage();
            if (!s) return dstd::Unexpected(s.error());
            p->stages.push_back(std::move(*s));
        } else {
            const Token& first = cur();
            auto e = expr();
            if (!e) return dstd::Unexpected(e.error());
            p->stages.push_back(leading_stage(std::move(*e), first));
        }
        return pipeline_tail(std::move(p));
    }

    Stage leading_stage(ExprPtr e, const Token& first) {
        Stage s;
        if (auto* c = std::get_if<Call>(&e->node);
            c && c->name.find('.') == dstd::String::npos &&
            first.kind == Tok::Name && first.text != "case")
            s.node = Use{std::move(*c)};
        else
            s.node = Where{std::move(e)};
        s.span = span_from(first);
        return s;
    }

    Res<PipelinePtr> pipeline_tail(PipelinePtr p) {
        while (accept(Tok::Pipe)) {
            if (at(Tok::Name) && !stage_word(cur().text) &&
                peek(1).kind == Tok::Lparen) {
                const Token& first = cur();
                Stage s;
                dstd::String n(advance().text);
                auto c = call(std::move(n));
                if (!c) return dstd::Unexpected(c.error());
                s.node = Use{std::move(*c)};
                s.span = span_from(first);
                p->stages.push_back(std::move(s));
                continue;
            }
            if (!at(Tok::Name) || !stage_word(cur().text))
                return fail(cur(), "Expected a stage after '|', got '" +
                                       dstd::String(cur().text) + "'");
            auto s = stage();
            if (!s) return dstd::Unexpected(s.error());
            p->stages.push_back(std::move(*s));
        }
        return p;
    }

    Res<dstd::Vector<Assign>> block() {
        auto open = expect(Tok::Lbrace, "'{'");
        if (!open) return dstd::Unexpected(open.error());
        dstd::Vector<Assign> out;
        if (!at(Tok::Rbrace)) {
            do {
                if (at(Tok::Name) && peek(1).kind == Tok::Assign) {
                    auto a = assign();
                    if (!a) return dstd::Unexpected(a.error());
                    out.push_back(std::move(*a));
                    continue;
                }
                const Token& first = cur();
                Assign a;
                auto e = expr();
                if (!e) return dstd::Unexpected(e.error());
                a.value = std::move(*e);
                a.name = implicit_name(to_text(*a.value));
                if (a.name.empty())
                    return fail(first, "Name this entry: 'name = ...'");
                out.push_back(std::move(a));
            } while (accept(Tok::Comma));
        }
        auto close = expect(Tok::Rbrace, "'}'");
        if (!close) return dstd::Unexpected(close.error());
        return out;
    }

    // Each run of characters other than letters, digits and '_' becomes
    // one '_', trimmed at both ends; a leading digit or a keyword gets '_'.
    static dstd::String implicit_name(dstd::StringView text) {
        dstd::String out;
        for (unsigned char c : text) {
            if (std::isalnum(c) || c == '_')
                out += static_cast<char>(c);
            else if (!out.empty() && out.back() != '_')
                out += '_';
        }
        const auto first = out.find_first_not_of('_');
        if (first == dstd::String::npos) return {};
        out = out.substr(first, out.find_last_not_of('_') - first + 1);
        if (std::isdigit(static_cast<unsigned char>(out[0])) || reserved(out))
            out.insert(out.begin(), '_');
        return out;
    }

    Res<Assign> assign() {
        Assign a;
        auto n = name("a field name");
        if (!n) return dstd::Unexpected(n.error());
        a.name = std::move(*n);
        auto eq = expect(Tok::Assign, "'='");
        if (!eq) return dstd::Unexpected(eq.error());
        auto v = expr();
        if (!v) return dstd::Unexpected(v.error());
        a.value = std::move(*v);
        return a;
    }

    Res<Item> item() {
        Item it;
        if (at(Tok::Name) && peek(1).kind == Tok::Assign) {
            auto n = name("a field name");
            if (!n) return dstd::Unexpected(n.error());
            it.name = std::move(*n);
            advance();
            auto v = expr();
            if (!v) return dstd::Unexpected(v.error());
            it.value = std::move(*v);
        } else {
            auto v = expr();
            if (!v) return dstd::Unexpected(v.error());
            it.value = std::move(*v);
            if (accept_word("as")) {
                auto n = name("a field name");
                if (!n) return dstd::Unexpected(n.error());
                it.name = std::move(*n);
            }
        }
        return it;
    }

    Res<dstd::Vector<SortKey>> sort_keys() {
        dstd::Vector<SortKey> keys;
        do {
            SortKey k;
            if (accept(Tok::Minus)) k.descending = true;
            auto v = expr();
            if (!v) return dstd::Unexpected(v.error());
            k.value = std::move(*v);
            if (accept_word("nulls")) {
                if (accept_word("first"))
                    k.nulls = NullsOrder::First;
                else if (accept_word("last"))
                    k.nulls = NullsOrder::Last;
                else
                    return fail(cur(),
                                "Expected 'first' or 'last' after 'nulls'");
            }
            keys.push_back(std::move(k));
        } while (accept(Tok::Comma));
        return keys;
    }

    Res<dstd::Vector<ExprPtr>> expr_list() {
        dstd::Vector<ExprPtr> out;
        do {
            auto e = expr();
            if (!e) return dstd::Unexpected(e.error());
            out.push_back(std::move(*e));
        } while (accept(Tok::Comma));
        return out;
    }

    Res<PipelinePtr> from_pipeline(dstd::StringView what) {
        auto open = expect(Tok::Lparen, "'('");
        if (!open) return dstd::Unexpected(open.error());
        if (!word("from"))
            return fail(cur(), dstd::String(what) + " starts with 'from'");
        auto p = pipeline();
        if (!p) return dstd::Unexpected(p.error());
        auto close = expect(Tok::Rparen, "')'");
        if (!close) return dstd::Unexpected(close.error());
        return p;
    }

    bool pipeline_end() const {
        return at(Tok::Pipe) || at(Tok::End) || at(Tok::Semi) ||
               at(Tok::Rparen) || at(Tok::Rbrace);
    }

    Res<Stage> stage() {
        const Token& first = cur();
        const dstd::String kw = [&] {
            dstd::String w(advance().text);
            ::dftracer::utils::common::detail::lower_ascii(w);
            return w;
        }();
        Stage s;
        if (kw == "where") {
            auto e = expr();
            if (!e) return dstd::Unexpected(e.error());
            s.node = Where{std::move(*e)};
        } else if (kw == "derive") {
            Derive d;
            do {
                auto a = assign();
                if (!a) return dstd::Unexpected(a.error());
                d.fields.push_back(std::move(*a));
            } while (accept(Tok::Comma));
            s.node = std::move(d);
        } else if (kw == "parse") {
            Parse p;
            auto col = path();
            if (!col) return dstd::Unexpected(col.error());
            p.column = std::move(*col);
            if (!at(Tok::Regex))
                return fail(cur(),
                            "Expected '~' after the column of 'parse', got '" +
                                shown(cur()) + "'");
            advance();
            if (!at(Tok::String) && !at(Tok::Param))
                return fail(cur(),
                            "Expected a pattern string or a parameter, got '" +
                                shown(cur()) + "'");
            auto pat = primary();
            if (!pat) return dstd::Unexpected(pat.error());
            p.pattern = std::move(*pat);
            s.node = std::move(p);
        } else if (kw == "select") {
            Select sel;
            do {
                wild_start_ = &cur();
                auto it = item();
                if (!it) return dstd::Unexpected(it.error());
                sel.items.push_back(std::move(*it));
                const auto* wp =
                    std::get_if<Path>(&sel.items.back().value->node);
                if (wp && has_wildcard(*wp) && !sel.items.back().name.empty())
                    return fail(cur(), "A wildcard path cannot be renamed");
            } while (accept(Tok::Comma));
            s.node = std::move(sel);
        } else if (kw == "drop") {
            Drop d;
            do {
                wild_start_ = &cur();
                auto pa = path();
                if (!pa) return dstd::Unexpected(pa.error());
                d.paths.push_back(std::move(*pa));
            } while (accept(Tok::Comma));
            s.node = std::move(d);
        } else if (kw == "rename") {
            Rename r;
            do {
                auto n = name("a field name");
                if (!n) return dstd::Unexpected(n.error());
                auto eq = expect(Tok::Assign, "'='");
                if (!eq) return dstd::Unexpected(eq.error());
                auto pa = path();
                if (!pa) return dstd::Unexpected(pa.error());
                r.pairs.emplace_back(std::move(*n), std::move(*pa));
            } while (accept(Tok::Comma));
            s.node = std::move(r);
        } else if (kw == "distinct") {
            Distinct d;
            if (!pipeline_end()) {
                do {
                    auto it = item();
                    if (!it) return dstd::Unexpected(it.error());
                    d.keys.push_back(std::move(*it));
                } while (accept(Tok::Comma));
            }
            s.node = std::move(d);
        } else if (kw == "group") {
            Group g;
            if (!at(Tok::Lbrace)) {
                do {
                    auto it = item();
                    if (!it) return dstd::Unexpected(it.error());
                    g.keys.push_back(std::move(*it));
                } while (accept(Tok::Comma));
            }
            auto aggs = block();
            if (!aggs) return dstd::Unexpected(aggs.error());
            g.aggregates = std::move(*aggs);
            s.node = std::move(g);
        } else if (kw == "agg") {
            auto aggs = block();
            if (!aggs) return dstd::Unexpected(aggs.error());
            s.node = Agg{std::move(*aggs)};
        } else if (kw == "window") {
            Window w;
            if (!at(Tok::Lbrace) && !word("sort")) {
                do {
                    auto it = item();
                    if (!it) return dstd::Unexpected(it.error());
                    w.partition.push_back(std::move(*it));
                } while (accept(Tok::Comma));
            }
            if (accept_word("sort")) {
                auto keys = sort_keys();
                if (!keys) return dstd::Unexpected(keys.error());
                w.order = std::move(*keys);
            }
            auto fields = block();
            if (!fields) return dstd::Unexpected(fields.error());
            w.fields = std::move(*fields);
            s.node = std::move(w);
        } else if (kw == "pivot") {
            Pivot p;
            auto key = level(AddLevel);
            if (!key) return dstd::Unexpected(key.error());
            p.key = std::move(*key);
            if (accept_word("in")) {
                auto open = expect(Tok::Lbracket, "'['");
                if (!open) return dstd::Unexpected(open.error());
                if (!at(Tok::Rbracket)) {
                    do {
                        auto v = expr();
                        if (!v) return dstd::Unexpected(v.error());
                        p.values.push_back(std::move(*v));
                        if (accept_word("as")) {
                            auto label = name("a column name");
                            if (!label) return dstd::Unexpected(label.error());
                            p.labels.push_back(std::move(*label));
                        } else {
                            p.labels.push_back(dstd::String());
                        }
                    } while (accept(Tok::Comma));
                }
                auto close = expect(Tok::Rbracket, "']'");
                if (!close) return dstd::Unexpected(close.error());
            }
            auto aggs = block();
            if (!aggs) return dstd::Unexpected(aggs.error());
            p.aggregates = std::move(*aggs);
            s.node = std::move(p);
        } else if (kw == "unpivot") {
            Unpivot u;
            do {
                wild_start_ = &cur();
                auto pa = path();
                if (!pa) return dstd::Unexpected(pa.error());
                u.paths.push_back(std::move(*pa));
            } while (accept(Tok::Comma));
            auto as = expect_word("as");
            if (!as) return dstd::Unexpected(as.error());
            auto key = name("a field name");
            if (!key) return dstd::Unexpected(key.error());
            u.key_name = std::move(*key);
            auto comma = expect(Tok::Comma, "','");
            if (!comma) return dstd::Unexpected(comma.error());
            auto value = name("a field name");
            if (!value) return dstd::Unexpected(value.error());
            u.value_name = std::move(*value);
            s.node = std::move(u);
        } else if (kw == "sort") {
            auto keys = sort_keys();
            if (!keys) return dstd::Unexpected(keys.error());
            s.node = Sort{std::move(*keys)};
        } else if (kw == "take") {
            Take t;
            auto count = count_text();
            if (!count) return dstd::Unexpected(count.error());
            t.count = std::move(*count);
            if (accept(Tok::Dotdot)) {
                auto last = count_text();
                if (!last) return dstd::Unexpected(last.error());
                t.last = std::move(*last);
                if (word("by")) return fail(cur(), "'take a..b' takes no 'by'");
            }
            if (accept_word("by")) {
                do {
                    auto v = expr();
                    if (!v) return dstd::Unexpected(v.error());
                    t.by.push_back(std::move(*v));
                } while (accept(Tok::Comma));
                if (accept_word("sort")) {
                    auto keys = sort_keys();
                    if (!keys) return dstd::Unexpected(keys.error());
                    t.order = std::move(*keys);
                }
            }
            s.node = std::move(t);
        } else if (kw == "skip") {
            auto count = count_text();
            if (!count) return dstd::Unexpected(count.error());
            s.node = Skip{std::move(*count)};
        } else if (kw == "sample") {
            Sample sm;
            if (at(Tok::Int) || at(Tok::Float))
                sm.amount = dstd::String(advance().text);
            else if (at(Tok::Param))
                sm.amount = "$" + dstd::String(advance().text);
            else
                return fail(cur(),
                            "Expected a row count, a percentage or a "
                            "parameter, got '" +
                                shown(cur()) + "'");
            sm.percent = accept(Tok::Percent);
            if (accept_word("seed")) {
                auto seed = count_text();
                if (!seed) return dstd::Unexpected(seed.error());
                sm.seed = std::move(*seed);
            }
            s.node = std::move(sm);
        } else if (kw == "expand") {
            Expand e;
            auto pa = path();
            if (!pa) return dstd::Unexpected(pa.error());
            e.path = std::move(*pa);
            if (accept_word("as")) {
                auto n = name("a field name");
                if (!n) return dstd::Unexpected(n.error());
                e.as = std::move(*n);
            }
            if (accept_word("with_index")) {
                auto n = name("a field name");
                if (!n) return dstd::Unexpected(n.error());
                e.with_index = std::move(*n);
            }
            e.keep_empty = accept_word("keep_empty");
            s.node = std::move(e);
        } else if (kw == "lookup") {
            Lookup l;
            if (at(Tok::Lparen)) {
                auto side = from_pipeline("A lookup side");
                if (!side) return dstd::Unexpected(side.error());
                l.side = std::move(*side);
            } else {
                auto n = name("a row set");
                if (!n) return dstd::Unexpected(n.error());
                l.rowset = std::move(*n);
            }
            auto on = expect_word("on");
            if (!on) return dstd::Unexpected(on.error());
            do {
                auto k = join_key();
                if (!k) return dstd::Unexpected(k.error());
                l.keys.push_back(std::move(*k));
            } while (accept(Tok::Comma));
            if (word("inner") || word("anti")) {
                l.kind = word("inner") ? LookupKind::Inner : LookupKind::Anti;
                advance();
                if (word("asof") || word("overlap"))
                    return fail(cur(),
                                "'inner' and 'anti' do not combine with "
                                "'asof' or 'overlap'");
                if (l.kind == LookupKind::Anti && word("into"))
                    return fail(cur(),
                                "'anti' adds no column, so it takes no 'into'");
            }
            if (accept_word("overlap")) {
                OverlapLookup o;
                o.rowset = std::move(l.rowset);
                o.side = std::move(l.side);
                o.keys = std::move(l.keys);
                if (accept_word("into")) {
                    auto n = name("a field name");
                    if (!n) return dstd::Unexpected(n.error());
                    o.into = std::move(*n);
                }
                s.node = std::move(o);
            } else if (accept_word("asof")) {
                AsofLookup a;
                a.rowset = std::move(l.rowset);
                a.side = std::move(l.side);
                a.keys = std::move(l.keys);
                auto time = join_key();
                if (!time) return dstd::Unexpected(time.error());
                a.time = std::move(*time);
                if (accept_word("backward"))
                    a.direction = AsofDirection::Backward;
                else if (accept_word("forward"))
                    a.direction = AsofDirection::Forward;
                else if (accept_word("nearest"))
                    a.direction = AsofDirection::Nearest;
                if (accept_word("within")) {
                    auto within = level(AddLevel);
                    if (!within) return dstd::Unexpected(within.error());
                    a.within = std::move(*within);
                }
                if (word("into"))
                    return fail(cur(), "'asof' and 'into' cannot be combined");
                s.node = std::move(a);
            } else {
                if (accept_word("into")) {
                    auto n = name("a field name");
                    if (!n) return dstd::Unexpected(n.error());
                    l.into = std::move(*n);
                }
                s.node = std::move(l);
            }
        } else if (kw == "union") {
            if (at(Tok::Name) || at(Tok::String)) {
                auto other = dstd::make_unique<Pipeline>();
                From f;
                f.quoted = at(Tok::String);
                if (f.quoted) {
                    auto text = string_value(advance());
                    if (!text) return dstd::Unexpected(text.error());
                    f.name = std::move(*text);
                } else {
                    auto n = name("a file, a row set or a let");
                    if (!n) return dstd::Unexpected(n.error());
                    f.name = std::move(*n);
                }
                other->sources.push_back(std::move(f));
                s.node = Union{std::move(other)};
            } else {
                auto other = from_pipeline("A union pipeline");
                if (!other) return dstd::Unexpected(other.error());
                s.node = Union{std::move(*other)};
            }
        } else if (kw == "call") {
            const Token& at_name = cur();
            auto first_name = name("a function name");
            if (!first_name) return dstd::Unexpected(first_name.error());
            dstd::String n = std::move(*first_name);
            while (at(Tok::Dot) && peek(1).kind == Tok::Name) {
                advance();
                n += '.';
                n += advance().text;
            }
            if (!at(Tok::Lparen))
                return fail(at_name, "Expected '(' after the name");
            auto c = call(std::move(n));
            if (!c) return dstd::Unexpected(c.error());
            s.node = CallStage{std::move(*c)};
        } else if (kw == "time_range") {
            TimeRange t;
            if (!at(Tok::Dotdot)) {
                auto low = expr();
                if (!low) return dstd::Unexpected(low.error());
                t.low = std::move(*low);
            }
            auto dots = expect(Tok::Dotdot, "'..'");
            if (!dots) return dstd::Unexpected(dots.error());
            if (!at(Tok::Pipe) && !at(Tok::End) && !word("overlap")) {
                auto high = expr();
                if (!high) return dstd::Unexpected(high.error());
                t.high = std::move(*high);
            }
            if (!t.low && !t.high)
                return fail(cur(), "'time_range' needs a low or a high bound");
            t.overlap = accept_word("overlap");
            s.node = std::move(t);
        } else if (kw == "call_tree") {
            s.node = CallTree{};
        } else if (kw == "bucket") {
            Bucket b;
            auto width = expr();
            if (!width) return dstd::Unexpected(width.error());
            b.width = std::move(*width);
            if (accept_word("every")) {
                auto every = expr();
                if (!every) return dstd::Unexpected(every.error());
                b.every = std::move(*every);
            }
            if (accept_word("at")) {
                auto when = expr();
                if (!when) return dstd::Unexpected(when.error());
                b.at = std::move(*when);
            }
            b.fill = accept_word("fill");
            if (b.fill) {
                if (accept_word("forward"))
                    b.fill_mode = FillMode::Forward;
                else if (accept_word("linear"))
                    b.fill_mode = FillMode::Linear;
            }
            if (word("from")) {
                if (!b.fill)
                    return fail(cur(), "'from .. to' sets the range of 'fill'");
                advance();
                auto low = expr();
                if (!low) return dstd::Unexpected(low.error());
                b.low = std::move(*low);
                auto to = expect_word("to");
                if (!to) return dstd::Unexpected(to.error());
                auto high = expr();
                if (!high) return dstd::Unexpected(high.error());
                b.high = std::move(*high);
            }
            if (accept_word("as")) {
                auto n = name("a field name");
                if (!n) return dstd::Unexpected(n.error());
                b.as = std::move(*n);
            }
            s.node = std::move(b);
        } else if (kw == "session") {
            Session ss;
            if (!word("gap")) {
                do {
                    auto it = item();
                    if (!it) return dstd::Unexpected(it.error());
                    ss.keys.push_back(std::move(*it));
                } while (accept(Tok::Comma));
            }
            auto gap_word = expect_word("gap");
            if (!gap_word) return dstd::Unexpected(gap_word.error());
            auto gap = expr();
            if (!gap) return dstd::Unexpected(gap.error());
            ss.gap = std::move(*gap);
            if (accept_word("max")) {
                auto max = expr();
                if (!max) return dstd::Unexpected(max.error());
                ss.max = std::move(*max);
            }
            if (accept_word("as")) {
                auto n = name("a field name");
                if (!n) return dstd::Unexpected(n.error());
                ss.as = std::move(*n);
            }
            s.node = std::move(ss);
        } else {
            return fail(first,
                        "Unknown stage '" + dstd::String(first.text) + "'");
        }
        s.span = span_from(first);
        return s;
    }

    Res<ExprPtr> expr() { return level(OrLevel); }

    Res<ExprPtr> level(int lv) {
        switch (lv) {
            case OrLevel:
            case AndLevel: {
                const Token& first = cur();
                const dstd::StringView op = lv == OrLevel ? "or" : "and";
                auto left = lv == OrLevel ? level(AndLevel) : not_expr();
                if (!left) return left;
                while (word(op)) {
                    advance();
                    auto right = lv == OrLevel ? level(AndLevel) : not_expr();
                    if (!right) return right;
                    *left = make(
                        Binary{lv == OrLevel ? BinaryOp::Or : BinaryOp::And,
                               std::move(*left), std::move(*right)},
                        first);
                }
                return left;
            }
            case CmpLevel:
                return comparison();
            case AddLevel:
            case MulLevel: {
                const Token& first = cur();
                auto left = lv == AddLevel ? level(MulLevel) : unary();
                if (!left) return left;
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
                    auto right = lv == AddLevel ? level(MulLevel) : unary();
                    if (!right) return right;
                    *left = make(
                        Binary{op, std::move(*left), std::move(*right)}, first);
                }
                return left;
            }
            default:
                return unary();
        }
    }

    Res<ExprPtr> not_expr() {
        const Token& first = cur();
        if (accept_word("not")) {
            auto operand = not_expr();
            if (!operand) return operand;
            return make(Unary{UnaryOp::Not, std::move(*operand)}, first);
        }
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

    Res<ExprPtr> comparison() {
        const Token& first = cur();
        if (legacy_contains()) {
            Contains c;
            auto text = string_value(advance());
            if (!text) return dstd::Unexpected(text.error());
            c.text = std::move(*text);
            c.negated = accept_word("not");
            auto in = expect_word("in");
            if (!in) return dstd::Unexpected(in.error());
            if (word("any") && peek(1).kind == Tok::Lparen &&
                peek(1).adjacent) {
                advance();
                advance();
                auto pa = path();
                if (!pa) return dstd::Unexpected(pa.error());
                c.path = std::move(*pa);
                c.any = true;
                auto close = expect(Tok::Rparen, "')' after any(path");
                if (!close) return dstd::Unexpected(close.error());
            } else {
                auto pa = path();
                if (!pa) return dstd::Unexpected(pa.error());
                c.path = std::move(*pa);
            }
            return make(std::move(c), first);
        }
        auto left = level(AddLevel);
        if (!left) return left;
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
                return keyword_comparison(std::move(*left), first);
        }
        advance();
        auto right = level(AddLevel);
        if (!right) return right;
        return make(Binary{op, std::move(*left), std::move(*right)}, first);
    }

    Res<ExprPtr> keyword_comparison(ExprPtr left, const Token& first) {
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
                if (!at(Tok::Rbracket)) {
                    auto list = expr_list();
                    if (!list) return dstd::Unexpected(list.error());
                    in.list = std::move(*list);
                }
                auto close = expect(Tok::Rbracket, "']' or ','");
                if (!close) return dstd::Unexpected(close.error());
            } else if (at(Tok::Lparen) && peek(1).kind == Tok::Name &&
                       iequals(peek(1).text, "from")) {
                auto sub = from_pipeline("A sub-query");
                if (!sub) return dstd::Unexpected(sub.error());
                in.subquery = std::move(*sub);
            } else if (at(Tok::Param)) {
                in.list_param = dstd::String(advance().text);
            } else {
                return fail(cur(),
                            "Expected '[', '(from' or a parameter after 'in'");
            }
            return make(std::move(in), first);
        }
        if (accept_word("between")) {
            Between b;
            b.subject = std::move(left);
            b.negated = negated;
            auto low = level(AddLevel);
            if (!low) return dstd::Unexpected(low.error());
            b.low = std::move(*low);
            auto and_word = expect_word("and");
            if (!and_word) return dstd::Unexpected(and_word.error());
            auto high = level(AddLevel);
            if (!high) return dstd::Unexpected(high.error());
            b.high = std::move(*high);
            return make(std::move(b), first);
        }
        if (word("like") || word("ilike")) {
            Like l;
            l.icase = iequals(advance().text, "ilike");
            l.subject = std::move(left);
            l.negated = negated;
            if (at(Tok::Param)) {
                l.pattern_param = dstd::String(advance().text);
            } else {
                auto tok = expect_token(Tok::String, "a string pattern");
                if (!tok) return dstd::Unexpected(tok.error());
                auto text = string_value(**tok);
                if (!text) return dstd::Unexpected(text.error());
                l.pattern = std::move(*text);
            }
            if (accept_word("escape")) {
                auto tok = expect_token(Tok::String, "an escape string");
                if (!tok) return dstd::Unexpected(tok.error());
                auto text = string_value(**tok);
                if (!text) return dstd::Unexpected(text.error());
                l.escape = std::move(*text);
            }
            return make(std::move(l), first);
        }
        if (negated)
            return fail(cur(), "Expected 'in', 'between', 'like' or 'ilike'");
        if (accept_word("is")) {
            Is is;
            is.subject = std::move(left);
            is.negated = accept_word("not");
            if (accept_word("missing")) {
                is.missing = true;
            } else {
                auto null_word = expect_word("null");
                if (!null_word) return dstd::Unexpected(null_word.error());
            }
            return make(std::move(is), first);
        }
        return left;
    }

    Res<ExprPtr> unary() {
        const Token& first = cur();
        if (accept(Tok::Minus)) {
            auto operand = unary();
            if (!operand) return operand;
            return make(Unary{UnaryOp::Neg, std::move(*operand)}, first);
        }
        return coalesce();
    }

    Res<ExprPtr> coalesce() {
        const Token& first = cur();
        auto left = postfix();
        if (!left) return left;
        while (accept(Tok::Coalesce)) {
            auto right = postfix();
            if (!right) return right;
            *left = make(
                Binary{BinaryOp::Coalesce, std::move(*left), std::move(*right)},
                first);
        }
        return left;
    }

    Res<ExprPtr> postfix() {
        const Token& first = cur();
        auto prim = primary();
        if (!prim) return prim;
        ExprPtr e = std::move(*prim);
        if (std::holds_alternative<Call>(e->node) && word("over")) {
            advance();
            Over o;
            o.call = std::move(e);
            if (!at(Tok::Int) && !at(Tok::Float) && !at(Tok::Duration) &&
                !at(Tok::Param))
                return fail(cur(),
                            "Expected a number, a duration or a parameter "
                            "after 'over'");
            auto width = primary();
            if (!width) return width;
            o.width = std::move(*width);
            o.rows = accept_word("rows");
            e = make(std::move(o), first);
        }
        while (accept(Tok::Arrow)) {
            Arrow a;
            a.key = std::move(e);
            auto rowset = name("a row set name after '->'");
            if (!rowset) return dstd::Unexpected(rowset.error());
            a.rowset = std::move(*rowset);
            if (at(Tok::Lparen) && cur().adjacent) {
                advance();
                auto target = path();
                if (!target) return dstd::Unexpected(target.error());
                a.target_key = std::move(*target);
                auto close = expect(Tok::Rparen, "')'");
                if (!close) return dstd::Unexpected(close.error());
            }
            auto dot = expect(Tok::Dot, "'.' and a path after the row set");
            if (!dot) return dstd::Unexpected(dot.error());
            auto steps = path_steps(Path{}, cur(), nullptr);
            if (!steps) return dstd::Unexpected(steps.error());
            a.path = std::move(*steps);
            e = make(std::move(a), first);
        }
        return e;
    }

    // Steps after a root: keys joined by '.', each with adjacent indexes. A
    // computed index ends the path; `out` receives it, and is null where
    // only a plain path may stand.
    Res<Path> path_steps(Path p, const Token& first, ExprPtr* out) {
        while (true) {
            if (at(Tok::Star) && !p.steps.empty() && &first == wild_start_) {
                advance();
                p.steps.push_back({"*", false, {}});
            } else if (at(Tok::Name)) {
                if (reserved(cur().text))
                    return fail(cur(),
                                "'" + dstd::String(cur().text) +
                                    "' is a keyword; quote the key as `" +
                                    dstd::String(cur().text) + "`");
                p.steps.push_back({dstd::String(advance().text), false, {}});
            } else if (at(Tok::Qname)) {
                p.steps.push_back({dstd::String(advance().text), true, {}});
            } else if (!p.steps.empty() && at(Tok::Int)) {
                p.steps.push_back({dstd::String(advance().text), false, {}});
            } else if (!p.steps.empty() && at(Tok::Float) &&
                       cur().text.find_first_not_of("0123456789.") ==
                           dstd::StringView::npos) {
                // `a.0.5` lexes 0.5 as one float: two numeric keys.
                const dstd::StringView f = advance().text;
                const auto dot = f.find('.');
                p.steps.push_back({dstd::String(f.substr(0, dot)), false, {}});
                p.steps.push_back({dstd::String(f.substr(dot + 1)), false, {}});
            } else {
                return fail(cur(), "Expected a key, got '" +
                                       dstd::String(cur().text) + "'");
            }
            auto ended = indexes(p, first, out);
            if (!ended.has_value()) return dstd::Unexpected(ended.error());
            if (*ended) return p;
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
    Res<bool> indexes(Path& p, const Token& first, ExprPtr* out) {
        while (at(Tok::Lbracket) && cur().adjacent) {
            if (!literal_index()) {
                const Token& open = cur();
                if (!out)
                    return fail(open, "A computed index is not allowed here");
                advance();
                auto index = expr();
                if (!index) return dstd::Unexpected(index.error());
                auto close = expect(Tok::Rbracket, "']'");
                if (!close) return dstd::Unexpected(close.error());
                if ((at(Tok::Dot) && cur().adjacent) ||
                    (at(Tok::Lbracket) && cur().adjacent))
                    return fail(cur(), "A computed index ends the path");
                *out = make(Index{make(std::move(p), first), std::move(*index)},
                            first);
                return true;
            }
            advance();
            const bool negative = accept(Tok::Minus);
            auto tok = expect_token(Tok::Int, "an array index");
            if (!tok) return dstd::Unexpected(tok.error());
            const Token& n = **tok;
            std::int64_t v = 0;
            const auto r = dstd::from_chars(n.text.data(),
                                            n.text.data() + n.text.size(), v);
            if (r.ec != std::errc{}) return fail(n, "Array index out of range");
            auto close = expect(Tok::Rbracket, "']'");
            if (!close) return dstd::Unexpected(close.error());
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

    Res<Path> path(ExprPtr* out = nullptr) {
        auto p = path_root(out);
        if (!p) return p;
        if (has_wildcard(*p)) {
            const Tok k = cur().kind;
            if (k != Tok::Comma && k != Tok::Pipe && k != Tok::Semi &&
                k != Tok::End && k != Tok::Rparen && k != Tok::Rbrace &&
                k != Tok::Name)
                return fail(cur(),
                            "A wildcard path stands alone in select, drop, "
                            "unpivot or any()/all()");
        }
        return p;
    }

    Res<Path> path_root(ExprPtr* out) {
        const Token& first = cur();
        if (at(Tok::Caret)) {
            advance();
            auto dot = expect(Tok::Dot, "'.' after '^'");
            if (!dot) return dstd::Unexpected(dot.error());
            return path_steps(Path{PathRoot::Enclosing, {}}, first, out);
        }
        if (at(Tok::Dot)) {
            advance();
            Path p{PathRoot::Current, {}};
            if ((at(Tok::Name) && !reserved(cur().text)) || at(Tok::Qname))
                return path_steps(std::move(p), first, out);
            auto ended = indexes(p, first, out);
            if (!ended.has_value()) return dstd::Unexpected(ended.error());
            return p;
        }
        return path_steps(Path{}, first, out);
    }

    Res<ExprPtr> path_expr(const Token& t) {
        ExprPtr computed;
        auto p = path(&computed);
        if (!p) return dstd::Unexpected(p.error());
        return computed ? std::move(computed) : make(std::move(*p), t);
    }

    Res<Call> call(dstd::String n) {
        Call c;
        c.name = std::move(n);
        auto open = expect(Tok::Lparen, "'('");
        if (!open) return dstd::Unexpected(open.error());
        if (c.name == "any" || c.name == "all") wild_start_ = &cur();
        if (!at(Tok::Rparen)) {
            do {
                Arg a;
                if (at(Tok::Name) && peek(1).kind == Tok::Assign) {
                    auto n2 = name("an argument name");
                    if (!n2) return dstd::Unexpected(n2.error());
                    a.name = std::move(*n2);
                    advance();
                }
                auto v = expr();
                if (!v) return dstd::Unexpected(v.error());
                a.value = std::move(*v);
                c.args.push_back(std::move(a));
            } while (accept(Tok::Comma));
        }
        auto close = expect(Tok::Rparen, "')'");
        if (!close) return dstd::Unexpected(close.error());
        return c;
    }

    // `case { c => a, ..., else => d }`: the call `case(c, a, ..., d)`.
    Res<Call> case_block() {
        advance();
        const Token& open = advance();
        Call c;
        c.name = "case";
        if (at(Tok::Rbrace))
            return fail(open, "A case block needs a 'c => v' pair");
        do {
            if (word("else") && peek(1).kind == Tok::Fatarrow) {
                advance();
                advance();
                auto v = expr();
                if (!v) return dstd::Unexpected(v.error());
                c.args.push_back({{}, std::move(*v)});
                if (!at(Tok::Rbrace))
                    return fail(
                        cur(), "'else' must be the last entry of a case block");
                break;
            }
            auto cond = expr();
            if (!cond) return dstd::Unexpected(cond.error());
            c.args.push_back({{}, std::move(*cond)});
            auto arrow = expect(Tok::Fatarrow, "'=>'");
            if (!arrow) return dstd::Unexpected(arrow.error());
            auto v = expr();
            if (!v) return dstd::Unexpected(v.error());
            c.args.push_back({{}, std::move(*v)});
        } while (accept(Tok::Comma));
        if (c.args.size() % 2 == 0)
            c.args.push_back({{}, make(Literal{Null{}, {}}, cur())});
        auto close = expect(Tok::Rbrace, "'}'");
        if (!close) return dstd::Unexpected(close.error());
        return c;
    }

    Res<ExprPtr> number(const Token& t) {
        Literal lit;
        lit.number_text = dstd::String(t.text);
        if (t.kind == Tok::Int) {
            std::int64_t v = 0;
            const auto r = dstd::from_chars(t.text.data(),
                                            t.text.data() + t.text.size(), v);
            if (r.ec != std::errc{})
                return fail(t,
                            "Invalid integer: '" + dstd::String(t.text) + "'");
            lit.value = static_cast<std::uint64_t>(v);
        } else {
            const dstd::Optional<double> d = dstd::from_chars<double>(t.text);
            if (!d)
                return fail(t, "Invalid float: '" + dstd::String(t.text) + "'");
            lit.value = *d;
        }
        advance();
        return make(std::move(lit), t);
    }

    Res<ExprPtr> primary() {
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
                return make(Duration{dstd::String(t.text.substr(0, i)),
                                     dstd::String(t.text.substr(i))},
                            t);
            }
            case Tok::String: {
                advance();
                auto text = string_value(t);
                if (!text) return dstd::Unexpected(text.error());
                return make(Literal{std::move(*text), {}}, t);
            }
            case Tok::Param:
                advance();
                return make(Param{dstd::String(t.text)}, t);
            case Tok::Lparen: {
                if (peek(1).kind == Tok::Name &&
                    iequals(peek(1).text, "from")) {
                    auto sub = from_pipeline("A sub-query");
                    if (!sub) return dstd::Unexpected(sub.error());
                    return make(Subquery{std::move(*sub)}, t);
                }
                advance();
                auto e = expr();
                if (!e) return e;
                if (at(Tok::Comma)) {
                    Tuple tu;
                    tu.items.push_back(std::move(*e));
                    while (accept(Tok::Comma)) {
                        auto item_expr = expr();
                        if (!item_expr) return item_expr;
                        tu.items.push_back(std::move(*item_expr));
                    }
                    auto close = expect(Tok::Rparen, "')'");
                    if (!close) return dstd::Unexpected(close.error());
                    return make(std::move(tu), t);
                }
                auto close = expect(Tok::Rparen, "')'");
                if (!close) return dstd::Unexpected(close.error());
                return e;
            }
            case Tok::Lbracket: {
                advance();
                List l;
                if (!at(Tok::Rbracket)) {
                    auto items = expr_list();
                    if (!items) return dstd::Unexpected(items.error());
                    l.items = std::move(*items);
                }
                auto close = expect(Tok::Rbracket, "']' or ','");
                if (!close) return dstd::Unexpected(close.error());
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
                if (iequals(t.text, "case") && peek(1).kind == Tok::Lbrace) {
                    auto c = case_block();
                    if (!c) return dstd::Unexpected(c.error());
                    return make(std::move(*c), t);
                }
                if (reserved(t.text))
                    return fail(t, "Expected a value, got the keyword '" +
                                       dstd::String(t.text) + "'");
                // A dotted name directly followed by '(' is a call.
                std::size_t k = 0;
                while (peek(k).kind == Tok::Name &&
                       peek(k + 1).kind == Tok::Dot &&
                       peek(k + 2).kind == Tok::Name && peek(k + 1).adjacent &&
                       peek(k + 2).adjacent)
                    k += 2;
                if (peek(k).kind == Tok::Name &&
                    peek(k + 1).kind == Tok::Lparen && peek(k + 1).adjacent) {
                    dstd::String n(advance().text);
                    for (std::size_t i = 0; i < k; i += 2) {
                        advance();
                        n += '.';
                        n += advance().text;
                    }
                    auto c = call(std::move(n));
                    if (!c) return dstd::Unexpected(c.error());
                    return make(std::move(*c), t);
                }
                return path_expr(t);
            }
            default:
                return fail(t, t.kind == Tok::End
                                   ? "Unexpected end of query"
                                   : "Expected a value, got '" +
                                         dstd::String(t.text) + "'");
        }
    }
};

}  // namespace

bool is_stage_word(dstd::StringView word) { return stage_word(word); }

dftracer::utils::dstd::Expected<Program, duql::DuqlError> parse(
    dstd::StringView source) {
    auto tokens = lex(source);
    if (!tokens) return dftracer::utils::dstd::Unexpected(tokens.error());
    return Parser(source, std::move(*tokens)).program();
}

}  // namespace dftracer::utils::duql::syntax
