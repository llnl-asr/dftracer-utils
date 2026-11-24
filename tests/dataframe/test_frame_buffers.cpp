// SPDX-License-Identifier: MIT
// A DataFrame round-trips as a header plus out-of-band buffers, and the
// import reads the buffers in place.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/frame_buffers.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dataframe/kernels/filter.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <span_list.h>
#include <task_result.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;

using namespace dftracer::utils::dataframe;

namespace {

Series strings(dstd::Vector<dstd::String> v) { return Series::strings(v); }

DataFrame sample() {
    DataFrame f;
    const std::int64_t ints[] = {1, 2, 3, 4};
    const std::uint8_t valid[] = {0b1011};
    const double doubles[] = {1.5, -2.25, 0, 1e300};
    const std::uint8_t bools[] = {0b0101};
    f.names = dstd::make_shared_strings({"i", "d", "s", "b", "j", "ts", "dec",
                                         "tags", "st", "nest", "m", "fl"});
    f.columns.push_back(Series::flat_i64(ints, 4, valid));
    f.columns.push_back(Series::flat_f64(doubles, 4));
    f.columns.push_back(strings({"", "alpha", "b\"eta", "\xc3\xa9"}));
    f.columns.push_back(Series::flat(TypeId::Bool, bools, 4));
    f.columns.push_back(strings({"1", "\"x\"", "true", "[1,2]"}).as_json());

    Series ts = Series::flat_i64(ints, 4);
    ts.handle()->type = TypeId::Timestamp;
    ts.handle()->set_time_unit(TimeUnit::Milli);
    ts.handle()->set_timezone("Europe/Paris");
    f.columns.push_back(std::move(ts));

    const std::uint8_t dec[64] = {
        7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Series d = Series::flat(TypeId::Decimal128, dec, 4);
    d.handle()->set_decimal(20, 3);
    f.columns.push_back(std::move(d));

    f.columns.push_back(
        Series::list(dftu_utils_test::span_of<std::int32_t>({0, 2, 2, 3, 6}),
                     strings({"a", "b", "c", "d", "e", "f"})));

    dstd::Vector<Series> fields;
    fields.push_back(Series::flat_i64(ints, 4));
    fields.push_back(strings({"w", "x", "y", "z"}));
    f.columns.push_back(Series::structs({"n", "name"}, std::move(fields)));

    dstd::Vector<Series> inner;
    inner.push_back(Series::flat_i64(ints, 4));
    f.columns.push_back(
        Series::list(dftu_utils_test::span_of<std::int32_t>({0, 1, 3, 3, 4}),
                     Series::structs({"n"}, std::move(inner))));

    Series m =
        Series::list(dftu_utils_test::span_of<std::int32_t>({0, 1, 1, 2, 2}),
                     Series::structs({"key", "value"}, [] {
                         dstd::Vector<Series> c;
                         c.push_back(strings({"a", "b"}));
                         const std::int64_t v[] = {7, 8};
                         c.push_back(Series::flat_i64(v, 2));
                         return c;
                     }()));
    m.handle()->type = TypeId::Map;
    f.columns.push_back(std::move(m));

    Series fl =
        Series::list(dftu_utils_test::span_of<std::int32_t>({0, 2, 4, 6, 8}),
                     strings({"a", "b", "c", "d", "e", "f", "g", "h"}));
    fl.handle()->type = TypeId::FixedSizeList;
    fl.handle()->set_fixed_size(2);
    fl.handle()->offsets = {};
    f.columns.push_back(std::move(fl));
    return f;
}

dstd::String native(const DataFrame& f) {
    return dftu_utils_test::ok(frame_to_native(f));
}

dstd::String native_column(const DataFrame& f, std::size_t c) {
    DataFrame one;
    one.names.push_back(f.names[c]);
    one.columns.push_back(f.columns[c].share());
    return native(one);
}

void check_same(const DataFrame& back, const DataFrame& f) {
    REQUIRE(back.names == f.names);
    for (std::size_t c = 0; c < f.columns.size(); ++c) {
        CAPTURE(c);
        CHECK(native_column(back, c) == native_column(f, c));
    }
    CHECK(native(back) == native(f));
}

struct Released {
    int count = 0;
};

// The buffers of `out` as a receiver sees them: the same bytes, each with an
// owner that keeps the bytes alive, as a transport does, and counts its
// release.
struct Held {
    BufferView view;
    Released* released;
};

// `r` null counts nothing, for a frame that outlives the caller's counter.
dstd::Vector<ReceivedBuffer> receive(const FrameBuffers& out, Released* r,
                                     bool read_only = true) {
    dstd::Vector<ReceivedBuffer> got;
    for (const BufferView& b : out.buffers) {
        dstd::SharedPtr<const void> owner = dstd::adopt_shared_ptr(
            static_cast<const void*>(new Held{b, r}), [](const void* p) {
                auto* held = static_cast<Held*>(const_cast<void*>(p));
                if (held->released != nullptr) ++held->released->count;
                delete held;
            });
        got.push_back({b.data(), b.size(), read_only, std::move(owner)});
    }
    return got;
}

dstd::Vector<ReceivedBuffer> receive(const FrameBuffers& out, Released& r) {
    return receive(out, &r);
}

DataFrame round_trip(const DataFrame& f) {
    const FrameBuffers out = dftu_utils_test::ok(frame_to_buffers(f));
    auto back = frame_from_buffers(out.header, receive(out, nullptr));
    REQUIRE(back);
    return std::move(*back);
}

}  // namespace

TEST_SUITE("frame buffers") {
    TEST_CASE("every column type round-trips") {
        const DataFrame f = sample();
        const DataFrame back = round_trip(f);
        CHECK(back.names == f.names);
        CHECK(native(back) == native(f));
        CHECK(back.columns[0].is_null(2));
        CHECK(back.columns[4].is_json());
        CHECK(back.columns[5].handle()->timezone() == "Europe/Paris");
        CHECK(back.columns[6].handle()->decimal_scale() == 3);
        CHECK(back.columns[11].handle()->fixed_size() == 2);
    }

    TEST_CASE("the receiver reads the buffers in place and releases each") {
        const std::int64_t ints[] = {1, 2, 3, 4};
        DataFrame f;
        f.names = dstd::make_shared_strings({"i", "s"});
        f.columns.push_back(Series::flat_i64(ints, 4));
        f.columns.push_back(strings({"a", "bb", "", "ccc"}));
        const FrameBuffers out = dftu_utils_test::ok(frame_to_buffers(f));
        CHECK(out.buffers[0].data() ==
              static_cast<const std::uint8_t*>(
                  dftu_series_data(f.columns[0].handle())));
        CHECK(out.header.size() < 128);
        Released r;
        {
            auto got = receive(out, r);
            auto back = frame_from_buffers(out.header, got);
            REQUIRE(back);
            got.clear();
            CHECK(r.count == 0);
            CHECK(dftu_series_data(back->columns[0].handle()) ==
                  static_cast<const void*>(out.buffers[0].data()));
            CHECK(back->columns[1].string_at(3) == "ccc");
        }
        CHECK(r.count == static_cast<int>(out.buffers.size()));
    }

    TEST_CASE("slices export only their own rows") {
        const DataFrame f = sample();
        DataFrame cut;
        for (std::size_t i = 0; i < f.columns.size(); ++i) {
            Series s = f.columns[i].slice(1, 2);
            if (!s.valid())
                s = f.columns[i].take(
                    dftu_utils_test::span_of<std::int64_t>({1, 2}));
            if (!s.valid()) continue;
            cut.names.push_back(f.names[i]);
            cut.columns.push_back(std::move(s));
        }
        REQUIRE(cut.columns.size() >= 10);
        const DataFrame back = round_trip(cut);
        CHECK(back.num_rows() == 2);
        check_same(back, cut);
        CHECK(back.columns[2].string_at(0) == "alpha");
        const FrameBuffers out = dftu_utils_test::ok(frame_to_buffers(cut));
        std::size_t bytes = 0;
        for (const BufferView& b : out.buffers) bytes += b.size();
        CHECK(bytes < 1024);
    }

    TEST_CASE("chunked, dictionary and selection columns are made flat") {
        const std::int64_t a[] = {1, 2};
        const std::int64_t b[] = {3};
        dstd::Vector<Series> parts;
        parts.push_back(Series::flat_i64(a, 2));
        parts.push_back(Series::flat_i64(b, 1));
        DataFrame f;
        f.names = dstd::make_shared_strings({"c", "d", "e"});
        f.columns.push_back(
            dftu_utils_test::ok(chunked_column(std::move(parts))));
        f.columns.push_back(strings({"x", "y", "x"}).dictionary_encode());
        const std::int64_t v[] = {5, 1, 9, 3, 7};
        f.columns.push_back(filter_gt(Series::flat_i64(v, 5), 4));
        REQUIRE(f.columns[0].encoding() == Encoding::Chunked);
        REQUIRE(f.columns[1].encoding() == Encoding::Dictionary);
        REQUIRE(f.columns[2].encoding() == Encoding::Selection);
        const DataFrame back = round_trip(f);
        check_same(back, f);
    }

    TEST_CASE("a buffer that is not aligned is copied") {
        const std::int64_t ints[] = {5, 6, 7};
        DataFrame f;
        f.names = dstd::make_shared_strings({"i"});
        f.columns.push_back(Series::flat_i64(ints, 3));
        const FrameBuffers out = dftu_utils_test::ok(frame_to_buffers(f));
        alignas(8) std::uint8_t raw[sizeof ints + 1];
        std::memcpy(raw + 1, out.buffers[0].data(), sizeof ints);
        dstd::Vector<ReceivedBuffer> got;
        got.push_back({raw + 1, sizeof ints, true, nullptr});
        auto back = frame_from_buffers(out.header, got);
        REQUIRE(back);
        CHECK(dftu_series_data(back->columns[0].handle()) !=
              static_cast<const void*>(raw + 1));
        CHECK(native(*back) == native(f));
    }

    TEST_CASE("empty frames and row labels round-trip") {
        DataFrame none;
        CHECK(round_trip(none).columns.empty());
        DataFrame empty;
        empty.names = dstd::make_shared_strings({"x"});
        empty.columns.push_back(strings({}));
        CHECK(round_trip(empty).num_rows() == 0);
        const dstd::StringView index[] = {"s", "i"};
        const DataFrame f = dftu_utils_test::ok(sample().with_index(index));
        const DataFrame back = round_trip(f);
        REQUIRE(back.index_names().size() == 2);
        CHECK(native(back) == native(f));
    }

    TEST_CASE("a bad header or buffer gives no frame") {
        const FrameBuffers out =
            dftu_utils_test::ok(frame_to_buffers(sample()));
        Released r;
        const auto got = receive(out, r);
        CHECK(frame_from_buffers(out.header, got));
        CHECK_FALSE(frame_from_buffers("", got));
        CHECK_FALSE(frame_from_buffers(
            out.header.substr(0, out.header.size() / 2), got));
        dstd::String other = out.header;
        other[4] = 9;
        CHECK_FALSE(frame_from_buffers(other, got));
        auto short_one = got;
        short_one[0].size -= 1;
        CHECK_FALSE(frame_from_buffers(out.header, short_one));
        auto fewer = got;
        fewer.pop_back();
        CHECK_FALSE(frame_from_buffers(out.header, fewer));
        dstd::String trailing = out.header;
        trailing += "x";
        CHECK_FALSE(frame_from_buffers(trailing, got));
    }

    TEST_CASE("a null buffer that claims bytes gives no frame") {
        const std::int64_t ints[] = {1, 2};
        DataFrame f;
        f.names = dstd::make_shared_strings({"i"});
        f.columns.push_back(Series::flat_i64(ints, 2));
        const FrameBuffers out = dftu_utils_test::ok(frame_to_buffers(f));
        dstd::Vector<ReceivedBuffer> got;
        got.push_back({nullptr, sizeof ints, true, nullptr});
        CHECK_FALSE(frame_from_buffers(out.header, got));
    }

    TEST_CASE("string offsets that leave their data give no frame") {
        DataFrame f;
        f.names = dstd::make_shared_strings({"s"});
        f.columns.push_back(strings({"ab", "c"}));
        const FrameBuffers out = dftu_utils_test::ok(frame_to_buffers(f));
        Released r;
        auto got = receive(out, r);
        const std::int32_t bad[] = {0, 5, 3};
        got[0] = {reinterpret_cast<const std::uint8_t*>(bad), sizeof bad, true,
                  nullptr};
        CHECK_FALSE(frame_from_buffers(out.header, got));
    }

    TEST_CASE("a row count whose buffer size wraps gives no frame") {
        // A one-letter name puts the column's length at bytes 29-36.
        const auto forged = [](Series column, std::int64_t n) {
            DataFrame f;
            f.names = dstd::make_shared_strings({"x"});
            f.columns.push_back(std::move(column));
            FrameBuffers out = dftu_utils_test::ok(frame_to_buffers(f));
            std::memcpy(out.header.data() + 8, &n, sizeof n);
            std::memcpy(out.header.data() + 29, &n, sizeof n);
            return out;
        };
        const std::int64_t none[] = {0};
        const FrameBuffers ints =
            forged(Series::flat_i64(none, 0), std::int64_t{1} << 61);
        Released r;
        CHECK_FALSE(frame_from_buffers(ints.header, receive(ints, r)));
        const FrameBuffers text = forged(strings({}), std::int64_t{1} << 62);
        CHECK_FALSE(frame_from_buffers(text.header, receive(text, r)));
    }

    TEST_CASE("offsets and validity of a buffer that can change are copied") {
        const std::int64_t ints[] = {1, 2, 3};
        const std::uint8_t valid[] = {0b101};
        DataFrame f;
        f.names = dstd::make_shared_strings({"i", "s"});
        f.columns.push_back(Series::flat_i64(ints, 3, valid));
        f.columns.push_back(strings({"ab", "c", "def"}));
        const FrameBuffers out = dftu_utils_test::ok(frame_to_buffers(f));
        Released r;
        const auto got = receive(out, &r, false);
        auto back = frame_from_buffers(out.header, got);
        REQUIRE(back);
        check_same(*back, f);
        const auto* i = back->columns[0].handle();
        const auto* s = back->columns[1].handle();
        const auto from = [&](const BufferView& b) {
            for (const ReceivedBuffer& g : got)
                if (b.data() == g.data) return true;
            return false;
        };
        CHECK(from(i->data));
        CHECK(from(s->data));
        CHECK_FALSE(from(i->validity));
        CHECK_FALSE(from(s->offsets));
    }

    TEST_CASE("an empty string slice at a later row round-trips") {
        DataFrame f;
        f.names = dstd::make_shared_strings({"s"});
        f.columns.push_back(strings({"a", "b"}).slice(1, 0));
        REQUIRE(f.columns[0].valid());
        const DataFrame back = round_trip(f);
        CHECK(back.num_rows() == 0);
    }

    TEST_CASE(
        "a time unit, decimal size or null count out of range gives no "
        "frame") {
        const auto refused = [](Series column) {
            DataFrame f;
            f.names = dstd::make_shared_strings({"x"});
            f.columns.push_back(std::move(column));
            const FrameBuffers out = dftu_utils_test::ok(frame_to_buffers(f));
            Released r;
            return !frame_from_buffers(out.header, receive(out, r));
        };
        const DataFrame f = sample();
        Series ts = f.columns[5].share();
        ts.handle()->set_time_unit(static_cast<TimeUnit>(99));
        CHECK(refused(std::move(ts)));
        Series dec = f.columns[6].share();
        dec.handle()->set_decimal(999, 999);
        CHECK(refused(std::move(dec)));
        Series zero = f.columns[6].share();
        zero.handle()->set_decimal(0, 0);
        CHECK(refused(std::move(zero)));
        Series nulls = f.columns[0].share();
        nulls.handle()->null_count = 2;
        CHECK(refused(std::move(nulls)));
        const std::int64_t ints[] = {1, 2};
        Series fine = Series::flat_i64(ints, 2);
        fine.handle()->type = TypeId::Time64;
        fine.handle()->set_time_unit(TimeUnit::Nano);
        CHECK_FALSE(refused(fine.share()));
        Series coarse = Series::flat_i64(ints, 2);
        coarse.handle()->type = TypeId::Time64;
        coarse.handle()->set_time_unit(TimeUnit::Second);
        CHECK(refused(std::move(coarse)));
        Series map =
            Series::list(dftu_utils_test::span_of<std::int32_t>({0, 1, 2}),
                         Series::flat_i64(ints, 2));
        map.handle()->type = TypeId::Map;
        CHECK(refused(std::move(map)));
    }
}
