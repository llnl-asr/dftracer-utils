#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/plan/prefilter.h>
#include <doctest/doctest.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using dftracer::utils::index::plan::Prefilter;
using Clauses = std::vector<std::vector<std::string>>;

namespace {

Prefilter of(const char* q) {
    return Prefilter(dftracer::utils::duql::parse_or_throw(q));
}

void agree(const Prefilter& p, std::initializer_list<const char*> lines,
           bool expect_any_true = true) {
    bool any_true = false;
    for (const char* l : lines) {
        CAPTURE(l);
        const bool a = p.may_match(l);
        CHECK(a == p.may_match_per_needle(l));
        any_true = any_true || a;
    }
    CHECK(any_true == expect_any_true);
}

}  // namespace

TEST_SUITE("Prefilter") {
    TEST_CASE("equality and in give value needles") {
        CHECK(of(R"(name == "read")").clauses() == Clauses{{"\"read\""}});
        CHECK(of("pid == 1234").clauses() == Clauses{{"1234"}});
        CHECK(of(R"(cat in ["POSIX", "STDIO"])").clauses() ==
              Clauses{{"\"POSIX\"", "\"STDIO\""}});
        CHECK(of(R"(args.io.op == "seek")").clauses() == Clauses{{"\"seek\""}});
    }

    TEST_CASE("and joins clauses, or distributes") {
        CHECK(of(R"(name == "read" and cat == "POSIX")").clauses() ==
              Clauses{{"\"read\""}, {"\"POSIX\""}});
        CHECK(of(R"(name == "read" or name == "write")").clauses() ==
              Clauses{{"\"read\"", "\"write\""}});
        CHECK(of(R"((name == "read" and cat == "POSIX") or pid == 1234)")
                  .clauses() ==
              Clauses{{"\"read\"", "1234"}, {"\"POSIX\"", "1234"}});
    }

    TEST_CASE("leaves that cannot require bytes add nothing") {
        for (const char* q :
             {R"(name != "read")", "dur != 100", R"(not (name == "read"))",
              R"(name ilike "%rea%")", R"(name !~ "cuda")",
              R"q(name ~ "(cuda|hip)")q", R"(name ~* "cuda")",
              R"(name like "a%b")", R"(name not in ["read"])",
              R"(fpath == "/tmp/x")", R"(name == "a\"b")",
              R"(name == "read" or dur > 5)", "tags.0 > 5",
              R"(not (dur > 5))"}) {
            CAPTURE(q);
            CHECK(of(q).empty());
        }
        // One side of an and still requires its needle.
        CHECK(of(R"(name == "read" and dur > 100)").clauses() ==
              Clauses{{"\"read\""}});
    }

    TEST_CASE("case-sensitive patterns require their literals") {
        CHECK(of(R"(name like "%rea%")").clauses() == Clauses{{"rea"}});
        CHECK(of(R"(name ~ "cuda.*Memcpy")").clauses() ==
              Clauses{{"Memcpy"}, {"cuda"}});
        CHECK(of(R"q(name ~ "^MPI_(Send|Recv)$")q").clauses() ==
              Clauses{{"MPI_"}});
        const auto p = of(R"(name ~ "cuda.*Memcpy")");
        CHECK(p.may_match(R"({"name":"cudaMemcpyAsync"})"));
        CHECK_FALSE(p.may_match(R"({"name":"cudaMalloc"})"));
    }

    TEST_CASE("lines pass on needles, whatever the spacing") {
        const auto p = of(R"(name == "read" and pid in [1234, 5678])");
        CHECK(p.may_match(R"({"name":"read","pid":1234})"));
        CHECK(p.may_match(R"({"name": "read", "pid": 5678})"));
        CHECK_FALSE(p.may_match(R"({"name":"write","pid":1234})"));
        CHECK_FALSE(p.may_match(R"({"name":"read","pid":99})"));
    }

    TEST_CASE("order leaves test the numbers written after their key") {
        const auto p = of("ts >= 100 and ts < 200 and dur > 5");
        CHECK_FALSE(p.empty());
        CHECK(p.may_match(R"({"ts":150,"dur":6})"));
        CHECK(p.may_match(R"({"ts" : 100 , "dur": 7.5})"));
        CHECK_FALSE(p.may_match(R"({"ts":200,"dur":6})"));
        CHECK_FALSE(p.may_match(R"({"ts":150,"dur":5})"));
        CHECK_FALSE(p.may_match(R"({"ts":150})"));
        // args.ts and a flat "args.ts" key both answer `ts`.
        CHECK(p.may_match(R"({"ts":0,"args":{"ts":150},"dur":9})"));
        CHECK(p.may_match(R"({"args.ts":150,"dur":9})"));
        // A key merely ending in the name is another field only when no dot
        // or quote precedes the name.
        CHECK_FALSE(p.may_match(R"({"xts":150,"dur":9})"));
        // Not a number: the evaluator fails the leaf too.
        CHECK_FALSE(p.may_match(R"({"ts":"150","dur":9})"));
        CHECK_FALSE(p.may_match(R"({"ts":[150],"dur":9})"));
        // One field reads one value, which must meet all of its bounds.
        CHECK_FALSE(p.may_match(R"({"ts":50,"args":{"ts":250},"dur":9})"));
        // Different paths read different values, though their keys match.
        const auto two = of("args.ts >= 5 and ts <= 3");
        CHECK(two.may_match(R"({"ts":1,"args":{"ts":9}})"));
    }

    TEST_CASE("numeric equality is a range with equal ends") {
        const auto zero = of("ts == 0");
        CHECK(zero.may_match(R"({"ts":0,"dur":7})"));
        CHECK(zero.may_match(R"({"ts": 0.0})"));
        CHECK_FALSE(zero.may_match(R"({"ts":1788011425034752})"));
        CHECK_FALSE(zero.may_match(R"({"ts":"0"})"));
        CHECK(of("dur == 1.5").may_match(R"({"dur":1.5})"));
        CHECK_FALSE(of("dur == 1.5").may_match(R"({"dur":2})"));
    }

    TEST_CASE("numbers the prefilter cannot read exactly keep the line") {
        const auto p = of("ts < 9007199254740993");
        CHECK(p.may_match(R"({"ts":9.1e15})"));
        CHECK(p.may_match(R"({"ts":99999999999999999999})"));
        CHECK_FALSE(of("ts < 10").may_match(R"({"ts":1e2})"));
    }

    TEST_CASE("a gate stops checking needles that do not select") {
        const auto p = of(R"(name == "read")");
        Prefilter::Gate gate(p);
        for (std::size_t i = 0; i < Prefilter::CHECK_WINDOW; ++i)
            CHECK(gate.may_match(R"({"name":"read"})"));
        CHECK(gate.may_match(R"({"name":"write"})"));

        Prefilter::Gate selective(p);
        for (std::size_t i = 0; i < Prefilter::CHECK_WINDOW; ++i)
            selective.may_match(R"({"name":"write"})");
        CHECK_FALSE(selective.may_match(R"({"name":"write"})"));
    }

    TEST_CASE("one scan agrees with the per-needle path") {
        const auto one = of(R"(name == "read")");
        agree(one, {R"({"name":"read"})", R"({"name":"write"})", "", "read"});
        CHECK(one.may_match(R"("read")"));
        CHECK_FALSE(one.may_match(""));

        const auto several = of(
            R"(name == "read" and cat in ["POSIX", "STDIO"] and pid == 1234)");
        agree(several, {R"({"name":"read","cat":"POSIX","pid":1234})",
                        R"({"name":"read","cat":"STDIO","pid":1234})",
                        R"({"name":"read","cat":"MPI","pid":1234})",
                        R"({"name":"read","cat":"POSIX","pid":12})", ""});
        CHECK(several.may_match(R"({"name":"read","cat":"STDIO","pid":1234})"));
        CHECK_FALSE(
            several.may_match(R"({"name":"read","cat":"MPI","pid":1234})"));

        std::string many = "pid in [";
        std::string last;
        for (int i = 100; i < 200; ++i) {
            if (i > 100) many += ",";
            many += std::to_string(i);
            last = std::to_string(i);
        }
        const auto big = of((many + "]").c_str());
        REQUIRE(big.clauses().size() == 1);
        REQUIRE(big.clauses()[0].size() == 100);
        CHECK(big.may_match("x " + last));
        CHECK(big.may_match("100"));
        CHECK_FALSE(big.may_match("x 99 y 200"));
        agree(big, {"199", "100 x", "99 200", ""});
    }

    TEST_CASE("clauses below and above HS_MIN_ALTERNATIVES agree") {
        constexpr std::size_t MIN = Prefilter::HS_MIN_ALTERNATIVES;
        for (const std::size_t k : {std::size_t{1}, MIN - 1, MIN, MIN + 1,
                                    2 * MIN, std::size_t{16}}) {
            std::string q = "name in [";
            for (std::size_t i = 0; i < k; ++i)
                q += (i ? ", \"n" : "\"n") + std::to_string(i) + "\"";
            const auto p = of((q + "]").c_str());
            REQUIRE(p.clauses().size() == 1);
            REQUIRE(p.clauses()[0].size() == k);
            const std::string last = "\"n" + std::to_string(k - 1) + "\"";
            CHECK(p.may_match("x " + last + " y"));
            CHECK_FALSE(p.may_match(R"({"name":"other"})"));
            agree(p, {"\"n0\"", last.c_str(), R"("n)", "", R"({"name":"zz"})"});
        }
    }

    TEST_CASE("overlapping needles and line ends") {
        const auto p = of(R"(name like "%abcde%" and cat like "%cde%")");
        REQUIRE(p.clauses().size() == 2);
        agree(p, {"abcde", "abcde cde", "cde abcde", "abcd cde", "cde",
                  "xabcde", "abcdecde", ""});
        CHECK(p.may_match("abcde"));
        CHECK_FALSE(p.may_match("abcd cde"));
        CHECK_FALSE(p.may_match("cde"));
        CHECK(p.may_match("cde abcde"));
    }

    TEST_CASE("needles at the start and end of the line, or across clauses") {
        const auto p =
            of(R"((name == "read" or pid == 1234) and cat == "POSIX")");
        CHECK(p.may_match(R"("read" "POSIX")"));
        CHECK(p.may_match(R"("POSIX" "read")"));
        CHECK(p.may_match(R"(1234 "POSIX")"));
        CHECK_FALSE(p.may_match(R"("read" "STDIO")"));
        CHECK_FALSE(p.may_match(R"("POSIX" 123)"));
        CHECK_FALSE(p.may_match(""));
        agree(p, {R"("read" "POSIX")", R"("POSIX" 1234)", "1234", "",
                  "1234POSIX"});
    }

    TEST_CASE("a needle shared by clauses and ranges still apply") {
        const auto p = of(R"(name == "read" or pid == 1234)");
        agree(p, {R"("read")", "1234", "123", ""});
        const auto r = of(R"(name == "read" and ts > 5)");
        CHECK(r.may_match(R"({"name":"read","ts":6})"));
        CHECK_FALSE(r.may_match(R"({"name":"read","ts":5})"));
        CHECK_FALSE(r.may_match(R"({"name":"write","ts":6})"));
        CHECK_FALSE(r.may_match(""));
    }
}
