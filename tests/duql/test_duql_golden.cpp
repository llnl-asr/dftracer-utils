// SPDX-License-Identifier: MIT
// Every filter string from the tests and docs, recorded with the parser that
// duql replaced: each must parse to the same printed tree, or fail as before.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/parser.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>

using namespace dftracer::utils;

TEST_CASE("the golden filters keep their trees") {
    const auto golden = dstd::fs::read_file(DUQL_GOLDEN_PATH);
    REQUIRE(golden.has_value());
    dftu_utils_test::LineStream in(*golden);
    simdjson::dom::parser json;
    std::size_t checked = 0;
    for (dstd::String line; in.next(line);) {
        if (line.empty()) continue;
        simdjson::dom::element row;
        const simdjson::padded_string padded(line);
        REQUIRE(json.parse(padded).get(row) == simdjson::SUCCESS);
        const dstd::String q(row["q"].get_string().value());
        const bool ok = row["ok"].get_bool().value();
        const auto tree = dftracer::utils::duql::parse(q);
        CAPTURE(q);
        CHECK(tree.has_value() == ok);
        if (ok && tree)
            CHECK(dftracer::utils::duql::to_string(**tree) ==
                  dstd::String(row["tree"].get_string().value()));
        ++checked;
    }
    CHECK(checked > 300);
}
