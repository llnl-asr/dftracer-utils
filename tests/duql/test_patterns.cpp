#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/duql/substr_simd.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace dftracer::utils::duql;
using dftracer::utils::json::JsonValue;

namespace {

Truth truth(const char* query, const char* record) {
    auto ast = parse(query);
    REQUIRE_MESSAGE(ast.has_value(), (ast ? "" : ast.error().message));
    simdjson::dom::parser p;
    simdjson::dom::element el;
    REQUIRE(p.parse(record, std::strlen(record)).get(el) == simdjson::SUCCESS);
    return evaluate_truth(**ast, JsonValue(el));
}

std::string error_of(const char* query) {
    auto ast = parse(query);
    REQUIRE_FALSE(ast.has_value());
    return ast.error().message;
}

bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// Reference SQL LIKE: `%` any run, `_` one UTF-8 character, no escape.
bool like_ref(std::string_view p, std::string_view s) {
    if (p.empty()) return s.empty();
    if (p[0] == '%')
        for (std::size_t i = 0; i <= s.size(); ++i) {
            if (i > 0 && i < s.size() &&
                (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
                continue;
            if (like_ref(p.substr(1), s.substr(i))) return true;
        }
    if (p[0] == '%') return false;
    if (s.empty()) return false;
    if (p[0] == '_') {
        std::size_t n = 1;
        while (n < s.size() &&
               (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
            ++n;
        return like_ref(p.substr(1), s.substr(n));
    }
    return p[0] == s[0] && like_ref(p.substr(1), s.substr(1));
}

}  // namespace

TEST_SUITE("duql patterns") {
    TEST_CASE("percent spans newlines") {
        CHECK(truth(R"(m like "a%b")", R"({"m":"a\nb"})") == Truth::YES);
        CHECK(truth(R"(m like "a%")", R"({"m":"a\n\n"})") == Truth::YES);
    }

    TEST_CASE("underscore is one UTF-8 character") {
        CHECK(truth(R"(m like "h_")", R"({"m":"hé"})") == Truth::YES);
        CHECK(truth(R"(m like "h__")", R"({"m":"hé"})") == Truth::NO);
        CHECK(truth(R"(m like "%_")", R"({"m":"é"})") == Truth::YES);
    }

    TEST_CASE("escape") {
        CHECK(truth(R"(m like "50!%" escape "!")", R"({"m":"50%"})") ==
              Truth::YES);
        CHECK(truth(R"(m like "50!%" escape "!")", R"({"m":"50x"})") ==
              Truth::NO);
        CHECK(truth(R"(m like "a!_b" escape "!")", R"({"m":"a_b"})") ==
              Truth::YES);
        CHECK(truth(R"(m like "a!!" escape "!")", R"({"m":"a!"})") ==
              Truth::YES);
        CHECK(has(error_of(R"(m like "a!x" escape "!")"), "escape"));
        CHECK(has(error_of(R"(m like "a" escape "!!")"), "one character"));
        CHECK(to_string(**parse(R"(m like "50!%" escape "!")")) ==
              R"(m like "50!%" escape "!")");
    }

    TEST_CASE("no escape character by default") {
        CHECK(truth(R"(m like "a\b")", R"({"m":"a\\b"})") == Truth::YES);
        CHECK(truth(R"(m like "a\%")", R"({"m":"a\\xyz"})") == Truth::YES);
    }

    TEST_CASE("ilike folds ASCII; ~* folds Unicode") {
        CHECK(truth(R"(m ilike "%READ%")", R"({"m":"pread64"})") == Truth::YES);
        CHECK(truth(R"(m ilike "%É%")", R"({"m":"é"})") == Truth::NO);
        CHECK(truth(R"(m ~* "É")", R"({"m":"é"})") == Truth::YES);
        CHECK(truth(R"("READ" in m)", R"({"m":"pread"})") == Truth::YES);
    }

    TEST_CASE("the regex dialect") {
        for (const char* q :
             {R"(n ~ "(a)\1")", R"q(n ~ "a(?=b)")q", R"q(n ~ "a(?!b)")q",
              R"(n ~ "(?<=a)b")", R"(n ~ "(?<!a)b")", R"q(n ~ "(?>ab)")q",
              R"(n ~ "a*+")", R"(n ~ "a(?R)?")", R"(n ~ "(*UTF)a")",
              R"(n ~ "a\C")", R"(n ~ "(?x)a b")", R"(n ~ "\k<x>")",
              R"q(n ~ "a(?C1)")q"}) {
            CAPTURE(std::string(q));
            CHECK(has(error_of(q), "dialect"));
        }
        for (const char* q :
             {R"(n ~ "^a+b*?c{2,3}$")", R"(n ~ "(?i)abc")",
              R"q(n ~ "(?:a|b)")q", R"q(n ~ "(?P<x>a)")q", R"q(n ~ "(?<x>a)")q",
              R"(n ~ "[[:alpha:]]+")", R"(n ~ "\d+\.\d*")", R"(n ~ "\p{L}")",
              R"q(n ~ "(?s:a.b)")q"}) {
            CAPTURE(std::string(q));
            CHECK(parse(q).has_value());
        }
        CHECK(has(error_of(R"(n ~ "a(")"), "pattern"));
    }

    TEST_CASE("regex searches; dot is one character") {
        CHECK(truth(R"(name ~ "Send")", R"({"name":"MPI_Send"})") ==
              Truth::YES);
        CHECK(truth(R"(name ~ "^Send")", R"({"name":"MPI_Send"})") ==
              Truth::NO);
        CHECK(truth(R"(m ~ "^h.$")", R"({"m":"hé"})") == Truth::YES);
        CHECK(truth(R"(name !~ "Send")", R"({"name":"MPI_Recv"})") ==
              Truth::YES);
    }

    TEST_CASE("a catastrophic pattern is bounded and stable") {
        const std::string record =
            R"({"s":")" + std::string(10000, 'a') + R"(!"})";
        auto ast = parse(R"(s ~ "^(a|aa)+$")");
        REQUIRE(ast);
        simdjson::dom::parser p;
        simdjson::dom::element el;
        REQUIRE(p.parse(record).get(el) == simdjson::SUCCESS);
        const Truth first = evaluate_truth(**ast, JsonValue(el));
        CHECK(first != Truth::YES);
        for (int i = 0; i < 100; ++i)
            CHECK(evaluate_truth(**ast, JsonValue(el)) == first);

        namespace df = dftracer::utils::dataframe;
        const std::string value = std::string(10000, 'a') + "!";
        const df::Series col =
            df::Series::strings(std::vector<std::string_view>{value, "aa"});
        const df::Series m = col.str_search("^(a|aa)+$");
        REQUIRE(m.handle());
        const bool null0 = m.is_null(0);
        for (int i = 0; i < 20; ++i) {
            const df::Series again = col.str_search("^(a|aa)+$");
            CHECK(again.is_null(0) == null0);
            CHECK_FALSE(again.is_null(1));
        }
    }

    TEST_CASE("extract") {
        const char* r = R"({"p":"/data/run_17/out.h5"})";
        CHECK(truth(R"q(extract(p, "run_([0-9]+)", 1) == "17")q", r) ==
              Truth::YES);
        CHECK(truth(R"(extract(p, "run_[0-9]+") == "run_17")", r) ==
              Truth::YES);
        CHECK(truth(R"q(extract(p, "job_([0-9]+)", 1) == "17")q", r) ==
              Truth::UNKNOWN);
        CHECK(truth(R"q(extract(p, "run_([0-9]+)", 1) == "17")q",
                    R"({"p":5})") == Truth::UNKNOWN);
        CHECK(has(error_of(R"q(extract(p, "run_([0-9]+)", 2) == "x")q"),
                  "group"));
        CHECK(has(error_of(R"(extract(p, "(a)\1") == "x")"), "dialect"));
    }

    TEST_CASE("like kinds agree with a reference matcher") {
        std::mt19937 rng(20260928);
        const std::string alphabet[] = {"a", "b", "c", "\xc3\xa9", "\n"};
        const char pattern_chars[] = {'a', 'b', 'c', '%', '_'};
        for (int round = 0; round < 3000; ++round) {
            std::string pat;
            for (int k = static_cast<int>(rng() % 6); k > 0; --k)
                pat += pattern_chars[rng() % 5];
            std::string s;
            for (int k = rng() % 8; k > 0; --k) s += alphabet[rng() % 5];
            const auto p = compile_like(pat, false, std::nullopt);
            REQUIRE(p);
            CAPTURE(pat);
            CAPTURE(s);
            CHECK((match(**p, s) == MatchResult::YES) == like_ref(pat, s));
        }
    }

    TEST_CASE("the case-insensitive SIMD search agrees with a scalar one") {
        std::mt19937 rng(7);
        for (int round = 0; round < 2000; ++round) {
            std::string hay;
            for (int k = static_cast<int>(rng() % 200); k > 0; --k)
                hay += "aAbB!zZ"[rng() % 7];
            std::string needle;
            for (int k = 1 + rng() % 4; k > 0; --k) needle += "abz!"[rng() % 4];
            std::string lower = hay;
            for (char& c : lower)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c | 0x20);
            const auto want = static_cast<std::int64_t>(lower.find(needle));
            const auto got = detail::substr_find_icase(
                hay.data(), static_cast<std::int64_t>(hay.size()),
                needle.data(), static_cast<std::int64_t>(needle.size()));
            CAPTURE(hay);
            CAPTURE(needle);
            CHECK(got == (want == static_cast<std::int64_t>(std::string::npos)
                              ? -1
                              : want));
        }
    }

    TEST_CASE("required literals") {
        auto lits = [](const char* re) {
            auto p = compile_regex(re, false);
            REQUIRE(p);
            return required_literals(**p);
        };
        CHECK(lits("cuda.*Memcpy") ==
              std::vector<std::string>{"Memcpy", "cuda"});
        CHECK(lits("^MPI_(Send|Recv)$") == std::vector<std::string>{"MPI_"});
        CHECK(lits("(abc)?xy") == std::vector<std::string>{"xy"});
        CHECK(lits("(abc)+") == std::vector<std::string>{"abc"});
        CHECK(lits("a|bcd").empty());
        CHECK(lits("(?i)abc").empty());
        CHECK(lits("ab\\.c") == std::vector<std::string>{"ab.c"});
    }

    TEST_CASE("DataFrame str_like escapes with a backslash") {
        namespace df = dftracer::utils::dataframe;
        const df::Series col = df::Series::strings(
            std::vector<std::string_view>{"50%", "50x", "h\xc3\xa9", "a\nb"});
        const df::Series pct = col.str_like("50\\%");
        CHECK(pct.handle());
        CHECK(pct.all() == false);
        const df::Series one = col.str_like("h_");
        const df::Series nl = col.str_like("a%b");
        REQUIRE(one.handle());
        REQUIRE(nl.handle());
        CHECK(one.any());
        CHECK(nl.any());
        CHECK_FALSE(col.str_like("a\\b").handle());
    }
}
