// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/extensions/kinds/payloads.h>
#include <dftracer/utils/trace/index/extensions/zone_bounds.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index::extensions;

namespace {

ZoneLiteral num(std::int64_t v) {
    return ZoneLiteral::from_number(duql::Number{v});
}
ZoneLiteral num(double v) { return ZoneLiteral::from_number(duql::Number{v}); }
ZoneLiteral num(std::uint64_t v) {
    return ZoneLiteral::from_number(duql::Number{v});
}
ZoneLiteral text(dstd::StringView v) {
    return ZoneLiteral::from_text(dstd::String(v));
}

}  // namespace

TEST_SUITE("ZoneBounds") {
    TEST_CASE("null and NaN are counted and never a bound") {
        ZoneBounds z;
        z.add(1.0);
        z.add(std::nan(""));
        z.add(2.5e-7);
        z.add_nulls(3);
        z.add(std::nan(""));
        CHECK(z.type() == BoundType::Double);
        CHECK(z.value_count() == 2);
        CHECK(z.nan_count() == 2);
        CHECK(z.null_count() == 3);
        CHECK(z.min_double() == 2.5e-7);
        CHECK(z.max_double() == 1.0);
        CHECK(z.exact());

        ZoneBounds only_nan;
        only_nan.add(std::nan(""));
        CHECK(only_nan.type() == BoundType::None);
        CHECK_FALSE(only_nan.has_bounds());
        CHECK(only_nan.nan_count() == 1);
    }

    TEST_CASE("a double bound is the real value, never rounded") {
        ZoneBounds z;
        z.add(1e-7);
        z.add(2e-7);
        CHECK(z.min_double() == 1e-7);
        CHECK(z.max_double() == 2e-7);
        CHECK(z.may_match(ZoneOp::Gt, num(1.5e-7)));
        CHECK_FALSE(z.may_match(ZoneOp::Gt, num(2e-7)));
        CHECK(z.may_match(ZoneOp::Lt, num(1.5e-7)));
        CHECK_FALSE(z.may_match(ZoneOp::Lt, num(1e-7)));
    }

    TEST_CASE("strings compare as unsigned bytes") {
        ZoneBounds z;
        z.add(dstd::StringView("a"));
        z.add(dstd::StringView("\xff"));
        CHECK(z.type() == BoundType::String);
        CHECK(z.min_string() == "a");
        CHECK(z.max_string() == "\xff");
        CHECK(z.may_match(ZoneOp::Gt, text("b")));
        CHECK_FALSE(z.may_match(ZoneOp::Gt, text("\xff")));
        CHECK(z.may_equal(text("\xfe")));
        CHECK_FALSE(z.may_equal(text("\x01")));
        ZoneBounds prefix;
        prefix.add(dstd::StringView("ab"));
        prefix.add(dstd::StringView("a"));
        CHECK(prefix.min_string() == "a");
        CHECK(prefix.max_string() == "ab");
    }

    TEST_CASE("a mixed int and double path prunes by exact numbers") {
        ZoneBounds z;
        z.add(std::int64_t{1});
        z.add(std::int64_t{3});
        z.add(2.5);
        CHECK(z.type() == BoundType::Double);
        CHECK(z.exact());
        CHECK(z.min_double() == 1.0);
        CHECK(z.max_double() == 3.0);
        CHECK_FALSE(z.may_match(ZoneOp::Gt, num(std::int64_t{3})));
        CHECK(z.may_match(ZoneOp::Ge, num(std::int64_t{3})));
        CHECK(z.may_match(ZoneOp::Gt, num(2.9)));
        CHECK_FALSE(z.may_match(ZoneOp::Lt, num(1.0)));
        CHECK(z.may_equal(num(2.5)));
        CHECK(z.may_equal(num(std::int64_t{2})));
        CHECK_FALSE(z.may_equal(num(std::int64_t{4})));
        CHECK_FALSE(z.may_equal(num(0.5)));
        CHECK(z.all_match(ZoneOp::Ge, num(1.0)));
        CHECK_FALSE(z.all_match(ZoneOp::Gt, num(1.0)));
    }

    TEST_CASE("an integer a double cannot hold makes the bound inexact") {
        // 2^63 + 1 is not a double, so the bound is rounded: it must not
        // prune the value 2^63 + 1 itself.
        const std::uint64_t big = (std::uint64_t{1} << 63) + 1;
        ZoneBounds z;
        z.add(big);
        z.add(1.0);
        CHECK(z.type() == BoundType::Double);
        CHECK_FALSE(z.max_exact());
        CHECK(z.min_exact());
        CHECK(z.may_match(ZoneOp::Gt, num(big)));
        CHECK(z.may_match(ZoneOp::Ge, num(big)));
        CHECK(z.may_equal(num(big)));
        CHECK_FALSE(
            z.all_match(ZoneOp::Lt, num(std::numeric_limits<double>::max())));
        // The exact lower bound still proves absence below it.
        CHECK_FALSE(z.may_match(ZoneOp::Lt, num(1.0)));
        CHECK_FALSE(z.may_equal(num(0.5)));
    }

    TEST_CASE("a zone with nulls or NaNs proves no all-match") {
        ZoneBounds z;
        z.add(std::int64_t{5});
        z.add(std::int64_t{9});
        CHECK(z.all_match(ZoneOp::Gt, num(std::int64_t{4})));
        ZoneBounds with_null = z;
        with_null.add_nulls(1);
        CHECK_FALSE(with_null.all_match(ZoneOp::Gt, num(std::int64_t{4})));
        ZoneBounds with_nan = z;
        with_nan.add(std::nan(""));
        CHECK_FALSE(with_nan.all_match(ZoneOp::Gt, num(std::int64_t{4})));
        // A NaN literal compares with nothing.
        CHECK(z.may_match(ZoneOp::Gt, num(std::nan(""))));
        CHECK(z.may_equal(num(std::nan(""))));
    }

    TEST_CASE("numbers with strings have no bounds") {
        ZoneBounds z;
        z.add(std::int64_t{1});
        z.add(dstd::StringView("x"));
        CHECK(z.type() == BoundType::Mixed);
        CHECK_FALSE(z.has_bounds());
        CHECK(z.may_match(ZoneOp::Gt, num(std::int64_t{100})));
        CHECK(z.may_equal(text("zzz")));
        z.add(std::int64_t{7});
        CHECK(z.type() == BoundType::Mixed);
        CHECK(z.value_count() == 3);
    }

    TEST_CASE("integers of both signs stay integers while they fit") {
        ZoneBounds z;
        z.add(std::uint64_t{5});
        z.add(std::int64_t{-3});
        CHECK(z.type() == BoundType::Int64);
        CHECK(z.exact());
        CHECK(z.min_is_negative());
        z.add(std::uint64_t{std::numeric_limits<std::uint64_t>::max()});
        CHECK(z.type() == BoundType::Double);
        CHECK_FALSE(z.max_exact());
    }

    TEST_CASE("an int64 zone takes a uint64 below its minimum") {
        ZoneBounds z;
        z.add(std::int64_t{10});
        z.add(std::int64_t{20});
        z.add(std::uint64_t{5});
        CHECK(z.type() == BoundType::Int64);
        CHECK(z.min_double() == 5.0);
        CHECK(z.max_double() == 20.0);
        CHECK_FALSE(z.may_match(ZoneOp::Lt, num(std::int64_t{5})));
        CHECK(z.may_match(ZoneOp::Lt, num(std::int64_t{8})));
    }

    TEST_CASE("int64 and uint64 zones keep both bounds in either order") {
        for (int order = 0; order < 2; ++order) {
            for (int how = 0; how < 2; ++how) {
                CAPTURE(order);
                CAPTURE(how);
                ZoneBounds ints;
                ints.add(std::int64_t{10});
                ints.add(std::int64_t{20});
                ZoneBounds uints;
                uints.add(std::uint64_t{5});
                uints.add(std::uint64_t{30});
                ZoneBounds z;
                if (how == 0) {
                    // Through add, value by value.
                    const ZoneBounds& first = order == 0 ? ints : uints;
                    const ZoneBounds& second = order == 0 ? uints : ints;
                    for (const ZoneBounds* part : {&first, &second}) {
                        if (part == &ints) {
                            z.add(std::int64_t{10});
                            z.add(std::int64_t{20});
                        } else {
                            z.add(std::uint64_t{5});
                            z.add(std::uint64_t{30});
                        }
                    }
                } else {
                    z = order == 0 ? ints : uints;
                    z.merge_from(order == 0 ? uints : ints);
                }
                CHECK(z.min_double() == 5.0);
                CHECK(z.max_double() == 30.0);
                CHECK(z.exact());
                CHECK_FALSE(z.may_match(ZoneOp::Lt, num(std::int64_t{5})));
                CHECK_FALSE(z.may_match(ZoneOp::Gt, num(std::int64_t{30})));
                CHECK(z.may_match(ZoneOp::Lt, num(std::int64_t{8})));
            }
        }
    }

    TEST_CASE("merging unions the bounds and adds the counts") {
        ZoneBounds a;
        a.add(std::int64_t{10});
        a.add_nulls(1);
        ZoneBounds b;
        b.add(2.5);
        b.add(std::nan(""));
        a.merge_from(b);
        CHECK(a.type() == BoundType::Double);
        CHECK(a.min_double() == 2.5);
        CHECK(a.max_double() == 10.0);
        CHECK(a.value_count() == 2);
        CHECK(a.null_count() == 1);
        CHECK(a.nan_count() == 1);
        ZoneBounds s;
        s.add(dstd::StringView("x"));
        a.merge_from(s);
        CHECK(a.type() == BoundType::Mixed);
    }
}

