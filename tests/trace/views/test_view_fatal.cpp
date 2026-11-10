// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_source.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <task_result.h>
#include <trace/views/test_view_common.h>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;

TEST_SUITE("View misuse") {
    TEST_CASE("reading a session result before execute ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        TraceSession session;
        Deferred<dataframe::DataFrame> pending =
            session.collect(dataframe::DataFrame{}.lazy());
        (void)pending.get();
    }

    TEST_CASE("reading an empty session result ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        Deferred<dataframe::DataFrame> empty;
        (void)empty.get();
    }

    TEST_CASE("registering on a session after execute ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        TraceSession session;
        Deferred<dataframe::DataFrame> first =
            session.collect(dataframe::DataFrame{}.lazy());
        test_view_common::run(session.execute());
        (void)session.collect(dataframe::DataFrame{}.lazy());
    }

    TEST_CASE("a terminal frame without its column ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        (void)trace::views::detail::stats_of(dataframe::DataFrame{});
    }

    TEST_CASE("session misuse is fatal") {
        const char* const message =
            "ViewSession result read before execute() resolved it";
        CHECK(fresh_process::run(
                  "reading a session result before execute ends the process")
                  .aborted_with(message));
        CHECK(fresh_process::run(
                  "reading an empty session result ends the process")
                  .aborted_with(message));
        CHECK(fresh_process::run(
                  "registering on a session after execute ends the process")
                  .aborted_with("TraceSession::collect after execute()"));
    }

    TEST_CASE("a terminal frame without its column is fatal") {
        CHECK(fresh_process::run(
                  "a terminal frame without its column ends the process")
                  .aborted_with("view terminal: the result frame has no row"));
    }
}
