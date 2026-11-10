// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/indexer.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

TEST_SUITE("View unified F filter") {
    TEST_CASE("F predicate pushes down like the query DSL") {
        const auto& s = shared_trace();
        StringSink sink;
        auto stats = dftu_utils_test::ok(
            View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                .filter(dataframe::field::F("cat") == "POSIX")
                .sink_json(sink)
                .get());

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
        dftu_utils_test::ok(
            View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                .filter(dataframe::field::F("dur") >= 30)
                .sink_json(sink)
                .get());
        CHECK(sink.lines().size() > 0);
        CHECK(sink.lines().size() < 50);
    }

    TEST_CASE("args keys named like fixed fields do not replace their zones") {
        // Events carry args.pid, args.tid and args.dur unlike their top-level
        // fields, and aggregated records hold only args.dur, which `dur`
        // reads for them. Pruning must answer as a full scan does.
        TestEnvironment env(10);
        const dstd::String plain = env.dir() + "/shadow.pfw";
        {
            dftu_utils_test::FileOut out(plain);
            for (int i = 0; i < 4000; ++i)
                out << R"({"name":"read","cat":"POSIX","pid":1,"tid":7,"ts":)"
                    << 1000 + i * 10 << R"(,"dur":)" << 1000 + i
                    << R"(,"ph":"X","args":{"pid":)" << 900 + i % 3
                    << R"(,"tid":5000,"dur":)" << i % 100 << "}}\n";
            for (int i = 0; i < 400; ++i)
                out << R"({"name":"mmap","cat":"POSIX","pid":1,"tid":7,"ts":)"
                    << 1000 + i * 100 << R"(,"ph":3,"args":{"dur":)" << i % 100
                    << R"(,"dft_cnt":2}})" << "\n";
        }
        const dstd::String gz = plain + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                                   16 * 1024));
        dstd::fs::remove(plain).discard();
        dftu_utils_test::ok(
            dftu_utils_test::ok(dftracer::utils::index::Indexer::open(
                                    {dstd::String(dstd::StringView(gz))}))
                .build());
        const dstd::String idx = dstd::String(determine_index_path(gz, ""));
        auto count = [&](Phase ph, const char* q) {
            return run(View::from_file(dstd::String(gz), dstd::String(idx))
                           .phase(ph)
                           .duql(q)
                           .collect())
                .num_rows();
        };
        CHECK(count(Phase::Events, "dur >= 4500") == 500);
        CHECK(count(Phase::Events, "tid == 7") == 4000);
        CHECK(count(Phase::Events, "pid == 1") == 4000);
        CHECK(count(Phase::Any, "dur <= 49") == 200);
    }

    TEST_CASE("a negated predicate keeps the events its positive form drops") {
        // Regression: the chunk pruner answered a NotNode with the set
        // difference of its operand's MAY-match chunks, but a chunk holding
        // events on both sides of `dur >= 30` is a may-match for the operand
        // AND holds matches for the negation. The difference dropped every
        // such chunk, so this exported nothing at all.
        const auto& s = shared_trace();

        StringSink pos;
        dftu_utils_test::ok(
            View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                .filter(dataframe::field::F("dur") >= 30)
                .sink_json(pos)
                .get());

        StringSink neg;
        dftu_utils_test::ok(
            View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                .filter(!(dataframe::field::F("dur") >= 30))
                .sink_json(neg)
                .get());

        StringSink all;
        dftu_utils_test::ok(
            View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                .sink_json(all)
                .get());

        CHECK(pos.lines().size() > 0);
        CHECK(neg.lines().size() > 0);
        // The two halves partition the trace exactly.
        CHECK(pos.lines().size() + neg.lines().size() == all.lines().size());
    }

    TEST_CASE("non-pushable F predicate raises at filter") {
        const auto& s = shared_trace();
        View base = View::from_file(dstd::String(s.gz), dstd::String(s.idx));
        CHECK_THROWS_AS(base.filter((dataframe::field::F("dur") +
                                     dataframe::field::F("ts")) > 3),
                        dftracer::utils::common::DFTUtilsException);
    }
}
