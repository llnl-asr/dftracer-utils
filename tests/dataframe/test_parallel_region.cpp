// SPDX-License-Identifier: MIT
// A fork-join started in one library is a parallel region for every other
// library, so a nested fork-join through the dataframe seam runs serial.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <atomic>
#include <cstdint>

using namespace dftracer::utils;

TEST_CASE("a dataframe fork-join inside a runtime fork-join runs serial") {
    dataframe::install_runtime_parallel_backend();
    std::atomic<int> inner_calls{0};
    dftu_utils_test::ok(
        default_runtime().parallel_for(8, 1, [&](std::int64_t, std::int64_t) {
            dataframe::parallel_for(1000, 8, [&](std::int64_t, std::int64_t) {
                inner_calls.fetch_add(1);
            });
        }));
    CHECK(inner_calls.load() == 8);
    dataframe::set_parallel_backend(nullptr, nullptr);
}
