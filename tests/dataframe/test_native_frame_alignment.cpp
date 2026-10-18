// A native frame decodes the same whatever the length of its column names. The
// blob puts a name of any length before each column, so the typed arrays after
// it start at any byte, and the decoder may not read them in place.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dataframe/series.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

const std::vector<std::string> TEXT = {"alpha", "", "b", "gamma-delta"};
const std::int32_t INT32S[] = {-7, 0, 2147483647, 42};
const std::int64_t INT64S[] = {-9000000000LL, 0, 1, 9000000000LL};
const double DOUBLES[] = {1.5, -2.25, 0.0, 1e300};

Series large_strings(const std::vector<std::string>& values) {
    auto* col = new dftu_series();
    col->type = TypeId::LargeString;
    col->encoding = Encoding::Flat;
    col->length = static_cast<std::int64_t>(values.size());
    std::vector<std::int64_t> offsets(values.size() + 1, 0);
    std::string data;
    for (std::size_t i = 0; i < values.size(); ++i) {
        data += values[i];
        offsets[i + 1] = static_cast<std::int64_t>(data.size());
    }
    col->offsets = new_buffer(offsets.size() * sizeof(std::int64_t));
    std::memcpy(writable(col->offsets), offsets.data(),
                offsets.size() * sizeof(std::int64_t));
    col->data = new_buffer(data.size());
    std::memcpy(writable(col->data), data.data(), data.size());
    return Series{col};
}

Series binaries(const std::vector<std::string>& values) {
    Series s = Series::strings(values);
    s.handle()->type = TypeId::Binary;
    return s;
}

std::string name_of(std::size_t length, std::size_t index) {
    return std::string(length - 1, 'n') + static_cast<char>('a' + index);
}

DataFrame frame_with_name_length(std::size_t length) {
    DataFrame f;
    f.columns.push_back(Series::strings(TEXT));
    f.columns.push_back(large_strings(TEXT));
    f.columns.push_back(binaries(TEXT));
    f.columns.push_back(Series::flat(TypeId::Int32, INT32S, 4));
    f.columns.push_back(Series::flat_i64(INT64S, 4));
    f.columns.push_back(Series::flat_f64(DOUBLES, 4));
    for (std::size_t c = 0; c < f.columns.size(); ++c)
        f.names.push_back(name_of(length, c));
    return f;
}

void check_strings(const Series& s) {
    REQUIRE(s.length() == static_cast<std::int64_t>(TEXT.size()));
    for (std::size_t i = 0; i < TEXT.size(); ++i) {
        CAPTURE(i);
        CHECK(s.string_at(static_cast<std::int64_t>(i)) == TEXT[i]);
    }
}

template <class T, std::size_t N>
void check_flat(const Series& s, const T (&expected)[N]) {
    REQUIRE(s.length() == static_cast<std::int64_t>(N));
    const void* data = dftu_series_data(s.handle());
    REQUIRE(data != nullptr);
    CHECK(std::memcmp(data, expected, sizeof expected) == 0);
}

}  // namespace

TEST_SUITE("native frame alignment") {
    TEST_CASE("columns decode alike for name lengths 1, 2 and 3") {
        for (std::size_t length = 1; length <= 3; ++length) {
            CAPTURE(length);
            const DataFrame f = frame_with_name_length(length);
            for (const Series& c : f.columns) REQUIRE(c.valid());
            const std::string bytes = frame_to_native(f);
            const auto back = frame_from_native(bytes);
            REQUIRE(back);
            CHECK(back->names == f.names);
            REQUIRE(back->columns.size() == 6);
            CHECK(back->columns[0].type() == TypeId::String);
            CHECK(back->columns[1].type() == TypeId::LargeString);
            CHECK(back->columns[2].type() == TypeId::Binary);
            check_strings(back->columns[0]);
            check_strings(back->columns[1]);
            check_strings(back->columns[2]);
            check_flat(back->columns[3], INT32S);
            check_flat(back->columns[4], INT64S);
            check_flat(back->columns[5], DOUBLES);
            CHECK(frame_to_native(*back) == bytes);
        }
    }

    TEST_CASE("a column after a null bitmap decodes at any name length") {
        for (std::size_t length = 1; length <= 3; ++length) {
            CAPTURE(length);
            DataFrame f;
            const std::uint8_t valid[] = {0b1101};
            f.columns.push_back(Series::flat_i64(INT64S, 4, valid));
            f.columns.push_back(Series::strings(TEXT));
            f.columns.push_back(large_strings(TEXT));
            for (std::size_t c = 0; c < f.columns.size(); ++c)
                f.names.push_back(name_of(length, c));
            const std::string bytes = frame_to_native(f);
            const auto back = frame_from_native(bytes);
            REQUIRE(back);
            CHECK(back->columns[0].is_null(1));
            check_strings(back->columns[1]);
            check_strings(back->columns[2]);
            CHECK(frame_to_native(*back) == bytes);
        }
    }
}
