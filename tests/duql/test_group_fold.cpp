// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/query/group_fold.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>
#include <optional>
#include <string_view>

using namespace dftracer::utils;

using Cells = dstd::Vector<dstd::Optional<dstd::String>>;

namespace {

dataframe::Series strs(const Cells& cells) {
    dstd::Vector<dstd::StringView> v;
    dstd::Vector<std::uint8_t> valid((cells.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        v.emplace_back(cells[i] ? dstd::StringView(*cells[i])
                                : dstd::StringView());
        if (cells[i]) valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    return dataframe::Series::strings(v, valid.data());
}

dataframe::Series ints(const dstd::Vector<std::int64_t>& v) {
    return dataframe::Series::flat_i64(v.data(),
                                       static_cast<std::int64_t>(v.size()));
}

dataframe::DataFrame batch(dataframe::Series k, dataframe::Series v) {
    dataframe::DataFrame f;
    f.names = {"k", "v"};
    f.columns.push_back(std::move(k));
    f.columns.push_back(std::move(v));
    return f;
}

template <class... Batches>
dataframe::DataFrame fold(dataframe::DataType key_type, Batches... bs) {
    dataframe::query::FoldAgg a;
    a.op = dataframe::query::FoldOp::CountDistinct;
    a.in = 1;
    a.type = dataframe::scalar(dataframe::TypeId::Int64);
    dataframe::query::GroupFold g({key_type}, {a});
    (dftu_utils_test::ok(g.add(std::move(bs))), ...);
    return dftu_utils_test::ok(g.finish({"k", "d"}));
}

dstd::Vector<std::int64_t> counts(const dataframe::DataFrame& f) {
    const dataframe::Series& d = f.columns[1];
    return {d.data<std::int64_t>(), d.data<std::int64_t>() + d.length()};
}

}  // namespace

TEST_CASE("count_distinct by a string key keeps first-seen order and nulls") {
    const dataframe::DataFrame out =
        fold(dataframe::scalar(dataframe::TypeId::String),
             batch(strs({"a", "b", "a", dstd::nullopt, "b", "a"}),
                   strs({"x", "y", "x", "z", dstd::nullopt, "w"})),
             batch(strs({"c", dstd::nullopt, "a", "b"}),
                   strs({"x", "z", "x", "y"})));
    REQUIRE(out.num_rows() == 4);
    CHECK(out.columns[0].string_at(0) == "a");
    CHECK(out.columns[0].string_at(1) == "b");
    CHECK(out.columns[0].is_null(2));
    CHECK(out.columns[0].string_at(3) == "c");
    CHECK(counts(out) == dstd::Vector<std::int64_t>{2, 1, 1, 1});
}

TEST_CASE("a string that spells the null key is its own group") {
    const dataframe::DataFrame out =
        fold(dataframe::scalar(dataframe::TypeId::String),
             batch(strs({dstd::String(1, '\0'), dstd::nullopt, ""}),
                   strs({"p", "q", "r"})));
    REQUIRE(out.num_rows() == 3);
    CHECK(counts(out) == dstd::Vector<std::int64_t>{1, 1, 1});
}

TEST_CASE("count_distinct over a JSON column reads values, not text") {
    const dataframe::DataFrame out =
        fold(dataframe::scalar(dataframe::TypeId::String),
             batch(strs({"a", "a", "a", "a"}),
                   strs({"1", "1.0", "\"1\"", dstd::nullopt}).as_json()));
    REQUIRE(out.num_rows() == 1);
    CHECK(counts(out) == dstd::Vector<std::int64_t>{2});
}

TEST_CASE("count_distinct by an integer key takes the general path") {
    const dataframe::DataFrame out = fold(
        dataframe::scalar(dataframe::TypeId::Int64),
        batch(ints({2, 1, 2, 1, 3}), strs({"a", "a", "a", "b", dstd::nullopt})),
        batch(ints({3, 2}), strs({"c", "b"})));
    REQUIRE(out.num_rows() == 3);
    const std::int64_t* k = out.columns[0].data<std::int64_t>();
    CHECK(k[0] == 2);
    CHECK(k[1] == 1);
    CHECK(k[2] == 3);
    CHECK(counts(out) == dstd::Vector<std::int64_t>{2, 2, 1});
}

TEST_CASE("a JSON batch and a plain batch share groups and distinct values") {
    const dataframe::DataFrame mixed = fold(
        dataframe::scalar(dataframe::TypeId::String),
        batch(strs({"a", "a\"b", "a", dstd::nullopt}),
              strs({"x", "q\"r", "x", "z"})),
        batch(strs({"\"a\"", "\"a\\\"b\"", dstd::nullopt, "\"c\""}).as_json(),
              strs({"\"x\"", "\"q\\\"r\"", "\"z\"", "\"x\""}).as_json()));
    const dataframe::DataFrame plain =
        fold(dataframe::scalar(dataframe::TypeId::String),
             batch(strs({"a", "a\"b", "a", dstd::nullopt}),
                   strs({"x", "q\"r", "x", "z"})),
             batch(strs({"a", "a\"b", dstd::nullopt, "c"}),
                   strs({"x", "q\"r", "z", "x"})));
    REQUIRE(mixed.num_rows() == 4);
    REQUIRE(plain.num_rows() == 4);
    for (std::int64_t i = 0; i < 4; ++i)
        CHECK(mixed.columns[0].is_null(i) == plain.columns[0].is_null(i));
    CHECK(counts(mixed) == counts(plain));
    CHECK(counts(mixed) == dstd::Vector<std::int64_t>{1, 1, 1, 1});
}

TEST_CASE("a JSON number and the same text stay distinct") {
    const dataframe::DataFrame out =
        fold(dataframe::scalar(dataframe::TypeId::String),
             batch(strs({"5", "\"5\"", "5"}).as_json(),
                   strs({"5", "\"5\"", "\"5\""}).as_json()),
             batch(strs({"5", "5"}), strs({"5", "7"})));
    REQUIRE(out.num_rows() == 2);
    CHECK(out.columns[0].string_at(0) == "5");
    CHECK(counts(out) == dstd::Vector<std::int64_t>{2, 2});
}
