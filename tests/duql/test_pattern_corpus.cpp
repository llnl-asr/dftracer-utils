// Every pattern of the golden filters, matched by the new engine and by the
// std::regex translation it replaced, on ASCII strings without newlines: the
// results agree. Newlines and multi-byte characters are the listed changes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace duql = dftracer::utils::duql;

namespace {

struct Leaf {
    duql::MatchOp op;
    std::string pattern;
    duql::PatternPtr compiled;
};

void leaves(const duql::QueryNode& node, std::vector<Leaf>& out) {
    std::visit(
        [&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, duql::AndNode> ||
                          std::is_same_v<T, duql::OrNode>) {
                leaves(*n.left, out);
                leaves(*n.right, out);
            } else if constexpr (std::is_same_v<T, duql::NotNode>) {
                leaves(*n.operand, out);
            } else if constexpr (std::is_same_v<T, duql::MatchNode>) {
                out.push_back({n.op, n.pattern, n.compiled});
            }
        },
        node.data);
}

void append_escaped(std::string& out, char c) {
    if (std::string_view(".\\+*?()[]{}^$|").find(c) != std::string_view::npos)
        out += '\\';
    out += c;
}

// The std::regex each operator used before this engine.
std::regex old_regex(const Leaf& l) {
    auto flags = std::regex::ECMAScript;
    std::string src;
    switch (l.op) {
        case duql::MatchOp::ILIKE:
            flags |= std::regex::icase;
            [[fallthrough]];
        case duql::MatchOp::LIKE:
            src = "^";
            for (char c : l.pattern) {
                if (c == '%')
                    src += ".*";
                else if (c == '_')
                    src += '.';
                else
                    append_escaped(src, c);
            }
            src += '$';
            break;
        case duql::MatchOp::IREGEX:
            flags |= std::regex::icase;
            [[fallthrough]];
        case duql::MatchOp::REGEX:
            src = l.pattern;
            break;
        case duql::MatchOp::ICONTAINS:
            flags |= std::regex::icase;
            for (char c : l.pattern) append_escaped(src, c);
            break;
    }
    return std::regex(src, flags);
}

// ASCII strings built from the pattern's own characters, so matches occur.
std::vector<std::string> strings_for(const std::string& pattern,
                                     std::mt19937& rng) {
    std::string pool = "abcXYZ_-./019 ";
    for (char c : pattern)
        if (c >= 0x20 && c < 0x7F) pool += c;
    std::vector<std::string> out = {"", pattern};
    std::string plain;
    for (char c : pattern)
        if (c != '%' && c != '_' && c != '\\' && c != '^' && c != '$')
            plain += c;
    out.push_back(plain);
    out.push_back("x" + plain + "y");
    for (int k = 0; k < 40; ++k) {
        std::string s;
        for (int n = rng() % 16; n > 0; --n) s += pool[rng() % pool.size()];
        out.push_back(s);
    }
    return out;
}

}  // namespace

TEST_CASE("golden patterns keep their results") {
    std::ifstream in(DUQL_GOLDEN_PATH);
    REQUIRE(in);
    simdjson::dom::parser json;
    std::mt19937 rng(12);
    std::set<std::pair<int, std::string>> seen;
    std::size_t compared = 0;
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        simdjson::dom::element row;
        const simdjson::padded_string padded(line);
        REQUIRE(json.parse(padded).get(row) == simdjson::SUCCESS);
        if (!row["ok"].get_bool().value()) continue;
        const auto tree =
            duql::parse(std::string(row["q"].get_string().value()));
        REQUIRE(tree);
        std::vector<Leaf> found;
        leaves(**tree, found);
        for (const Leaf& l : found) {
            if (!seen.emplace(static_cast<int>(l.op), l.pattern).second)
                continue;
            CAPTURE(l.pattern);
            const std::regex re = old_regex(l);
            const bool anchored =
                l.op == duql::MatchOp::LIKE || l.op == duql::MatchOp::ILIKE;
            for (const auto& s : strings_for(l.pattern, rng)) {
                CAPTURE(s);
                const bool old = anchored ? std::regex_match(s, re)
                                          : std::regex_search(s, re);
                CHECK((duql::match(*l.compiled, s) == duql::MatchResult::YES) ==
                      old);
                ++compared;
            }
        }
    }
    CHECK(seen.size() > 10);
    CHECK(compared > 400);
}

