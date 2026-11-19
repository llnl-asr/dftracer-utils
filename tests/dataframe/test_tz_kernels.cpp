// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/string.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

// The expected values come from pandas 3.1 over the TZif files in data/tz
// (copied from the Python tzdata package, see data/tz/README).

namespace {

constexpr std::int64_t HOUR_US = 3'600'000'000;

struct UsePath {
    UsePath() {
        dftu_tz_set_search_path(
            DFTRACER_UTILS_TZ_DATA_DIR,
            static_cast<std::int32_t>(std::strlen(DFTRACER_UTILS_TZ_DATA_DIR)));
    }
};

Series stamps(const std::int64_t* us, std::int64_t n, dstd::StringView zone,
              const std::uint8_t* valid = nullptr) {
    Series s = Series::flat(TypeId::Int64, us, n, valid);
    s.handle()->type = TypeId::Timestamp;
    s.handle()->set_time_unit(TimeUnit::Micro);
    s.handle()->set_timezone(zone);
    return s;
}

const char* last_error_text(std::int32_t* kind) {
    const char* text = nullptr;
    *kind = dftu_tz_take_error(&text);
    return text;
}

}  // namespace

TEST_SUITE("dataframe/tz_kernels") {
    TEST_CASE("calendar parts of a zone-aware column are wall-clock ones") {
        UsePath path;
        // 2024-03-10 06:59:59 UTC is 01:59:59 EST; one second later the New
        // York clock reads 03:00:00 EDT.
        const std::int64_t base = 1'710'053'999'000'000;
        const std::int64_t us[3] = {base, base + 1'000'000,
                                    base + 1'000'000 + HOUR_US};
        Series ny = stamps(us, 3, "America/New_York");
        Series hour = ny.dt_part(DFTU_DT_HOUR, DFTU_TIME_UNIT_MICRO);
        REQUIRE(hour.valid());
        CHECK(hour.data<std::int64_t>()[0] == 1);
        CHECK(hour.data<std::int64_t>()[1] == 3);
        CHECK(hour.data<std::int64_t>()[2] == 4);
        Series utc = stamps(us, 3, "UTC");
        CHECK(utc.dt_part(DFTU_DT_HOUR, DFTU_TIME_UNIT_MICRO)
                  .data<std::int64_t>()[0] == 6);
        Series naive = stamps(us, 3, "");
        CHECK(naive.dt_part(DFTU_DT_HOUR, DFTU_TIME_UNIT_MICRO)
                  .data<std::int64_t>()[1] == 7);
        Series kolkata = stamps(us, 3, "Asia/Kolkata");
        CHECK(kolkata.dt_part(DFTU_DT_MINUTE, DFTU_TIME_UNIT_MICRO)
                  .data<std::int64_t>()[0] == 29);
    }

    TEST_CASE("an unknown zone is an error, not a UTC read") {
        UsePath path;
        const std::int64_t us[1] = {0};
        Series s = stamps(us, 1, "Nowhere/Land");
        std::int32_t kind = 0;
        dftu_tz_take_error(nullptr);
        CHECK_FALSE(s.dt_part(DFTU_DT_HOUR, DFTU_TIME_UNIT_MICRO).valid());
        last_error_text(&kind);
        CHECK(kind == DFTU_TZ_ERROR_UNKNOWN_ZONE);
        Series naive = stamps(us, 1, "");
        CHECK_FALSE(naive.with_timezone("Nowhere/Land").valid());
        last_error_text(&kind);
        CHECK(kind == DFTU_TZ_ERROR_UNKNOWN_ZONE);
    }

    TEST_CASE("date, time and strftime read the wall time") {
        UsePath path;
        // 2024-01-01 03:30:00 UTC is 2023-12-31 22:30:00 -05:00 in New York.
        const std::int64_t us[1] = {1'704'079'800'000'000};
        Series ny = stamps(us, 1, "America/New_York");
        Series date = ny.dt_date();
        REQUIRE(date.valid());
        CHECK(date.data<std::int32_t>()[0] == 19722);  // 2023-12-31
        Series time = ny.dt_time();
        REQUIRE(time.valid());
        CHECK(time.data<std::int64_t>()[0] ==
              (22 * 3600 + 30 * 60) * 1'000'000LL);
        Series text = ny.dt_format("%Y-%m-%d %H:%M %z %Z", TimeUnit::Micro);
        REQUIRE(text.valid());
        CHECK(text.string_at(0) == "2023-12-31 22:30 -0500 EST");
        Series kolkata = stamps(us, 1, "Asia/Kolkata");
        CHECK(kolkata.dt_format("%H:%M %z %Z", TimeUnit::Micro).string_at(0) ==
              "09:00 +0530 IST");
        Series utc = stamps(us, 1, "UTC");
        CHECK(utc.dt_format("%z %Z", TimeUnit::Micro).string_at(0) ==
              "+0000 UTC");
        // %s is the epoch seconds of the instant, whatever the zone.
        CHECK(ny.dt_format("%s", TimeUnit::Micro).string_at(0) == "1704079800");
    }

    TEST_CASE("rounding works on the wall time and maps back to UTC") {
        UsePath path;
        // 2024-03-10 12:00 UTC is 08:00 EDT the day the clocks went forward.
        const std::int64_t us[1] = {1'710'072'000'000'000};
        Series ny = stamps(us, 1, "America/New_York");
        const std::int64_t day = 24 * HOUR_US;
        Series floored = ny.dt_round(day, DFTU_DT_FLOOR);
        REQUIRE(floored.valid());
        // Midnight is 05:00 UTC (EST), so the day is 23 hours long.
        CHECK(floored.data<std::int64_t>()[0] == 1'710'046'800'000'000);
        Series ceiled = ny.dt_round(day, DFTU_DT_CEIL);
        REQUIRE(ceiled.valid());
        CHECK(ceiled.data<std::int64_t>()[0] ==
              1'710'046'800'000'000 + 23 * HOUR_US);
        // Half past midnight rounds, in Kolkata, on its +05:30 wall time.
        const std::int64_t half[1] = {1'710'072'000'000'000 -
                                      30 * 60'000'000LL};
        Series kolkata = stamps(half, 1, "Asia/Kolkata");
        Series hour = kolkata.dt_round(HOUR_US, DFTU_DT_FLOOR);
        REQUIRE(hour.valid());
        CHECK(hour.data<std::int64_t>()[0] % HOUR_US == 30 * 60'000'000LL);
    }

    TEST_CASE("rounding into a repeated wall time fails, as pandas") {
        UsePath path;
        // 2024-11-03 05:10 UTC is 01:10 EDT, inside the repeated hour.
        const std::int64_t us[1] = {1'730'610'600'000'000};
        Series ny = stamps(us, 1, "America/New_York");
        dftu_tz_take_error(nullptr);
        CHECK_FALSE(ny.dt_round(HOUR_US, DFTU_DT_FLOOR).valid());
        std::int32_t kind = 0;
        last_error_text(&kind);
        CHECK(kind == DFTU_TZ_ERROR_AMBIGUOUS);
        CHECK(ny.dt_round(24 * HOUR_US, DFTU_DT_FLOOR).valid());
    }

    TEST_CASE("tz_localize turns wall times into instants") {
        UsePath path;
        // 2024-11-03 01:30 wall time happens twice in New York.
        const std::int64_t wall[3] = {1'730'597'400'000'000,
                                      1'730'597'400'000'000 + 24 * HOUR_US,
                                      1'710'037'800'000'000};
        // Row 2 is 2024-03-10 02:30, which never happens.
        Series naive = stamps(wall, 3, "");
        std::int32_t kind = 0;
        dftu_tz_take_error(nullptr);
        CHECK_FALSE(naive
                        .tz_localize("America/New_York",
                                     DFTU_TZ_AMBIGUOUS_RAISE,
                                     DFTU_TZ_NONEXISTENT_RAISE)
                        .valid());
        last_error_text(&kind);
        CHECK(kind == DFTU_TZ_ERROR_AMBIGUOUS);

        Series first =
            naive.tz_localize("America/New_York", DFTU_TZ_AMBIGUOUS_EARLIEST,
                              DFTU_TZ_NONEXISTENT_SHIFT_FORWARD);
        REQUIRE(first.valid());
        CHECK(first.handle()->timezone() == "America/New_York");
        CHECK(first.data<std::int64_t>()[0] ==
              1'730'597'400'000'000 + 4 * HOUR_US);
        Series second =
            naive.tz_localize("America/New_York", DFTU_TZ_AMBIGUOUS_LATEST,
                              DFTU_TZ_NONEXISTENT_SHIFT_FORWARD);
        CHECK(second.data<std::int64_t>()[0] ==
              1'730'597'400'000'000 + 5 * HOUR_US);
        // The skipped time moves to the first instant after the gap (03:00
        // EDT).
        CHECK(first.data<std::int64_t>()[2] == 1'710'054'000'000'000);
        Series back =
            naive.tz_localize("America/New_York", DFTU_TZ_AMBIGUOUS_EARLIEST,
                              DFTU_TZ_NONEXISTENT_SHIFT_BACKWARD);
        CHECK(back.data<std::int64_t>()[2] == 1'710'054'000'000'000 - 1);
        Series nat = naive.tz_localize(
            "America/New_York", DFTU_TZ_AMBIGUOUS_NAT, DFTU_TZ_NONEXISTENT_NAT);
        REQUIRE(nat.valid());
        CHECK(nat.is_null(0));
        CHECK_FALSE(nat.is_null(1));
        CHECK(nat.is_null(2));
        CHECK(nat.null_count() == 2);
        // 02:30 plus one hour, read at the offset after the gap.
        Series shifted =
            naive.tz_localize("America/New_York", DFTU_TZ_AMBIGUOUS_EARLIEST,
                              DFTU_TZ_NONEXISTENT_SHIFT, HOUR_US);
        REQUIRE(shifted.valid());
        CHECK(shifted.data<std::int64_t>()[2] ==
              1'710'054'000'000'000 + 30 * 60'000'000LL);
    }

    TEST_CASE("tz_localize round trips and refuses what it cannot read") {
        UsePath path;
        const std::int64_t us[2] = {1'704'079'800'000'000, 5};
        Series ny = stamps(us, 2, "America/New_York");
        Series walls = ny.tz_localize("", DFTU_TZ_AMBIGUOUS_RAISE,
                                      DFTU_TZ_NONEXISTENT_RAISE);
        REQUIRE(walls.valid());
        CHECK(walls.handle()->timezone().empty());
        CHECK(walls.data<std::int64_t>()[0] == us[0] - 5 * HOUR_US);
        Series again =
            walls.tz_localize("America/New_York", DFTU_TZ_AMBIGUOUS_RAISE,
                              DFTU_TZ_NONEXISTENT_RAISE);
        REQUIRE(again.valid());
        CHECK(again.data<std::int64_t>()[0] == us[0]);
        // An aware column cannot take a second zone.
        CHECK_FALSE(ny.tz_localize("Europe/London", 0, 0).valid());
        CHECK_FALSE(walls.tz_localize("Nowhere/Land", 0, 0).valid());
        CHECK_FALSE(walls.tz_localize("Europe/London", 9, 0).valid());
        dftu_tz_take_error(nullptr);
    }

    TEST_CASE("naive and UTC columns keep their value, a null stays null") {
        UsePath path;
        const std::int64_t us[3] = {1'704'079'800'000'000, 0, 7};
        const std::uint8_t valid = 0b101;
        Series ny = stamps(us, 3, "America/New_York", &valid);
        Series hour = ny.dt_part(DFTU_DT_HOUR, DFTU_TIME_UNIT_MICRO);
        REQUIRE(hour.valid());
        CHECK(hour.is_null(1));
        CHECK(hour.data<std::int64_t>()[0] == 22);
        Series text = ny.dt_format("%H", TimeUnit::Micro);
        CHECK(text.is_null(1));
        CHECK(text.string_at(0) == "22");
    }

    TEST_CASE("each kind of failure is reported to the caller") {
        UsePath path;
        std::int32_t kind = 0;
        // 2024-03-10 02:30 never happens in New York.
        const std::int64_t gap[1] = {1'710'037'800'000'000};
        Series naive = stamps(gap, 1, "");
        dftu_tz_take_error(nullptr);
        CHECK_FALSE(naive
                        .tz_localize("America/New_York",
                                     DFTU_TZ_AMBIGUOUS_RAISE,
                                     DFTU_TZ_NONEXISTENT_RAISE)
                        .valid());
        last_error_text(&kind);
        CHECK(kind == DFTU_TZ_ERROR_NONEXISTENT);

        CHECK_FALSE(naive.with_timezone("../etc/passwd").valid());
        last_error_text(&kind);
        CHECK(kind == DFTU_TZ_ERROR_INVALID_ZONE);

        // The wall time of the last instant does not fit Int64 east of UTC.
        const std::int64_t last[1] = {std::numeric_limits<std::int64_t>::max()};
        Series edge = stamps(last, 1, "Asia/Kolkata");
        CHECK_FALSE(edge.dt_part(DFTU_DT_HOUR, DFTU_TIME_UNIT_MICRO).valid());
        const char* text = nullptr;
        kind = dftu_tz_take_error(&text);
        CHECK(kind == DFTU_TZ_ERROR_OUT_OF_RANGE);
        CHECK(std::strlen(text) > 0);
        // The failure is taken once.
        CHECK(dftu_tz_take_error(nullptr) == DFTU_TZ_ERROR_NONE);
    }

    TEST_CASE("a failure that nobody took does not read as a later call's") {
        UsePath path;
        const std::int64_t gap[1] = {1'710'037'800'000'000};
        const std::int64_t fine[1] = {1'710'041'400'000'000};
        Series bad = stamps(gap, 1, "");
        Series good = stamps(fine, 1, "");
        CHECK_FALSE(bad.tz_localize("America/New_York", DFTU_TZ_AMBIGUOUS_RAISE,
                                    DFTU_TZ_NONEXISTENT_RAISE)
                        .valid());
        CHECK(good.tz_localize("America/New_York", DFTU_TZ_AMBIGUOUS_RAISE,
                               DFTU_TZ_NONEXISTENT_RAISE)
                  .valid());
        CHECK(dftu_tz_take_error(nullptr) == DFTU_TZ_ERROR_NONE);

        CHECK_FALSE(bad.with_timezone("Nowhere/Land").valid());
        Series ny = stamps(fine, 1, "America/New_York");
        CHECK(ny.dt_part(DFTU_DT_HOUR, DFTU_TIME_UNIT_MICRO).valid());
        CHECK(dftu_tz_take_error(nullptr) == DFTU_TZ_ERROR_NONE);
    }

    TEST_CASE("a packed shift that is not a whole number of ticks is refused") {
        UsePath path;
        const std::int64_t gap[1] = {1'710'037'800'000'000};
        Series naive = stamps(gap, 1, "");
        const std::int64_t options =
            DFTU_TZ_AMBIGUOUS_RAISE |
            (static_cast<std::int64_t>(DFTU_TZ_NONEXISTENT_SHIFT) << 8) |
            ((3'600'000'000'000 + 1'500) << 16);
        dftu_tz_take_error(nullptr);
        CHECK(dftu_series_tz_localize_opts(naive.handle(), "America/New_York",
                                           16, options) == nullptr);
        CHECK(dftu_tz_take_error(nullptr) == DFTU_TZ_ERROR_OTHER);
        const std::int64_t whole = options - (1'500 << 16);
        dftu_series* shifted = dftu_series_tz_localize_opts(
            naive.handle(), "America/New_York", 16, whole);
        REQUIRE(shifted != nullptr);
        CHECK(dftu_series_length(shifted) == 1);
        dftu_series_free(shifted);
    }
}
