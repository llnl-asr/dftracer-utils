// Every pattern of the golden filters, matched by the new engine and by the
// std::regex translation it replaced, on ASCII strings without newlines: the
// results agree. Newlines and multi-byte characters are the listed changes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
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
