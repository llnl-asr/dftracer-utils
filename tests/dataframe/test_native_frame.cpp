// A DataFrame round-trips through the native byte format, with or without
// Arrow, and foreign bytes read as no frame.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dataframe/series.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

Series strings(std::vector<std::string> v) { return Series::strings(v); }

DataFrame sample() {
    DataFrame f;
    const std::int64_t ints[] = {1, 2, 3, 4};
    const std::uint8_t valid[] = {0b1011};
    const double doubles[] = {1.5, -2.25, 0, 1e300};
    const std::uint8_t bools[] = {0b0101};
    f.names = {"i", "d", "s", "b", "j", "ts", "dec", "tags", "st", "nest"};
    f.columns.push_back(Series::flat_i64(ints, 4, valid));
    f.columns.push_back(Series::flat_f64(doubles, 4));
    f.columns.push_back(strings({"", "alpha", "b\"eta", "\xc3\xa9"}));
    f.columns.push_back(Series::flat(TypeId::Bool, bools, 4));
    f.columns.push_back(strings({"1", "\"x\"", "true", "[1,2]"}).as_json());

    Series ts = Series::flat_i64(ints, 4);
    ts.handle()->type = TypeId::Timestamp;
    ts.handle()->time_unit = TimeUnit::Milli;
    ts.handle()->timezone = "Europe/Paris";
    f.columns.push_back(std::move(ts));

    const std::uint8_t dec[64] = {
        7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Series d = Series::flat(TypeId::Decimal128, dec, 4);
    d.handle()->decimal_precision = 20;
    d.handle()->decimal_scale = 3;
    f.columns.push_back(std::move(d));

    // List<String>: ["a","b"], [], ["c"], ["d","e","f"]
    f.columns.push_back(
        Series::list({0, 2, 2, 3, 6}, strings({"a", "b", "c", "d", "e", "f"})));

    std::vector<Series> fields;
    fields.push_back(Series::flat_i64(ints, 4));
    fields.push_back(strings({"w", "x", "y", "z"}));
    f.columns.push_back(Series::structs({"n", "name"}, std::move(fields)));

    // List<Struct{n}> with a list offset that does not start at zero.
    std::vector<Series> inner;
    inner.push_back(Series::flat_i64(ints, 4));
    f.columns.push_back(Series::list({0, 1, 3, 3, 4},
                                     Series::structs({"n"}, std::move(inner))));
    return f;
}

std::string bytes_of(const DataFrame& f) { return frame_to_native(f); }

}  // namespace