TEST_SUITE("zone payload") {
    TEST_CASE("counts, exact flags and bounds survive the payload") {
        kinds::Zone zone;
        zone.add(std::uint64_t{(std::uint64_t{1} << 63) + 1});
        zone.add(1.0);
        zone.add(std::nan(""));
        zone.add_nulls(4);
        zone.observed = 20;
        zone.present = 3;
        const auto decoded = kinds::decode_zone(kinds::encode_zone(zone));
        REQUIRE(decoded.has_value());
        CHECK(decoded->type() == BoundType::Double);
        CHECK(decoded->min_double() == 1.0);
        CHECK(decoded->max_double() == zone.max_double());
        CHECK(decoded->min_exact());
        CHECK_FALSE(decoded->max_exact());
        CHECK(decoded->nan_count() == 1);
        CHECK(decoded->null_count() == 4);
        CHECK(decoded->observed == 20);
        CHECK(decoded->present == 3);

        kinds::Zone strings;
        strings.add(dstd::StringView("a"));
        strings.add(dstd::StringView("\xff\xfe"));
        const auto s = kinds::decode_zone(kinds::encode_zone(strings));
        REQUIRE(s.has_value());
        CHECK(s->min_string() == "a");
        CHECK(s->max_string() == "\xff\xfe");
    }

    TEST_CASE("a NaN bound or reversed bounds are not read") {
        kinds::Zone nan;
        nan.assign(BoundType::Double, 2, 0, 0, true, true);
        nan.set_min(std::nan(""));
        nan.set_max(1.0);
        CHECK_FALSE(kinds::decode_zone(kinds::encode_zone(nan)).has_value());
        kinds::Zone reversed;
        reversed.assign(BoundType::UInt64, 2, 0, 0, true, true);
        reversed.set_min(std::uint64_t{9});
        reversed.set_max(std::uint64_t{3});
        CHECK_FALSE(
            kinds::decode_zone(kinds::encode_zone(reversed)).has_value());
        kinds::Zone strings;
        strings.assign(BoundType::String, 2, 0, 0, true, true);
        strings.set_min(dstd::String("\xff"));
        strings.set_max(dstd::String("a"));
        CHECK_FALSE(
            kinds::decode_zone(kinds::encode_zone(strings)).has_value());
    }
}