TEST_CASE("DataFrame kernels and duql agree on the golden patterns") {
    namespace df = dftracer::utils::dataframe;
    std::ifstream in(DUQL_GOLDEN_PATH);
    REQUIRE(in);
    simdjson::dom::parser json;
    std::mt19937 rng(34);
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        simdjson::dom::element row;
        const simdjson::padded_string padded(line);
        REQUIRE(json.parse(padded).get(row) == simdjson::SUCCESS);
        if (!row["ok"].get_bool().value()) continue;
        const auto tree =
            duql::parse(std::string(row["q"].get_string().value()));
        REQUIRE(tree);
        std::vector<Leaf> found;
        leaves(**tree, found);
        for (const Leaf& l : found) {
            const bool regex = l.op == duql::MatchOp::REGEX;
            // str_like escapes with `\`; duql like has no escape character.
            const bool like = l.op == duql::MatchOp::LIKE &&
                              l.pattern.find('\\') == std::string::npos;
            if (!regex && !like) continue;
            CAPTURE(l.pattern);
            const auto strings = strings_for(l.pattern, rng);
            std::vector<std::string_view> views(strings.begin(), strings.end());
            const df::Series col = df::Series::strings(views);
            const df::Series m =
                regex ? col.str_search(l.pattern) : col.str_like(l.pattern);
            REQUIRE(m.handle());
            for (std::size_t i = 0; i < strings.size(); ++i) {
                CAPTURE(strings[i]);
                const bool bit =
                    (m.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
                CHECK(bit == (duql::match(*l.compiled, strings[i]) ==
                              duql::MatchResult::YES));
            }
            if (!regex) continue;
            // str_matches is the whole-string form of the same regex.
            const df::Series whole = col.str_matches(l.pattern);
            const auto full = duql::compile_regex(l.pattern, false, true);
            REQUIRE(whole.handle());
            REQUIRE(full);
            const std::regex re(l.pattern);
            for (std::size_t i = 0; i < strings.size(); ++i) {
                CAPTURE(strings[i]);
                const bool bit =
                    (whole.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
                CHECK(bit == ((*full) && duql::match(**full, strings[i]) ==
                                             duql::MatchResult::YES));
                CHECK(bit == std::regex_match(strings[i], re));
            }
        }
    }
}

#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
namespace {

const std::vector<std::string> WIDE = {
    "\xC3\xA9",     "\xC3\x89",         "\xCE\xB1",         "\xCE\x91",
    "\xE4\xB8\xAD", "\xF0\x9F\x98\x80", "\xC3\xA9\xCE\xB1", "\xC4\xB0"};

std::vector<std::string> wide_strings(const std::string& pattern,
                                      std::mt19937& rng) {
    std::vector<std::string> out = {"caf\xC3\xA9",
                                    "CAF\xC3\x89",
                                    "\xCE\xB1\xCE\xB2\xCE\xB3",
                                    "\xCE\x91\xCE\x92",
                                    "\xE4\xB8\xAD\xE6\x96\x87",
                                    "a\xF0\x9F\x98\x80z",
                                    "\xF0\x9F\x98\x80",
                                    "x\xC3\xA9\nY",
                                    "\xC3\xA9\n",
                                    "ab\xE4\xB8\xAD"
                                    "cd",
                                    "foo \xC3\xA9 bar",
                                    "\xCE\xB1 1",
                                    "\xC2\xA0",
                                    "\xE2\x80\xA8"};
    std::string plain;
    for (char c : pattern)
        if (c != '%' && c != '_' && c != '\\' && c != '^' && c != '$')
            plain += c;
    for (const auto& base : strings_for(pattern, rng)) {
        for (int k = 0; k < 2; ++k) {
            std::string s = base;
            const std::string& w = WIDE[rng() % WIDE.size()];
            const std::size_t at = s.empty() ? 0 : rng() % (s.size() + 1);
            if (k == 0 || s.empty())
                s.insert(at, w);
            else
                s.replace(at == s.size() ? at - 1 : at, 1, w);
            out.push_back(std::move(s));
        }
        if (out.size() > 60) break;
    }
    out.push_back(plain + "\xC3\xA9");
    out.push_back("\xC3\xA9" + plain);
    out.push_back(plain + "\xC3\x89" + plain);
    return out;
}

const std::vector<std::string> INVALID = {"\xC0\xAF",
                                          "\xE0\x80\xAF",
                                          "\xED\xA0\x80",
                                          "\xE4\xB8",
                                          "\x80",
                                          "\xF4\x90\x80\x80",
                                          "\xF8",
                                          "a\xC0\xAF"
                                          "b",
                                          "abcdefgh\xE4\xB8",
                                          "\xC3",
                                          "x\xFF",
                                          "\xF0\x80\x80\x80",
                                          "\xC1\xBF",
                                          "abc\x80"
                                          "def"};

}  // namespace

TEST_CASE("Vectorscan and PCRE2 agree on every golden regex") {
    std::ifstream in(DUQL_GOLDEN_PATH);
    REQUIRE(in);
    simdjson::dom::parser json;
    std::mt19937 rng(31);
    std::set<std::pair<int, std::string>> seen;
    std::size_t compared = 0;
    auto agree = [&](const duql::CompiledPattern& p, const std::string& s) {
        CAPTURE(s);
        CHECK(duql::detail::match_regex(p, s) ==
              duql::detail::match_regex_pcre2(p, s));
        ++compared;
        for (const std::size_t len :
             {duql::detail::HS_MIN_VALUE - 1, duql::detail::HS_MIN_VALUE,
              duql::detail::HS_MIN_VALUE + 1}) {
            if (s.size() >= len) continue;
            const std::string pad(len - s.size(), 'x');
            for (const std::string& v : {s + pad, pad + s}) {
                CAPTURE(v.size());
                CHECK(duql::detail::match_regex(p, v) ==
                      duql::detail::match_regex_pcre2(p, v));
                ++compared;
            }
        }
    };
    std::vector<std::string> extra = {"",
                                      "\n",
                                      "a\n",
                                      "\na",
                                      "a\nb",
                                      "a\n\n",
                                      "ab\n",
                                      "foo bar",
                                      "foo_bar",
                                      "a1 b2\tc",
                                      "AbC",
                                      "abc",
                                      "\x0b",
                                      "caf\xc3\xa9",
                                      "\xc3\xa9",
                                      "a\xc3\xa9"
                                      "b",
                                      "\xff\xfe",
                                      std::string("a\0b", 3)};
    std::vector<std::string> shapes = {"a*",        "",
                                       "a",         "^a$",
                                       "a$",        "^a",
                                       "(?m)^b$",   "(?m)a$",
                                       "(?m)^",     "(?s)a.b",
                                       "a.b",       "a.*",
                                       "\\bfoo\\b", "\\w+",
                                       "\\d",       "\\s",
                                       "\\S+",      "[[:alpha:]]",
                                       "^$",        "(?i)abc",
                                       "abc",       "a|b",
                                       "(a|b)+c",   "a{2,3}",
                                       "\\Aa",      "a\\z",
                                       "a\\Z",      "\\Bo",
                                       "[^a]",      "x?",
                                       "\\p{L}",    "caf\xc3\xa9",
                                       "."};
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        simdjson::dom::element row;
        const simdjson::padded_string padded(line);
        REQUIRE(json.parse(padded).get(row) == simdjson::SUCCESS);
        if (!row["ok"].get_bool().value()) continue;
        const auto tree =
            duql::parse(std::string(row["q"].get_string().value()));
        REQUIRE(tree);
        std::vector<Leaf> found;
        leaves(**tree, found);
        for (const Leaf& l : found) {
            if (l.op != duql::MatchOp::REGEX && l.op != duql::MatchOp::IREGEX)
                continue;
            if (!seen.emplace(static_cast<int>(l.op), l.pattern).second)
                continue;
            CAPTURE(l.pattern);
            auto strings = strings_for(l.pattern, rng);
            strings.insert(strings.end(), extra.begin(), extra.end());
            for (const auto& s : strings) agree(*l.compiled, s);
            for (const bool whole : {false, true}) {
                auto w = duql::compile_regex(
                    l.pattern, l.op == duql::MatchOp::IREGEX, whole);
                REQUIRE(w);
                for (const auto& s : strings) agree(**w, s);
            }
        }
    }
    for (const auto& pat : shapes)
        for (const bool icase : {false, true})
            for (const bool whole : {false, true}) {
                auto p = duql::compile_regex(pat, icase, whole);
                if (!p) continue;
                CAPTURE(pat);
                for (const auto& s : extra) agree(**p, s);
                for (const auto& s : strings_for(pat, rng)) agree(**p, s);
            }
    CHECK(seen.size() > 5);
    CHECK(compared > 2000);
}

