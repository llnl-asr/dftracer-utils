#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
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

TEST_CASE("a catastrophic pattern retries on Vectorscan at any length") {
    auto p = compile_regex("^(a|aa)+$", false);
    REQUIRE(p);
    const std::string no = std::string(10000, 'a') + "!";
    const std::string yes(10000, 'a');
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    CHECK(match(**p, no) == MatchResult::NO);
    CHECK(match(**p, std::string(60, 'a') + "!") == MatchResult::NO);
#else
    CHECK(match(**p, no) == MatchResult::LIMIT);
    CHECK(match(**p, std::string(60, 'a') + "!") == MatchResult::LIMIT);
#endif
    CHECK(match(**p, yes) == MatchResult::YES);
}

#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
#include <hs.h>

TEST_CASE("the engine path gives exact results") {
    namespace df = dftracer::utils::dataframe;
    auto m = [](const char* re, bool icase, std::string_view s,
                bool whole = false) {
        auto p = compile_regex(re, icase, whole);
        REQUIRE(p);
        return match(**p, s);
    };
    const std::string hard = std::string(10000, 'a') + "!";
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    CHECK(m("^(a|aa)+$", false, hard) == MatchResult::NO);
    CHECK(m("^(a|aa)+$", true, hard) == MatchResult::NO);
#endif
    CHECK(m("^(a|aa)+$", false, std::string(10000, 'a')) == MatchResult::YES);
    CHECK(m("^(a|aa)+$", true, "AAA") == MatchResult::YES);
    CHECK(m("b+", true, "aBBc") == MatchResult::YES);
    CHECK(m("b", false, "aBBc") == MatchResult::NO);
    CHECK(m("b", false, "abc", true) == MatchResult::NO);
    CHECK(m("a|b", false, "b", true) == MatchResult::YES);
    CHECK(m("a|b", false, "ab", true) == MatchResult::NO);
    CHECK(m("a*", false, "xyz") == MatchResult::YES);
    CHECK(m("", false, "") == MatchResult::YES);
    CHECK(m("a.b", false, "a\nb") == MatchResult::NO);
    CHECK(m("(?s)a.b", false, "a\nb") == MatchResult::YES);
    CHECK(m("a$", false, "a\n") == MatchResult::YES);
    CHECK(m("a$", false, "a\nb") == MatchResult::NO);
    CHECK(m("(?m)a$", false, "a\nb") == MatchResult::YES);
    CHECK(m("(?m)^b", false, "a\nb") == MatchResult::YES);
    CHECK(m("^b", false, "a\nb") == MatchResult::NO);
    CHECK(m("\\bfoo\\b", false, "a foo b") == MatchResult::YES);
    CHECK(m("\\bfoo\\b", false, "afoo") == MatchResult::NO);

    CHECK(m("caf\xc3\xa9", false, "un caf\xc3\xa9 noir") == MatchResult::YES);
    CHECK(m("^.$", false, "\xc3\xa9") == MatchResult::YES);
    CHECK(m("\xc3\xa9", true, "\xc3\x89") == MatchResult::YES);
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    CHECK(m("^(a|aa)+$", false, hard + "\xc3\xa9") == MatchResult::NO);
#else
    CHECK(m("^(a|aa)+$", false, hard + "\xc3\xa9") == MatchResult::LIMIT);
#endif
    CHECK(m("^(a|aa)+$", false, hard + "\xc0\xaf") == MatchResult::LIMIT);

    CHECK(m("^a\\Xc$", false, "abc") == MatchResult::YES);
    CHECK(m("^a\\Xc$", false, "ac") == MatchResult::NO);
    CHECK(m("a\\Rb", false, "a\r\nb") == MatchResult::YES);

    const df::Series col =
        df::Series::strings(std::vector<std::string_view>{hard, "aa"});
    const df::Series r = col.str_search("^(a|aa)+$");
    REQUIRE(r.handle());
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    CHECK_FALSE(r.is_null(0));
#endif
    CHECK_FALSE(r.is_null(1));
}

TEST_CASE("vectorscan links and scans") {
    hs_database_t* db = nullptr;
    hs_compile_error_t* err = nullptr;
    REQUIRE(hs_compile("a.c", HS_FLAG_SINGLEMATCH, HS_MODE_BLOCK, nullptr, &db,
                       &err) == HS_SUCCESS);
    hs_scratch_t* scratch = nullptr;
    REQUIRE(hs_alloc_scratch(db, &scratch) == HS_SUCCESS);
    auto count = [&](const char* s) {
        int hits = 0;
        hs_scan(
            db, s, static_cast<unsigned>(std::strlen(s)), 0, scratch,
            [](unsigned, unsigned long long, unsigned long long, unsigned,
               void* ctx) {
                ++*static_cast<int*>(ctx);
                return 0;
            },
            &hits);
        return hits;
    };
    CHECK(count("xxabcxx") == 1);
    CHECK(count("xxabxx") == 0);
    hs_free_scratch(scratch);
    hs_free_database(db);
}
#endif

