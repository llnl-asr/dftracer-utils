// Every filter string from the tests and docs, recorded with the parser that
// duql replaced: each must parse to the same printed tree, or fail as before.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/duql/parser.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <fstream>
#include <string>

TEST_CASE("the golden filters keep their trees") {
    std::ifstream in(DUQL_GOLDEN_PATH);
    REQUIRE(in);
    simdjson::dom::parser json;
    std::size_t checked = 0;
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        simdjson::dom::element row;
        const simdjson::padded_string padded(line);
        REQUIRE(json.parse(padded).get(row) == simdjson::SUCCESS);
        const std::string q(row["q"].get_string().value());
        const bool ok = row["ok"].get_bool().value();
        const auto tree = dftracer::utils::duql::parse(q);
        CAPTURE(q);
        CHECK(tree.has_value() == ok);
        if (ok && tree)
            CHECK(dftracer::utils::duql::to_string(**tree) ==
                  std::string(row["tree"].get_string().value()));
        ++checked;
    }
    CHECK(checked > 300);
}
