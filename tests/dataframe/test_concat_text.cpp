// Concatenating String columns copies buffers and must give what appending the
// rows one at a time gives: values, nulls, type; a null row holds no bytes even
// when its own offset range is not empty; empty parts add nothing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;
namespace dfops = dftracer::utils::dataframe;

namespace {

using Row = std::optional<std::string>;
using Rows = std::vector<Row>;

Series build(const Rows& rows) {
    std::vector<std::string_view> views;
    std::vector<std::uint8_t> validity((rows.size() + 7) / 8, 0xFF);
    bool any_null = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i]) {
            views.emplace_back(*rows[i]);
        } else {
            views.emplace_back();  // a null slot holds no bytes
            validity[i >> 3] &= static_cast<std::uint8_t>(~(1u << (i & 7)));
            any_null = true;
        }
    }
    return Series::strings(std::span<const std::string_view>(views),
                           any_null ? validity.data() : nullptr);
}

Rows read(const Series& s) {
    Rows out;
    for (std::int64_t i = 0; i < s.length(); ++i)
        out.push_back(s.is_null(i) ? Row{} : Row{std::string(s.string_at(i))});
    return out;
}

}  // namespace

TEST_CASE("strings with nulls and an empty part") {
    Series a = build({Row{"a"}, Row{}, Row{"ccc"}});
    Series e = build({});
    Series b = build({Row{""}, Row{"dd"}});
    Series out = dfops::concat_columns({&a, &e, &b});
    CHECK(out.type() == TypeId::String);
    CHECK(out.length() == 5);
    CHECK(out.null_count() == 1);
    CHECK(read(out) == Rows{Row{"a"}, Row{}, Row{"ccc"}, Row{""}, Row{"dd"}});
}

TEST_CASE("a null row with a non-empty offset range copies no bytes") {
    // Row 1 is null but spans the two bytes "bc" of its own data.
    const std::int32_t off[] = {0, 1, 3, 4};
    const char data[] = "abcd";
    const std::uint8_t valid = 0b101;
    Series odd{dftu_series_new_string(static_cast<dftu_dtype>(TypeId::String),
                                      off, data, 3, &valid)};
    Series z = build({Row{"z"}});
    Series out = dfops::concat_columns({&odd, &z});
    CHECK(read(out) == Rows{Row{"a"}, Row{}, Row{"d"}, Row{"z"}});
    REQUIRE(out.offsets() != nullptr);
    CHECK(out.offsets()[4] ==
          3);  // "a" + "d" + "z"; the null row's "bc" is not copied
}

TEST_CASE("a part with no data pointer and all-empty rows") {
    Series empties = build({Row{""}, Row{""}, Row{""}});
    Series s = build({Row{"x"}});
    Series out = dfops::concat_columns({&empties, &s, &empties});
    CHECK(read(out) ==
          Rows{Row{""}, Row{""}, Row{""}, Row{"x"}, Row{""}, Row{""}, Row{""}});
}

TEST_CASE("random parts equal appending the rows one at a time") {
    std::mt19937_64 rng(20261001);
    for (int round = 0; round < 60; ++round) {
        const int parts = 1 + static_cast<int>(rng() % 9);
        std::vector<Series> built;
        Rows want;
        for (int p = 0; p < parts; ++p) {
            const int rows = static_cast<int>(rng() % 300);
            const bool nulls = rng() % 3 != 0;
            Rows part;
            for (int i = 0; i < rows; ++i) {
                if (nulls && rng() % 5 == 0) {
                    part.emplace_back();
                    continue;
                }
                std::string s(static_cast<std::size_t>(rng() % 41), 'x');
                for (char& c : s) c = static_cast<char>('a' + rng() % 26);
                part.emplace_back(std::move(s));
            }
            want.insert(want.end(), part.begin(), part.end());
            built.push_back(build(part));
        }
        std::vector<const Series*> ptrs;
        for (const Series& s : built) ptrs.push_back(&s);
        Series out = dfops::concat_columns(ptrs);
        REQUIRE(out.length() == static_cast<std::int64_t>(want.size()));
        CHECK(out.type() == TypeId::String);
        CHECK(read(out) == want);
        std::int64_t nulls = 0;
        for (const Row& r : want) nulls += r ? 0 : 1;
        CHECK(out.null_count() == nulls);
    }
}
