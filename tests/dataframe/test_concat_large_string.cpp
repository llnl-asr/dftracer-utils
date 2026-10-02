// concat accepts string and large_string columns, alone or mixed: the result is
// a string view column, nulls and order are kept, and a string next to an
// integer still fails.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
// clang-format off
#include <nanoarrow/nanoarrow.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/arrow.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
// clang-format on

using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::InMemorySource;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;
namespace dfops = dftracer::utils::dataframe;

namespace {

struct BuiltArrow {
    ArrowSchema schema{};
    ArrowArray array{};
    ~BuiltArrow() {
        if (array.release) array.release(&array);
        if (schema.release) schema.release(&schema);
    }
};

using Cells = std::vector<std::optional<std::string>>;

Series make_large_utf8(const Cells& values) {
    BuiltArrow b;
    ArrowSchemaInit(&b.schema);
    REQUIRE(ArrowSchemaSetType(&b.schema, NANOARROW_TYPE_LARGE_STRING) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(&b.array) == NANOARROW_OK);
    for (const auto& v : values) {
        if (!v) {
            REQUIRE(ArrowArrayAppendNull(&b.array, 1) == NANOARROW_OK);
            continue;
        }
        ArrowStringView sv{v->data(), static_cast<int64_t>(v->size())};
        REQUIRE(ArrowArrayAppendString(&b.array, sv) == NANOARROW_OK);
    }
    ArrowError err;
    REQUIRE(ArrowArrayFinishBuildingDefault(&b.array, &err) == NANOARROW_OK);
    return Series::from_arrow(&b.schema, &b.array);
}

// A String column with a null where `values[i]` is nullopt.
Series make_string(const Cells& values) {
    std::vector<std::string> data;
    std::vector<std::uint8_t> validity((values.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        data.push_back(values[i].value_or(""));
        if (values[i])
            validity[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    std::vector<std::string_view> views(data.begin(), data.end());
    return Series::strings(std::span<const std::string_view>(views),
                           validity.data());
}

Cells cells_of(const Series& s) {
    const Series m = s.materialize();
    Cells out;
    for (std::int64_t i = 0; i < m.length(); ++i)
        out.push_back(m.is_null(i) ? std::nullopt
                                   : std::optional<std::string>(
                                         std::string(m.string_at(i))));
    return out;
}

}  // namespace

TEST_SUITE("concat string and large_string") {
    TEST_CASE("two large_string parts concatenate into a string view") {
        Series a = make_large_utf8({"a", "bb"});
        Series b = make_large_utf8({"ccc", "dddd"});
        REQUIRE(a.type() == TypeId::LargeString);
        Series out = dfops::concat_columns({&a, &b});
        REQUIRE(out.valid());
        CHECK(out.type() == TypeId::String);
        CHECK(out.encoding() == dftracer::utils::dataframe::Encoding::View);
        CHECK(cells_of(out) == Cells{"a", "bb", "ccc", "dddd"});
    }

    TEST_CASE("a string part and a large_string part, in either order") {
        Series s = make_string({"x", "yy"});
        Series l = make_large_utf8({"zzz"});
        Series sl = dfops::concat_columns({&s, &l});
        Series ls = dfops::concat_columns({&l, &s});
        CHECK(sl.type() == TypeId::String);
        CHECK(ls.type() == TypeId::String);
        CHECK(cells_of(sl) == Cells{"x", "yy", "zzz"});
        CHECK(cells_of(ls) == Cells{"zzz", "x", "yy"});
    }

    TEST_CASE("nulls are kept at the same rows") {
        Series a = make_large_utf8({"a", std::nullopt, "c"});
        Series b = make_string({std::nullopt, "e"});
        Series out = dfops::concat_columns({&a, &b});
        CHECK(out.null_count() == 2);
        CHECK(cells_of(out) ==
              Cells{"a", std::nullopt, "c", std::nullopt, "e"});
    }

    TEST_CASE("empty values and an empty part") {
        Series a = make_large_utf8({"", "x"});
        Series e = make_large_utf8({});
        Series out = dfops::concat_columns({&a, &e, &a});
        CHECK(cells_of(out) == Cells{"", "x", "", "x"});
    }

    TEST_CASE("string parts alone stay string") {
        Series a = make_string({"a"});
        Series b = make_string({"b", std::nullopt});
        Series out = dfops::concat_columns({&a, &b});
        CHECK(out.type() == TypeId::String);
        CHECK(cells_of(out) == Cells{"a", "b", std::nullopt});
    }

    TEST_CASE("a string next to an integer still fails") {
        Series a = make_large_utf8({"a"});
        const std::vector<std::int64_t> v{1};
        Series i = Series::flat_i64(v.data(), 1);
        CHECK_THROWS_AS(dfops::concat_columns({&a, &i}), std::invalid_argument);
        CHECK_THROWS_AS(dfops::concat_columns({&i, &a}), std::invalid_argument);
    }

    TEST_CASE("a frame concat keeps every column") {
        DataFrame x, y;
        x.names = {"s", "v"};
        y.names = {"s", "v"};
        const std::vector<std::int64_t> v0{1, 2}, v1{3};
        x.columns.push_back(make_large_utf8({"a", "b"}));
        x.columns.push_back(Series::flat_i64(v0.data(), 2));
        y.columns.push_back(make_string({"c"}));
        y.columns.push_back(Series::flat_i64(v1.data(), 1));
        DataFrame out = dfops::concat({&x, &y});
        REQUIRE(out.num_rows() == 3);
        CHECK(cells_of(out.column("s")) == Cells{"a", "b", "c"});
    }

    TEST_CASE("a lazy concat of a string and a large_string plan collects") {
        DataFrame x, y;
        x.names = {"s"};
        y.names = {"s"};
        x.columns.push_back(make_string({"a", "b"}));
        y.columns.push_back(make_large_utf8({"c"}));
        LazyFrame lx =
            LazyFrame::scan(std::make_shared<InMemorySource>(std::move(x)));
        LazyFrame ly =
            LazyFrame::scan(std::make_shared<InMemorySource>(std::move(y)));
        DataFrame out = dftracer::utils::default_runtime()
                            .submit(lx.concat(ly).collect())
                            .get();
        CHECK(cells_of(out.column("s")) == Cells{"a", "b", "c"});
    }
}
#else
TEST_SUITE("concat string and large_string") {
    TEST_CASE("Arrow disabled: nothing to exercise") { CHECK(true); }
}
#endif