namespace {

std::string replace_with(const char* re, const char* to, std::string_view s,
                         MatchResult* res = nullptr) {
    auto p = compile_regex(re, false);
    REQUIRE(p.has_value());
    auto sub = compile_substitution(**p, to);
    REQUIRE_MESSAGE(sub.has_value(), (sub ? "" : sub.error().message));
    std::string out;
    const MatchResult r = regex_replace(**p, *sub, s, out);
    if (res) *res = r;
    return out;
}

}  // namespace

TEST_CASE("capture_names lists named groups in group order") {
    auto p = compile_regex(R"re((a)(?<second>b)(c)(?<fourth>d)(?P<fifth>e))re",
                           false);
    REQUIRE(p.has_value());
    const auto names = capture_names(**p);
    REQUIRE(names.size() == 3);
    CHECK(names[0] == std::make_pair(std::size_t{2}, std::string("second")));
    CHECK(names[1] == std::make_pair(std::size_t{4}, std::string("fourth")));
    CHECK(names[2] == std::make_pair(std::size_t{5}, std::string("fifth")));
    auto none = compile_regex("(a)b", false);
    REQUIRE(none.has_value());
    CHECK(capture_names(**none).empty());
    auto lit = compile_contains("x", false);
    REQUIRE(lit.has_value());
    CHECK(capture_names(**lit).empty());
}

TEST_CASE("regex_replace substitution forms") {
    CHECK(replace_with(R"re((?<op>[a-z]+)64_(\d+))re", "${op}#$2",
                       "open64_17") == "open#17");
    CHECK(replace_with("(a)(b)", "$2$1", "ab") == "ba");
    CHECK(replace_with("(a)", "${1}0", "a") == "a0");
    CHECK(replace_with("a", "[$0]", "xax") == "x[a]x");
    CHECK(replace_with("a", "$$1", "a") == "$1");
    CHECK(replace_with("/+", "/", "/a//b///c") == "/a/b/c");
    CHECK(replace_with("a", "", "banana") == "bnn");
}

TEST_CASE("regex_replace no match keeps the string") {
    MatchResult r;
    CHECK(replace_with("z", "y", "abc", &r) == "abc");
    CHECK(r == MatchResult::NO);
    CHECK(replace_with("a", "b", "a", &r) == "b");
    CHECK(r == MatchResult::YES);
}

TEST_CASE("regex_replace empty matches advance one character") {
    CHECK(replace_with("x*", "-", "ab") == "-a-b-");
    CHECK(replace_with("x*", "-", "") == "-");
    CHECK(replace_with("x*", "-", "\xC3\xA9z") == "-\xC3\xA9-z-");
    CHECK(replace_with("b*", "-", "abba") == "-a--a-");
}

TEST_CASE("regex_replace unset group inserts nothing") {
    CHECK(replace_with("(a)|(b)", "[$1|$2]", "ab") == "[a|][|b]");
}

TEST_CASE("regex_replace invalid UTF-8 never matches the bad bytes") {
    CHECK(replace_with("a", "X",
                       "a\xFF"
                       "a") == "X\xFFX");
    CHECK(replace_with(".", "X", "\xFF") == "\xFF");
}

TEST_CASE("compile_substitution rejects bad templates") {
    auto p = compile_regex("(?<n>a)", false);
    REQUIRE(p.has_value());
    auto bad = [&](const char* to) { return compile_substitution(**p, to); };
    auto g2 = bad("x$2");
    REQUIRE(!g2.has_value());
    CHECK(g2.error().message.find("group 2") != std::string::npos);
    CHECK(g2.error().offset == 1);
    auto name = bad("${nope}");
    REQUIRE(!name.has_value());
    CHECK(name.error().message.find("nope") != std::string::npos);
    CHECK(!bad("${5}").has_value());
    CHECK(!bad("$10").has_value());
    CHECK(!bad("$").has_value());
    CHECK(!bad("a$x").has_value());
    CHECK(!bad("${").has_value());
    CHECK(!bad("${}").has_value());
    CHECK(!bad("${n").has_value());
    CHECK(bad("${n}$1$$").has_value());
    auto lit = compile_contains("x", false);
    REQUIRE(lit.has_value());
    CHECK(!compile_substitution(**lit, "y").has_value());
}