TEST_SUITE("native frame") {
    TEST_CASE("every column type round-trips") {
        const DataFrame f = sample();
        for (std::size_t c = 0; c < f.columns.size(); ++c) {
            CAPTURE(f.names[c]);
            REQUIRE(f.columns[c].valid());
        }
        const std::string bytes = bytes_of(f);
        const auto back = frame_from_native(bytes);
        REQUIRE(back);
        CHECK(back->names == f.names);
        CHECK(back->num_rows() == 4);
        REQUIRE(back->columns.size() == f.columns.size());
        for (std::size_t c = 0; c < f.columns.size(); ++c) {
            CAPTURE(f.names[c]);
            CHECK(back->columns[c].type() == f.columns[c].type());
            CHECK(back->columns[c].null_count() == f.columns[c].null_count());
        }
        CHECK(bytes_of(*back) == bytes);
        CHECK(back->columns[0].is_null(2));
        CHECK(back->columns[4].is_json());
        CHECK_FALSE(back->columns[2].is_json());
        CHECK(back->columns[4].string_at(1) == "\"x\"");
        CHECK(back->columns[5].handle()->time_unit == TimeUnit::Milli);
        CHECK(back->columns[5].handle()->timezone == "Europe/Paris");
        CHECK(back->columns[6].handle()->decimal_scale == 3);
        CHECK(back->columns[6].handle()->decimal_precision == 20);
        const Series& tags = back->columns[7];
        CHECK(tags.child(0).length() == 6);
        CHECK(tags.child(0).string_at(5) == "f");
        CHECK(back->columns[8].field_name(1) == "name");
        CHECK(back->columns[8].child(1).string_at(3) == "z");
        const Series& nest = back->columns[9];
        CHECK(nest.child(0).num_children() == 1);
    }

    TEST_CASE("an empty frame and a frame with no columns") {
        DataFrame none;
        const auto a = frame_from_native(bytes_of(none));
        REQUIRE(a);
        CHECK(a->columns.empty());
        DataFrame empty;
        empty.names = {"x"};
        empty.columns.push_back(strings({}));
        const auto b = frame_from_native(bytes_of(empty));
        REQUIRE(b);
        CHECK(b->num_rows() == 0);
        CHECK(b->names == empty.names);
    }

    TEST_CASE("a slice writes its own rows") {
        const DataFrame f = sample();
        DataFrame cut;
        cut.names = f.names;
        for (const Series& c : f.columns) cut.columns.push_back(c.take({1, 2}));
        const auto back = frame_from_native(bytes_of(cut));
        REQUIRE(back);
        CHECK(back->num_rows() == 2);
        CHECK(back->columns[2].string_at(0) == "alpha");
        CHECK(back->columns[7].child(0).length() == 1);
        CHECK(back->columns[7].child(0).string_at(0) == "c");
    }

    TEST_CASE("bytes of another format are no frame") {
        const std::string ipc("\xff\xff\xff\xff\x10\x00\x00\x00", 8);
        CHECK_FALSE(frame_from_native(ipc));
        CHECK_FALSE(frame_from_native(""));
        const std::string good = bytes_of(sample());
        CHECK_FALSE(frame_from_native(good.substr(0, good.size() / 2)));
        CHECK_FALSE(frame_from_native(good + "x"));
        std::string other_version = good;
        other_version[4] = 9;
        CHECK_FALSE(frame_from_native(other_version));
    }

    TEST_CASE("a map and a fixed-size list round-trip") {
        DataFrame f;
        f.names = {"m", "fl"};
        Series m =
            Series::list({0, 1, 1}, Series::structs({"key", "value"}, [] {
                             std::vector<Series> c;
                             c.push_back(strings({"a"}));
                             const std::int64_t v[] = {7};
                             c.push_back(Series::flat_i64(v, 1));
                             return c;
                         }()));
        m.handle()->type = TypeId::Map;
        f.columns.push_back(std::move(m));
        Series fl = Series::list({0, 2, 4}, strings({"a", "b", "c", "d"}));
        fl.handle()->type = TypeId::FixedSizeList;
        fl.handle()->fixed_size = 2;
        fl.handle()->offsets.reset();
        f.columns.push_back(std::move(fl));
        const auto back = frame_from_native(frame_to_native(f));
        REQUIRE(back);
        CHECK(back->columns[0].type() == TypeId::Map);
        CHECK(back->columns[1].type() == TypeId::FixedSizeList);
        CHECK(back->columns[1].handle()->fixed_size == 2);
        CHECK(back->columns[1].child(0).string_at(3) == "d");
    }

    TEST_CASE("a group key keeps its JSON flag") {
        DataFrame f;
        f.names = {"k", "v"};
        f.columns.push_back(strings({"1", "\"1\"", "1", "\"1\""}).as_json());
        const std::int64_t v[] = {1, 2, 3, 4};
        f.columns.push_back(Series::flat_i64(v, 4));
        const DataFrame g =
            f.group_by(std::string("k"), {GroupAgg{Agg::Sum, "v", "s"}});
        REQUIRE(g.num_rows() == 2);
        CHECK(g.column("k").is_json());
        const auto back = frame_from_native(frame_to_native(g));
        REQUIRE(back);
        CHECK(back->column("k").is_json());
    }
}