TEST_CASE("Vectorscan and PCRE2 agree on non-ASCII and invalid UTF-8") {
    std::ifstream in(DUQL_GOLDEN_PATH);
    REQUIRE(in);
    simdjson::dom::parser json;
    std::mt19937 rng(77);
    std::set<std::string> seen;
    std::size_t compared = 0;
    std::size_t invalid = 0;
    auto agree = [&](const duql::CompiledPattern& p, const std::string& s) {
        CAPTURE(s);
        CHECK(duql::detail::match_regex(p, s) ==
              duql::detail::match_regex_pcre2(p, s));
        ++compared;
    };
    auto run = [&](const std::string& pat, bool icase) {
        for (const bool whole : {false, true}) {
            auto p = duql::compile_regex(pat, icase, whole);
            if (!p) continue;
            CAPTURE(pat);
            CAPTURE(icase);
            CAPTURE(whole);
            for (const auto& s : wide_strings(pat, rng)) agree(**p, s);
            for (const auto& s : INVALID) {
                agree(**p, s);
                ++invalid;
            }
        }
    };
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        simdjson::dom::element row;
        const simdjson::padded_string padded(line);
        REQUIRE(json.parse(padded).get(row) == simdjson::SUCCESS);
        if (!row["ok"].get_bool().value()) continue;
        const auto tree =
            duql::parse(std::string(row["q"].get_string().value()));
        REQUIRE(tree);
        std::vector<Leaf> found;
        leaves(**tree, found);
        for (const Leaf& l : found) {
            if (l.op != duql::MatchOp::REGEX && l.op != duql::MatchOp::IREGEX)
                continue;
            if (!seen.insert(l.pattern).second) continue;
            run(l.pattern, false);
            run(l.pattern, true);
        }
    }
    for (const auto* pat : {".",
                            "..",
                            "^.$",
                            "\\w+",
                            "[^a]",
                            "(?i)abc",
                            "(?i)\xC3\xA9",
                            "\\p{L}",
                            "[[:alpha:]]",
                            "\\b",
                            "\\bcaf",
                            "^",
                            "$",
                            "a.b",
                            "(?s)a.b",
                            "(?m)^.$",
                            "\\W",
                            "\\D",
                            "\\S",
                            "\\B",
                            "[a-z]+",
                            "[\xC3\xA9]",
                            "\xC3\xA9",
                            "\xCE\xB1",
                            "\xE4\xB8\xAD",
                            "x.y",
                            "[^\\x00-\\x7f]",
                            "\\x{e9}",
                            "[[:^alpha:]]",
                            "\\p{Greek}",
                            "\\P{L}",
                            "[\\w]",
                            "[^\\w]",
                            "\\s",
                            "(?i)[a-z]",
                            "(?i)[^a]",
                            "a|\xC3\xA9"}) {
        run(pat, false);
        run(pat, true);
    }
    CHECK(seen.size() > 5);
    CHECK(compared > 4000);
    CHECK(invalid > 400);
}
#endif

