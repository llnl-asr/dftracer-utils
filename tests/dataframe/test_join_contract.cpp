// SPDX-License-Identifier: MIT
// The join build side ends the process when it is used against its contract.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/join.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <task_result.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

DataFrame keyed_frame() {
    const std::int64_t keys[] = {1, 2, 3};
    const std::int64_t values[] = {10, 20, 30};
    DataFrame df;
    df.names = {"k", "v"};
    df.columns.push_back(Series::flat_i64(keys, 3));
    df.columns.push_back(Series::flat_i64(values, 3));
    return df;
}

HashJoin make_join(JoinHow how) {
    return dftu_utils_test::ok(
        HashJoin::make(keyed_frame(), {"k"}, {"k"}, how, "_r"));
}

}  // namespace

TEST_SUITE("join contract") {
    TEST_CASE("a lookup probed block by block ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        HashJoin join = make_join(JoinHow::Lookup);
        dstd::Vector<std::uint8_t> matched(3, 0);
        (void)join.probe(keyed_frame(), &matched, 0);
    }

    TEST_CASE("a lookup probed block by block is fatal") {
        auto result = fresh_process::run(
            "a lookup probed block by block ends the process");
        CHECK(result.aborted_with(
            "join: a lookup join cannot be probed block by block"));
    }

    TEST_CASE("lookup conflicts for the wrong row count end the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        HashJoin join = make_join(JoinHow::Lookup);
        join.set_lookup_conflicts(dstd::Vector<std::int64_t>{-1});
    }

    TEST_CASE("lookup conflicts for the wrong row count are fatal") {
        auto result = fresh_process::run(
            "lookup conflicts for the wrong row count end the process");
        CHECK(result.aborted_with(
            "join: lookup conflicts do not match the right rows"));
    }

    TEST_CASE("left templates of the wrong count end the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        HashJoin join = make_join(JoinHow::Right);
        (void)join.flush({"k", "v"}, dstd::Vector<Series>{});
    }

    TEST_CASE("left templates of the wrong count are fatal") {
        auto result = fresh_process::run(
            "left templates of the wrong count end the process");
        CHECK(result.aborted_with(
            "join: left templates do not match the left column count"));
    }
}
