#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test_view_common.h"

using dftracer::utils::DFTUtilsException;
using dftracer::utils::dataframe::field::F;

TEST_SUITE("View unified F filter") {
    TEST_CASE("F predicate pushes down like the query DSL") {
        const auto& s = shared_trace();
        StringSink sink;
        auto stats = View::from_file(s.gz, s.idx)
                         .metadata(false)
                         .filter(F("cat") == "POSIX")
                         .export_json(sink)
                         .get();

        auto lines = sink.lines();
        CHECK(lines.size() == 30);
        CHECK(count_containing(lines, "POSIX") == 30);
        CHECK(count_containing(lines, "STDIO") == 0);
        CHECK(stats.events_matched == 30);
    }

    TEST_CASE("F numeric comparison filters events") {
        const auto& s = shared_trace();
        StringSink sink;
        // POSIX dur is 10..39, STDIO dur is 20..39; dur >= 30 keeps a subset.
        View::from_file(s.gz, s.idx)
            .metadata(false)
            .filter(F("dur") >= 30)
            .export_json(sink)
            .get();
        CHECK(sink.lines().size() > 0);
        CHECK(sink.lines().size() < 50);
    }

    TEST_CASE("a negated predicate keeps the events its positive form drops") {
        // Regression: the chunk pruner answered a NotNode with the set
        // difference of its operand's MAY-match chunks, but a chunk holding
        // events on both sides of `dur >= 30` is a may-match for the operand
        // AND holds matches for the negation. The difference dropped every
        // such chunk, so this exported nothing at all.
        const auto& s = shared_trace();

        StringSink pos;
        View::from_file(s.gz, s.idx)
            .metadata(false)
            .filter(F("dur") >= 30)
            .export_json(pos)
            .get();

        StringSink neg;
        View::from_file(s.gz, s.idx)
            .metadata(false)
            .filter(!(F("dur") >= 30))
            .export_json(neg)
            .get();

        StringSink all;
        View::from_file(s.gz, s.idx).metadata(false).export_json(all).get();

        CHECK(pos.lines().size() > 0);
        CHECK(neg.lines().size() > 0);
        // The two halves partition the trace exactly.
        CHECK(pos.lines().size() + neg.lines().size() == all.lines().size());
    }

    TEST_CASE("non-pushable F predicate raises at filter") {
        const auto& s = shared_trace();
        View base = View::from_file(s.gz, s.idx).metadata(false);
        CHECK_THROWS_AS(base.filter((F("dur") + F("ts")) > 3),
                        DFTUtilsException);
    }
}