namespace {

// Reference decoder: returns the length of the first scalar value at s, or 0.
std::size_t ref_scalar_len(const std::string& s, std::size_t i) {
    const auto b = [&](std::size_t k) {
        return static_cast<unsigned char>(s[i + k]);
    };
    const std::size_t left = s.size() - i;
    const unsigned c = b(0);
    unsigned long cp;
    std::size_t len;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (left < len) return 0;
    for (std::size_t k = 1; k < len; ++k) {
        if ((b(k) & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b(k) & 0x3F);
    }
    static const unsigned long MIN[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < MIN[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return len;
}

bool ref_valid(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = ref_scalar_len(s, i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

void check_utf8(const std::string& core) {
    for (const std::size_t pad : {0u, 5u, 8u}) {
        for (const bool tail : {false, true}) {
            const std::string s = std::string(pad, 'a') + core +
                                  (tail ? std::string(9, 'b') : std::string());
            CAPTURE(pad);
            CAPTURE(tail);
            CAPTURE(core);
            CHECK(duql::detail::valid_utf8(s) == ref_valid(s));
        }
    }
}

}  // namespace

TEST_CASE("valid_utf8 matches a reference decoder") {
    CHECK(duql::detail::valid_utf8(""));
    for (std::size_t n = 0; n <= 200; ++n)
        for (const char* bad :
             {"", "\xC3\xA9", "\x80", "\xE4\xB8", "\xF0\x9F\x98\x80"}) {
            const std::string tail(bad);
            for (std::size_t at = 0; at <= n; at += (n < 40 ? 1 : 7)) {
                const std::string s =
                    std::string(at, 'a') + tail + std::string(n - at, 'b');
                CAPTURE(n);
                CAPTURE(at);
                CHECK(duql::detail::valid_utf8(s) == ref_valid(s));
            }
        }
    for (const std::size_t n : {4096u, 100000u}) {
        std::string s(n, 'a');
        CHECK(duql::detail::valid_utf8(s));
        s[n - 1] = '\x80';
        CHECK_FALSE(duql::detail::valid_utf8(s));
        s[n / 2] = '\xC3';
        s[n / 2 + 1] = '\xA9';
        s[n - 1] = 'a';
        CHECK(duql::detail::valid_utf8(s));
    }
    for (unsigned a = 0; a < 256; ++a) {
        check_utf8(std::string(1, static_cast<char>(a)));
        for (unsigned b = 0; b < 256; ++b)
            check_utf8(std::string{static_cast<char>(a), static_cast<char>(b)});
    }
    for (unsigned a = 0xE0; a < 0xF0; ++a)
        for (unsigned b = 0; b < 256; ++b)
            for (unsigned c = 0; c < 256; ++c)
                check_utf8(std::string{static_cast<char>(a),
                                       static_cast<char>(b),
                                       static_cast<char>(c)});
    for (const char* s :
         {"\xF0\x9F\x98\x80", "\xF0\x90\x80\x80", "\xF0\x8F\xBF\xBF",
          "\xF4\x8F\xBF\xBF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80",
          "\xF0\x9F\x98", "\xF0\x9F\x98\x80\x80", "\xF0\x9F\x98\x20", "\xFF",
          "\xF8\x88\x80\x80\x80"})
        check_utf8(s);
}
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
namespace {

std::string encode(unsigned cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 63));
        s += static_cast<char>(0x80 | (cp & 63));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 63));
        s += static_cast<char>(0x80 | ((cp >> 6) & 63));
        s += static_cast<char>(0x80 | (cp & 63));
    }
    return s;
}

}  // namespace

