#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <doctest/doctest.h>

#include "test_view_common.h"

namespace dataframe = dftracer::utils::dataframe;

namespace {

std::int64_t col_of(const dataframe::DataFrame& b, const std::string& name) {
    for (std::size_t i = 0; i < b.names.size(); ++i)
        if (b.names[i] == name) return static_cast<std::int64_t>(i);
    return -1;
}
std::int64_t row_of(const dataframe::DataFrame& b, const std::string& key) {
    for (std::int64_t i = 0; i < b.num_rows(); ++i)
        if (b.columns[0].string_at(i) == key) return i;
    return -1;
}
double num(const dataframe::DataFrame& b, const std::string& col,
           std::int64_t row) {
    const dataframe::Series& c =
        b.columns[static_cast<std::size_t>(col_of(b, col))];
    switch (c.type()) {
        case dataframe::TypeId::Int64:
            return static_cast<double>(c.data<std::int64_t>()[row]);
        case dataframe::TypeId::Uint64:
            return static_cast<double>(c.data<std::uint64_t>()[row]);
        default:
            return c.data<double>()[row];
    }
}
bool is_null(const dataframe::DataFrame& b, const std::string& col,
             std::int64_t row) {
    return b.columns[static_cast<std::size_t>(col_of(b, col))].is_null(row);
}

}  // namespace

TEST_SUITE("ViewJoin") {
    TEST_CASE("LazyFrame join equi-joins two aggregated Views") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);  // 30 posix, 20 stdio
        std::string idx = determine_index_path(gz, "");

        auto left = View::from_file(gz, idx)
                        .group_by({GroupKey::cat()})
                        .agg({{AggOp::Count, "", "n"}});
        auto right = View::from_file(gz, idx)
                         .query(R"(cat == "POSIX")")
                         .group_by({GroupKey::cat()})
                         .agg({{AggOp::Sum, "dur", "s"}});

        SUBCASE("Inner keeps the key intersection with both sides present") {
            dataframe::DataFrame j =
                run(left.join(right.lazy(), {"cat"}, dataframe::JoinHow::Inner)
                        .collect());
            REQUIRE(j.num_rows() == 1);
            const std::int64_t p = row_of(j, "posix");
            REQUIRE(p >= 0);
            CHECK(num(j, "n", p) == doctest::Approx(30.0));
            // dur = 10 + i for i in [0,30): sum(10..39) = 735.
            CHECK(num(j, "s", p) == doctest::Approx(735.0));
            CHECK(row_of(j, "stdio") == -1);
        }

        SUBCASE("Left keeps all left keys; unmatched null the right side") {
            dataframe::DataFrame j =
                run(left.join(right.lazy(), {"cat"}, dataframe::JoinHow::Left)
                        .sort_by("cat")
                        .collect());
            REQUIRE(j.num_rows() == 2);
            CHECK(j.columns[0].string_at(0) == "posix");
            CHECK(j.columns[0].string_at(1) == "stdio");
            const std::int64_t s = row_of(j, "stdio");
            REQUIRE(s >= 0);
            CHECK(num(j, "n", s) == doctest::Approx(20.0));
            CHECK(is_null(j, "s", s));
        }

        SUBCASE("join is deterministic across repeated runs") {
            auto plan =
                left.join(right.lazy(), {"cat"}, dataframe::JoinHow::Outer)
                    .sort_by("cat");
            dataframe::DataFrame a = run(plan.collect());
            dataframe::DataFrame b = run(plan.collect());
            REQUIRE(a.num_rows() == b.num_rows());
            for (std::int64_t i = 0; i < a.num_rows(); ++i)
                CHECK(a.columns[0].string_at(i) == b.columns[0].string_at(i));
        }

        SUBCASE("a key absent from one side fails at collect") {
            auto other = View::from_file(gz, idx)
                             .group_by({GroupKey::pid()})
                             .agg({{AggOp::Count, "", "n"}});
            auto plan =
                left.join(other.lazy(), {"cat"}, dataframe::JoinHow::Inner);
            CHECK_THROWS(run(plan.collect()));
        }
    }
}
