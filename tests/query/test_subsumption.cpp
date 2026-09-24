#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/query/subsumption.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::query;

namespace {

// True if an MV filtered by `mv` can answer a query filtered by `q`.
bool subsumes(const char* mv, const char* q) {
    return query_subsumes(parse_or_throw(mv).root(), parse_or_throw(q).root());
}

}  // namespace

TEST_CASE("subsumption - identity and reflexivity") {
    CHECK(subsumes("name == \"read\"", "name == \"read\""));
    CHECK(subsumes("dur > 100", "dur > 100"));
    CHECK(subsumes("name == \"read\" or name == \"write\"",
                   "name == \"read\" or name == \"write\""));
}

TEST_CASE("subsumption - added conjunct narrows the query") {
    CHECK(subsumes("name == \"read\"", "name == \"read\" and dur > 100"));
    CHECK(
        subsumes("cat == \"io\"",
                 "cat == \"io\" and (name == \"read\" or name == \"write\")"));
    // A query missing the mv's conjunct is not a subset.
    CHECK_FALSE(subsumes("name == \"read\" and dur > 100", "name == \"read\""));
}

TEST_CASE("subsumption - numeric interval containment") {
    CHECK(subsumes("ts > 0", "ts > 5"));         // narrower lower bound
    CHECK(subsumes("dur >= 0", "dur > 100"));    // strictness within
    CHECK(subsumes("dur < 1000", "dur < 500"));  // narrower upper bound
    CHECK(subsumes("ts >= 100 and ts <= 1000", "ts >= 200 and ts <= 500"));
    CHECK_FALSE(subsumes("ts > 5", "ts > 0"));   // mv is tighter -> not super
    CHECK_FALSE(subsumes("dur > 100", "dur < 500"));  // unrelated bound
    // A field the mv bounds but the query does not: mv is narrower.
    CHECK_FALSE(subsumes("dur > 100", "name == \"read\""));
}

TEST_CASE("subsumption - inclusive vs exclusive edges") {
    CHECK(subsumes("dur >= 100", "dur > 100"));        // > 100 is within >= 100
    CHECK_FALSE(subsumes("dur > 100", "dur >= 100"));  // >= 100 includes 100
    CHECK(subsumes("dur <= 100", "dur < 100"));
    CHECK_FALSE(subsumes("dur < 100", "dur <= 100"));
}

TEST_CASE("subsumption - value sets and IN lists") {
    CHECK(subsumes("name in [\"read\", \"write\"]", "name == \"read\""));
    CHECK(subsumes("pid in [1, 2, 7]", "pid == 2"));
    CHECK(subsumes("pid in [1, 2, 7]", "pid in [1, 7]"));
    CHECK_FALSE(subsumes("name == \"read\"", "name in [\"read\", \"write\"]"));
    CHECK_FALSE(subsumes("pid in [1, 2]", "pid in [1, 3]"));
}

TEST_CASE("subsumption - cross bucket numeric point and range") {
    CHECK(subsumes("dur >= 50", "dur == 100"));       // point in range
    CHECK(subsumes("dur >= 50 and dur <= 200", "dur in [100, 150]"));
    CHECK_FALSE(subsumes("dur >= 50", "dur == 10"));  // point below range
    CHECK_FALSE(subsumes("dur >= 50 and dur <= 200", "dur in [100, 500]"));
}

TEST_CASE("subsumption - top-level OR decomposes on both sides") {
    // A conjunct added to the OR narrows it.
    CHECK(subsumes("name == \"read\" or name == \"write\"",
                   "(name == \"read\" or name == \"write\") and dur > 100"));
    // A single branch cannot cover the union.
    CHECK_FALSE(
        subsumes("name == \"read\"", "name == \"read\" or name == \"write\""));
    // A broader OR subsumes a narrower one (every query branch fits some mv
    // branch).
    CHECK(subsumes("name == \"read\" or name == \"write\" or name == \"open\"",
                   "name == \"read\" or name == \"write\""));
    // A branch absent from the mv is not covered.
    CHECK_FALSE(subsumes("name == \"read\" or name == \"write\"",
                         "name == \"read\" or name == \"close\""));
    // Interval branches broaden too.
    CHECK(subsumes("dur < 10 or dur > 100", "dur < 5 or dur > 200"));
}

TEST_CASE("subsumption - an OR of equalities on one field is a value set") {
    // `x == a or x == b` is `x in [a,b]`, matched either direction.
    CHECK(subsumes("name == \"read\" or name == \"write\"",
                   "name in [\"read\", \"write\"]"));
    CHECK(subsumes("name in [\"read\", \"write\"]",
                   "name == \"read\" or name == \"write\""));
    // Broader set/OR subsumes a narrower one across the two forms.
    CHECK(subsumes("name in [\"read\", \"write\", \"open\"]",
                   "name == \"read\" or name == \"write\""));
    CHECK_FALSE(subsumes("name == \"read\" or name == \"write\"",
                         "name in [\"read\", \"close\"]"));
    // A mixed-field OR stays an opaque residual - it covers its read branch
    // but not an unrelated query.
    CHECK(subsumes("name == \"read\" or pid == 1", "name == \"read\""));
    CHECK_FALSE(subsumes("name == \"read\" or pid == 1", "name == \"write\""));
}

TEST_CASE("subsumption - negation stays opaque, never decomposed") {
    CHECK(subsumes("not (name == \"posix\")", "not (name == \"posix\")"));
    // not(x>5) does NOT imply x<=5 under false-on-absent semantics; the matcher
    // must not prove this via interval reasoning.
    CHECK_FALSE(subsumes("dur <= 5", "not (dur > 5)"));
    CHECK_FALSE(subsumes("not (dur > 5)", "dur <= 5"));
}

TEST_CASE("subsumption - like/regex residuals matched verbatim") {
    CHECK(
        subsumes("name like \"%read%\"", "name like \"%read%\" and dur > 100"));
    CHECK_FALSE(subsumes("name like \"%read%\"", "name like \"%write%\""));
}

TEST_CASE("subsumption - any() leaves are not intervals on the path") {
    // A plain field and any() over it select different records.
    CHECK_FALSE(subsumes("sizes > 50", "any(sizes) > 100"));
    CHECK_FALSE(subsumes("any(sizes) > 50", "sizes > 100"));
    // Two any() bounds may hold on different elements, so they do not narrow
    // one interval.
    CHECK_FALSE(subsumes("any(sizes) > 50 and any(sizes) < 60",
                         "any(sizes) > 55 and any(sizes) < 58"));
    CHECK(subsumes(R"(any(tags) == "a")", R"(any(tags) == "a")"));
}