TEST_CASE("Vectorscan and PCRE2 agree on every code point") {
    std::vector<std::string> subjects;
    auto range = [&](unsigned lo, unsigned hi) {
        for (unsigned c = lo; c < hi; ++c) subjects.push_back(encode(c));
    };
    range(0x20, 0x7F);
    range(0x80, 0x600);
    range(0x1C80, 0x1CC0);
    range(0x1E00, 0x2200);
    range(0xA640, 0xA800);
    range(0xFB00, 0xFC00);
    range(0x10400, 0x10450);
    range(0x1E900, 0x1E960);
    const unsigned pair[] = {0x41,   0x301,   0x61,   0xE9,    0x3B1,
                             0x4E2D, 0x1F600, 0x200D, 0x1F3FB, 0xD55C,
                             0x1100, 0x11A8,  0x0D,   0x0A,    0x2028,
                             0x212A, 0x17F,   0x131,  0x130,   0x20};
    for (unsigned a : pair)
        for (unsigned b : pair) subjects.push_back(encode(a) + encode(b));

    std::vector<std::string> patterns;
    for (char c = 'a'; c <= 'z'; ++c) {
        patterns.push_back(std::string("(?i)") + c);
        patterns.push_back(std::string("(?i)[") + c + "]");
    }
    for (unsigned c = 0x80; c < 0x600; c += 3)
        patterns.push_back("(?i)" + encode(c));
    for (unsigned c = 0x1E00; c < 0x1F00; c += 5)
        patterns.push_back("(?i)" + encode(c));
    for (const char* p : {"(?i)[a-z]",
                          "(?i)[^a]",
                          "(?i)[^\xC3\xA9]",
                          "(?i)\\w",
                          "(?i)\\W",
                          "(?i)[[:alpha:]]",
                          "(?i)[[:upper:]]",
                          "(?i)[[:lower:]]",
                          "\\W",
                          "\\D",
                          "\\S",
                          "\\B",
                          "\\b",
                          "[^a]",
                          "[^\xC3\xA9]",
                          "[[:^alpha:]]",
                          "[[:alpha:]]",
                          "[\\w]",
                          "\\p{L}",
                          "\\p{Lu}",
                          "\\p{Ll}",
                          "\\P{L}",
                          "\\p{N}",
                          "\\p{Greek}",
                          "\\p{Han}",
                          "\\p{Cyrillic}",
                          "\\p{Latin}",
                          "\\p{Z}",
                          "\\p{P}",
                          "\\p{S}",
                          "\\p{M}",
                          "\\pL",
                          "(?i)\\p{Lu}",
                          "(?i)\\p{Ll}",
                          "\\s",
                          "\\h",
                          "\\H",
                          "\\v",
                          "\\V",
                          "\\R",
                          "\\X",
                          "^\\X$",
                          "\\d",
                          "(?i)[\xC3\xA0-\xC3\xBF]",
                          "[\xC3\xA0-\xC3\xBF]",
                          "(?i)[\xCE\xB1-\xCF\x89]",
                          "(?i)\\x{e9}",
                          "\\x{e9}",
                          "(?i)\\x{3b1}",
                          "(?i)\\o{351}",
                          "[^\\x{e9}]",
                          "a.",
                          "^.{2}$",
                          "(?s).",
                          "(?m)^.$",
                          ".",
                          "(?i)s+",
                          "(?i)k+",
                          "(?i)ss",
                          "(?i)i",
                          "(?i)[i]",
                          "(?i)\xC4\xB1"})
        patterns.push_back(p);

    std::size_t compared = 0;
    for (const auto& pat : patterns)
        for (const bool icase : {false, true})
            for (const bool whole : {false, true}) {
                auto p = duql::compile_regex(pat, icase, whole);
                if (!p) continue;
                for (const auto& s : subjects) {
                    if (duql::detail::match_regex(**p, s) !=
                        duql::detail::match_regex_pcre2(**p, s)) {
                        CAPTURE(pat);
                        CAPTURE(icase);
                        CAPTURE(whole);
                        CAPTURE(s);
                        CHECK(false);
                        return;
                    }
                    ++compared;
                }
            }
    CHECK(compared > 1000000);
}
#endif
