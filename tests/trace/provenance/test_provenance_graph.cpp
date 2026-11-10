// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/provenance/provenance_graph.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>

using namespace dftracer::utils::trace::provenance;

TEST_CASE("a constant query that parses gives its query") {
    const auto query = constant_query("cat == \"POSIX\"");
    CHECK(!query.source().empty());
}

TEST_CASE("a constant query that does not parse ends the process") {
    auto result = fresh_process::run("constant query fatal scenario");
    CHECK(result.aborted_with("provenance: a constant query does not parse"));
}

TEST_CASE("constant query fatal scenario" * doctest::skip()) {
    fresh_process::announce();
    constant_query("cat ==");
}
