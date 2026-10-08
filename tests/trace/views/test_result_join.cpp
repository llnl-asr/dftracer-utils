// Native equi join over two aggregation-result Batches: builds two Batches with
// a shared group-key schema and joins them via vec's take() (nulls for OUTER
// rows). No Arrow. The join only compares the key columns and copies value
// columns opaquely, so it is generic over any aggregation schema.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/result_join.h>
#include <doctest/doctest.h>

#include <initializer_list>
#include <set>
#include <string>
#include <vector>

namespace dataframe = dftracer::utils::dataframe;

namespace views = dftracer::utils::trace::views;
using views::join_batches;
using views::JoinType;

namespace {

// A DataFrame of cat(String) + one Float64 value column.
dataframe::DataFrame make(const std::vector<std::string>& cats,
                          const std::string& vname,
                          const std::vector<double>& vals) {
    dataframe::DataFrame b;
    b.names = {"cat", vname};
    b.columns.push_back(dataframe::Series::strings(cats));
    b.columns.push_back(dataframe::Series::flat_f64(
        vals.data(), static_cast<std::int64_t>(vals.size())));
    return b;
}

std::int64_t col_of(const dataframe::DataFrame& b, const std::string& name) {
    for (std::size_t i = 0; i < b.names.size(); ++i)
        if (b.names[i] == name) return static_cast<std::int64_t>(i);
    return -1;
}
std::int64_t row_of(const dataframe::DataFrame& b, const std::string& cat) {
    for (std::int64_t i = 0; i < b.num_rows(); ++i)
        if (b.columns[0].string_at(i) == cat) return i;
    return -1;
}
std::set<std::string> keys_of(const dataframe::DataFrame& b) {
    std::set<std::string> out;
    for (std::int64_t i = 0; i < b.num_rows(); ++i)
        out.insert(std::string(b.columns[0].string_at(i)));
    return out;
}
double num(const dataframe::DataFrame& b, const std::string& col,
           std::int64_t row) {
    return b.columns[static_cast<std::size_t>(col_of(b, col))]
        .data<double>()[row];
}
bool is_null(const dataframe::DataFrame& b, const std::string& col,
             std::int64_t row) {
    return b.columns[static_cast<std::size_t>(col_of(b, col))].is_null(row);
}

// left = cat -> count over {a,b,c}; right = cat -> sum over {b,c,d}.
std::vector<std::string> cats_of(std::initializer_list<const char*> names) {
    std::vector<std::string> out;
    out.reserve(names.size());
    for (const char* n : names) out.emplace_back(n);
    return out;
}

dataframe::DataFrame build_left() {
    const std::vector<std::string> cats = cats_of({"a", "b", "c"});
    return make(cats, "count", {10, 25, 30});
}
dataframe::DataFrame build_right() {
    const std::vector<std::string> cats = cats_of({"b", "c", "d"});
    return make(cats, "sum", {2.5, 3.5, 4.0});
}

}  // namespace

