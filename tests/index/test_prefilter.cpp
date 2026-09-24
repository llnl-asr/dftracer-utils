#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/plan/prefilter.h>
#include <dftracer/utils/query/query.h>
#include <doctest/doctest.h>

#include <string>
#include <vector>

using dftracer::utils::index::plan::Prefilter;
using Clauses = std::vector<std::vector<std::string>>;

namespace {

Prefilter of(const char* q) {
    return Prefilter(dftracer::utils::query::parse_or_throw(q));
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
              R"(name like "%rea%")", R"(name not in ["read"])",
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
            (void)selective.may_match(R"({"name":"write"})");
        CHECK_FALSE(selective.may_match(R"({"name":"write"})"));
    }
}
