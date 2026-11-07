// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/views/coverage.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::trace::views::detail;

TEST_SUITE("Coverage") {
    TEST_CASE("an unrecorded unit is never reported as covered") {
        CoverageSet cov;
        CHECK(cov.empty());
        CHECK_FALSE(cov.covers("a.pfw.gz", 0));

        cov.add("a.pfw.gz", 0);
        CHECK(cov.covers("a.pfw.gz", 0));
        // Neither a different checkpoint of the same file nor the same
        // checkpoint of a different file may inherit that coverage.
        CHECK_FALSE(cov.covers("a.pfw.gz", 1));
        CHECK_FALSE(cov.covers("b.pfw.gz", 0));
    }

    TEST_CASE("file and checkpoint cannot be confused by the key encoding") {
        CoverageSet cov;
        cov.add("a", 11);
        CHECK_FALSE(cov.covers("a1", 1));
        CHECK(cov.covers("a", 11));
    }

    TEST_CASE("a sealed scan publishes its units") {
        CoverageSet cov;
        {
            PendingCoverage p(cov);
            p.mark_complete("a.pfw.gz", 0);
            p.mark_complete("a.pfw.gz", 1);
            CHECK(p.pending() == 2);
            CHECK(cov.empty());  // nothing visible before seal
            p.seal();
            CHECK(p.sealed());
        }
        CHECK(cov.size() == 2);
        CHECK(cov.covers("a.pfw.gz", 0));
        CHECK(cov.covers("a.pfw.gz", 1));
    }

    TEST_CASE("a scan that ends early publishes nothing") {
        CoverageSet cov;

        SUBCASE("explicit abandon") {
            PendingCoverage p(cov);
            p.mark_complete("a.pfw.gz", 0);
            p.abandon();
            p.seal();  // sealing after abandon must not resurrect the units
        }
        SUBCASE("dropped without sealing, as on an early return or throw") {
            PendingCoverage p(cov);
            p.mark_complete("a.pfw.gz", 0);
        }

        CHECK(cov.empty());
        CHECK_FALSE(cov.covers("a.pfw.gz", 0));
    }

    TEST_CASE("an exception between marking and sealing publishes nothing") {
        CoverageSet cov;
        try {
            PendingCoverage p(cov);
            p.mark_complete("a.pfw.gz", 0);
            throw std::runtime_error("scan failed");
        } catch (const std::runtime_error&) {
        }
        CHECK(cov.empty());
    }

    TEST_CASE("a whole-file unit is independent of the member units") {
        CoverageSet cov;
        cov.add("a.pfw.gz", 0);
        cov.add("a.pfw.gz", 1);
        // Covering every member a scan happened to visit is not a claim about
        // the file: pruning may have skipped members the scan never saw.
        CHECK_FALSE(cov.covers_file("a.pfw.gz"));

        cov.add_file("a.pfw.gz");
        CHECK(cov.covers_file("a.pfw.gz"));
        CHECK_FALSE(cov.covers_file("b.pfw.gz"));
        // The marker must not read as a checkpoint of the same file.
        CHECK_FALSE(cov.covers("a.pfw.gz", 42));
    }

    TEST_CASE("a whole-file claim is dropped unless the scan sealed it") {
        CoverageSet cov;
        {
            PendingCoverage p(cov);
            p.mark_file_complete("a.pfw.gz");
        }
        CHECK_FALSE(cov.covers_file("a.pfw.gz"));

        {
            PendingCoverage p(cov);
            p.mark_file_complete("a.pfw.gz");
            p.seal();
        }
        CHECK(cov.covers_file("a.pfw.gz"));
    }

    TEST_CASE("absorbing a worker's set unions it and empties the source") {
        CoverageSet a, b;
        a.add("a.pfw.gz", 0);
        b.add("a.pfw.gz", 0);  // same unit from another worker
        b.add("b.pfw.gz", 0);

        a.absorb(std::move(b));
        CHECK(a.size() == 2);
        CHECK(a.covers("a.pfw.gz", 0));
        CHECK(a.covers("b.pfw.gz", 0));
        CHECK(b.empty());

        // The empty-target path moves the whole container rather than
        // inserting; it must reach the same state.
        CoverageSet fresh, c;
        c.add("c.pfw.gz", 3);
        fresh.absorb(std::move(c));
        CHECK(fresh.size() == 1);
        CHECK(fresh.covers("c.pfw.gz", 3));
        CHECK(c.empty());
    }

    TEST_CASE("sealing accumulates rather than replacing") {
        CoverageSet cov;
        {
            PendingCoverage p(cov);
            p.mark_complete("a.pfw.gz", 0);
            p.seal();
        }
        {
            PendingCoverage p(cov);
            p.mark_complete("b.pfw.gz", 0);
            p.seal();
        }
        CHECK(cov.size() == 2);
        CHECK(cov.covers("a.pfw.gz", 0));
        CHECK(cov.covers("b.pfw.gz", 0));
    }
}