TEST_SUITE("ResultJoin") {
    TEST_CASE("INNER keeps only the key intersection") {
        dataframe::DataFrame j =
            join_batches(build_left(), build_right(), 1, JoinType::INNER);
        CHECK(keys_of(j) == std::set<std::string>{"b", "c"});
        CHECK(col_of(j, "l_count") >= 0);
        CHECK(col_of(j, "r_sum") >= 0);
        CHECK(num(j, "l_count", row_of(j, "b")) == doctest::Approx(25.0));
        CHECK(num(j, "r_sum", row_of(j, "b")) == doctest::Approx(2.5));
        CHECK(num(j, "r_sum", row_of(j, "c")) == doctest::Approx(3.5));
    }

    TEST_CASE("LEFT keeps all left keys; unmatched null the right side") {
        dataframe::DataFrame j =
            join_batches(build_left(), build_right(), 1, JoinType::LEFT);
        CHECK(keys_of(j) == std::set<std::string>{"a", "b", "c"});
        CHECK(num(j, "l_count", row_of(j, "a")) == doctest::Approx(10.0));
        CHECK(is_null(j, "r_sum", row_of(j, "a")));
        CHECK_FALSE(is_null(j, "r_sum", row_of(j, "b")));
        CHECK(num(j, "r_sum", row_of(j, "b")) == doctest::Approx(2.5));
    }

    TEST_CASE("RIGHT keeps all right keys; unmatched null the left side") {
        dataframe::DataFrame j =
            join_batches(build_left(), build_right(), 1, JoinType::RIGHT);
        CHECK(keys_of(j) == std::set<std::string>{"b", "c", "d"});
        CHECK(is_null(j, "l_count", row_of(j, "d")));
        CHECK(num(j, "r_sum", row_of(j, "d")) == doctest::Approx(4.0));
        CHECK(num(j, "l_count", row_of(j, "c")) == doctest::Approx(30.0));
    }

    TEST_CASE("FULL keeps the key union, nulling each absent side") {
        dataframe::DataFrame j =
            join_batches(build_left(), build_right(), 1, JoinType::FULL);
        CHECK(keys_of(j) == std::set<std::string>{"a", "b", "c", "d"});
        CHECK(is_null(j, "r_sum", row_of(j, "a")));
        CHECK(is_null(j, "l_count", row_of(j, "d")));
        CHECK_FALSE(is_null(j, "l_count", row_of(j, "b")));
        CHECK_FALSE(is_null(j, "r_sum", row_of(j, "b")));
    }

    TEST_CASE("LEFT_SEMI keeps matched left rows, left columns only") {
        dataframe::DataFrame j =
            join_batches(build_left(), build_right(), 1, JoinType::LEFT_SEMI);
        CHECK(keys_of(j) == std::set<std::string>{"b", "c"});
        CHECK(col_of(j, "l_count") >= 0);
        CHECK(col_of(j, "r_sum") == -1);  // no right columns
        CHECK(num(j, "l_count", row_of(j, "b")) == doctest::Approx(25.0));
    }

    TEST_CASE("LEFT_ANTI keeps unmatched left rows, left columns only") {
        dataframe::DataFrame j =
            join_batches(build_left(), build_right(), 1, JoinType::LEFT_ANTI);
        CHECK(keys_of(j) == std::set<std::string>{"a"});
        CHECK(col_of(j, "r_sum") == -1);
        CHECK(num(j, "l_count", row_of(j, "a")) == doctest::Approx(10.0));
    }

    TEST_CASE("rows are sorted by key; column order is key, l_, r_") {
        dataframe::DataFrame j =
            join_batches(build_left(), build_right(), 1, JoinType::FULL);
        for (std::int64_t i = 0; i + 1 < j.num_rows(); ++i)
            CHECK(j.columns[0].string_at(i) < j.columns[0].string_at(i + 1));
        CHECK(j.names[0] == "cat");
        CHECK(j.names[1] == "l_count");
        CHECK(j.names[2] == "r_sum");
    }

    TEST_CASE("multi-column group key joins on the full tuple") {
        dataframe::DataFrame l;
        l.names = {"pid", "name", "count"};
        l.columns.push_back(dataframe::Series::strings({"1", "1", "2"}));
        l.columns.push_back(
            dataframe::Series::strings({"read", "write", "read"}));
        std::vector<double> lv{1.0, 2.0, 3.0};
        l.columns.push_back(dataframe::Series::flat_f64(lv.data(), 3));
        dataframe::DataFrame r;
        r.names = {"pid", "name", "sum"};
        r.columns.push_back(dataframe::Series::strings({"1", "2", "2"}));
        r.columns.push_back(
            dataframe::Series::strings({"write", "read", "open"}));
        std::vector<double> rv{9.0, 8.0, 7.0};
        r.columns.push_back(dataframe::Series::flat_f64(rv.data(), 3));

        dataframe::DataFrame j = join_batches(l, r, 2, JoinType::INNER);
        REQUIRE(j.num_rows() == 2);  // (1,write), (2,read)
        CHECK(j.columns[0].string_at(0) == "1");
        CHECK(j.columns[1].string_at(0) == "write");
        CHECK(num(j, "l_count", 0) == doctest::Approx(2.0));
        CHECK(num(j, "r_sum", 0) == doctest::Approx(9.0));
    }

    TEST_CASE("a text column survives the join opaquely") {
        dataframe::DataFrame l;
        l.names = {"cat", "top"};
        l.columns.push_back(dataframe::Series::strings({"a", "b"}));
        l.columns.push_back(dataframe::Series::strings({"read", "write"}));
        dataframe::DataFrame r = make({"a"}, "sum", {2.5});

        dataframe::DataFrame j = join_batches(l, r, 1, JoinType::LEFT);
        const std::int64_t ci = col_of(j, "l_top");
        REQUIRE(ci >= 0);
        CHECK(j.columns[static_cast<std::size_t>(ci)].string_at(
                  row_of(j, "a")) == "read");
        CHECK(is_null(j, "r_sum", row_of(j, "b")));
    }

    TEST_CASE("mismatched key schema yields an empty DataFrame") {
        dataframe::DataFrame l = make({"a"}, "count", {1.0});
        dataframe::DataFrame r;
        r.names = {"pid", "name"};
        r.columns.push_back(dataframe::Series::strings({"1"}));
        r.columns.push_back(dataframe::Series::strings({"read"}));
        dataframe::DataFrame j = join_batches(l, r, 1, JoinType::INNER);
        CHECK(j.num_columns() == 0);
    }

    TEST_CASE("a non-string key column throws instead of crashing") {
        // Keys are matched with string_at(); a numeric key would read a null
        // offset buffer, so join_batches rejects it up front.
        const double keys[] = {1.0, 2.0};
        dataframe::DataFrame b;
        b.names = {"k", "count"};
        b.columns.push_back(dataframe::Series::flat_f64(keys, 2));
        b.columns.push_back(dataframe::Series::flat_f64(keys, 2));
        CHECK_THROWS_AS(join_batches(b, b, 1, JoinType::INNER),
                        dftracer::utils::DFTUtilsException);
    }
}
