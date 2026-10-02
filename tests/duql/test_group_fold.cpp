#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/duql/group_fold.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace df = dftracer::utils::dataframe;
using dftracer::utils::duql::FoldAgg;
using dftracer::utils::duql::FoldOp;
using dftracer::utils::duql::GroupFold;
using Cells = std::vector<std::optional<std::string>>;

namespace {

df::Series strs(const Cells& cells) {
    std::vector<std::string_view> v;
    std::vector<std::uint8_t> valid((cells.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        v.emplace_back(cells[i] ? std::string_view(*cells[i])
                                : std::string_view());
        if (cells[i]) valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    return df::Series::strings(v, valid.data());
}

df::Series ints(const std::vector<std::int64_t>& v) {
    return df::Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
}

df::DataFrame batch(df::Series k, df::Series v) {
    df::DataFrame f;
    f.names = {"k", "v"};
    f.columns.push_back(std::move(k));
    f.columns.push_back(std::move(v));
    return f;
}

template <class... Batches>
df::DataFrame fold(df::DataType key_type, Batches... bs) {
    FoldAgg a;
    a.op = FoldOp::COUNT_DISTINCT;
    a.in = 1;
    a.type = df::scalar(df::TypeId::Int64);
    GroupFold g({key_type}, {a});
    (g.add(std::move(bs)), ...);
    return g.finish({"k", "d"});
}

std::vector<std::int64_t> counts(const df::DataFrame& f) {
    const df::Series& d = f.columns[1];
    return {d.data<std::int64_t>(), d.data<std::int64_t>() + d.length()};
}

}  // namespace

TEST_CASE("count_distinct by a string key keeps first-seen order and nulls") {
    const df::DataFrame out = fold(
        df::scalar(df::TypeId::String),
        batch(strs({"a", "b", "a", std::nullopt, "b", "a"}),
              strs({"x", "y", "x", "z", std::nullopt, "w"})),
        batch(strs({"c", std::nullopt, "a", "b"}), strs({"x", "z", "x", "y"})));
    REQUIRE(out.num_rows() == 4);
    CHECK(out.columns[0].string_at(0) == "a");
    CHECK(out.columns[0].string_at(1) == "b");
    CHECK(out.columns[0].is_null(2));
    CHECK(out.columns[0].string_at(3) == "c");
    CHECK(counts(out) == std::vector<std::int64_t>{2, 1, 1, 1});
}

TEST_CASE("a string that spells the null key is its own group") {
    const df::DataFrame out =
        fold(df::scalar(df::TypeId::String),
             batch(strs({std::string(1, '\0'), std::nullopt, ""}),
                   strs({"p", "q", "r"})));
    REQUIRE(out.num_rows() == 3);
    CHECK(counts(out) == std::vector<std::int64_t>{1, 1, 1});
}

TEST_CASE("count_distinct over a JSON column reads values, not text") {
    const df::DataFrame out =
        fold(df::scalar(df::TypeId::String),
             batch(strs({"a", "a", "a", "a"}),
                   strs({"1", "1.0", "\"1\"", std::nullopt}).as_json()));
    REQUIRE(out.num_rows() == 1);
    CHECK(counts(out) == std::vector<std::int64_t>{2});
}

TEST_CASE("count_distinct by an integer key takes the general path") {
    const df::DataFrame out = fold(
        df::scalar(df::TypeId::Int64),
        batch(ints({2, 1, 2, 1, 3}), strs({"a", "a", "a", "b", std::nullopt})),
        batch(ints({3, 2}), strs({"c", "b"})));
    REQUIRE(out.num_rows() == 3);
    const std::int64_t* k = out.columns[0].data<std::int64_t>();
    CHECK(k[0] == 2);
    CHECK(k[1] == 1);
    CHECK(k[2] == 3);
    CHECK(counts(out) == std::vector<std::int64_t>{2, 2, 1});
}

TEST_CASE("a JSON batch and a plain batch share groups and distinct values") {
    const df::DataFrame mixed = fold(
        df::scalar(df::TypeId::String),
        batch(strs({"a", "a\"b", "a", std::nullopt}),
              strs({"x", "q\"r", "x", "z"})),
        batch(strs({"\"a\"", "\"a\\\"b\"", std::nullopt, "\"c\""}).as_json(),
              strs({"\"x\"", "\"q\\\"r\"", "\"z\"", "\"x\""}).as_json()));
    const df::DataFrame plain =
        fold(df::scalar(df::TypeId::String),
             batch(strs({"a", "a\"b", "a", std::nullopt}),
                   strs({"x", "q\"r", "x", "z"})),
             batch(strs({"a", "a\"b", std::nullopt, "c"}),
                   strs({"x", "q\"r", "z", "x"})));
    REQUIRE(mixed.num_rows() == 4);
    REQUIRE(plain.num_rows() == 4);
    for (std::int64_t i = 0; i < 4; ++i)
        CHECK(mixed.columns[0].is_null(i) == plain.columns[0].is_null(i));
    CHECK(counts(mixed) == counts(plain));
    CHECK(counts(mixed) == std::vector<std::int64_t>{1, 1, 1, 1});
}

TEST_CASE("a JSON number and the same text stay distinct") {
    const df::DataFrame out =
        fold(df::scalar(df::TypeId::String),
             batch(strs({"5", "\"5\"", "5"}).as_json(),
                   strs({"5", "\"5\"", "\"5\""}).as_json()),
             batch(strs({"5", "5"}), strs({"5", "7"})));
    REQUIRE(out.num_rows() == 2);
    CHECK(out.columns[0].string_at(0) == "5");
    CHECK(counts(out) == std::vector<std::int64_t>{2, 2});
}
