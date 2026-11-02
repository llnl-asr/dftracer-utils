#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <doctest/doctest.h>

TEST_SUITE("str_format::parse_bytes") {
    TEST_CASE("bare number is bytes") {
        CHECK(*dftracer::utils::common::parse_bytes("0") == 0);
        CHECK(*dftracer::utils::common::parse_bytes("512") == 512);
        CHECK(*dftracer::utils::common::parse_bytes("1073741824") ==
              1073741824ull);
    }

    TEST_CASE("byte units are always 1024-based; KB == KiB") {
        CHECK(*dftracer::utils::common::parse_bytes("1KB") == 1024);
        CHECK(*dftracer::utils::common::parse_bytes("1KiB") == 1024);
        CHECK(*dftracer::utils::common::parse_bytes("64KB") == 64ull * 1024);
        CHECK(*dftracer::utils::common::parse_bytes("64KiB") == 64ull * 1024);
        CHECK(*dftracer::utils::common::parse_bytes("2MB") ==
              2ull * 1024 * 1024);
        CHECK(*dftracer::utils::common::parse_bytes("1GB") ==
              1024ull * 1024 * 1024);
        CHECK(*dftracer::utils::common::parse_bytes("1TB") ==
              1024ull * 1024 * 1024 * 1024);
        CHECK(*dftracer::utils::common::parse_bytes("1PB") ==
              1024ull * 1024 * 1024 * 1024 * 1024);
    }

    TEST_CASE("lowercase b is bits, divided by 8") {
        CHECK(*dftracer::utils::common::parse_bytes("8b") == 1);
        CHECK(*dftracer::utils::common::parse_bytes("8B") == 8);
        CHECK(*dftracer::utils::common::parse_bytes("8kb") ==
              1024);  // 8 * 1024 bits / 8
        CHECK(*dftracer::utils::common::parse_bytes("8Kib") ==
              1024);  // 'i' ignored
        CHECK(*dftracer::utils::common::parse_bytes("1Mb") ==
              1024ull * 1024 / 8);
    }

    TEST_CASE("prefix magnitude is case-insensitive, unit char is not") {
        CHECK(*dftracer::utils::common::parse_bytes("1kB") ==
              *dftracer::utils::common::parse_bytes("1KB"));
        CHECK(*dftracer::utils::common::parse_bytes("1kb") ==
              *dftracer::utils::common::parse_bytes("1KB") / 8);
    }

    TEST_CASE("fractional values") {
        CHECK(*dftracer::utils::common::parse_bytes("1.5KiB") == 1536);
        CHECK(*dftracer::utils::common::parse_bytes("1.5KB") == 1536);
        CHECK(*dftracer::utils::common::parse_bytes("0.5MiB") == 512ull * 1024);
    }

    TEST_CASE("surrounding and interior whitespace") {
        CHECK(*dftracer::utils::common::parse_bytes("  4MiB  ") ==
              4ull * 1024 * 1024);
        CHECK(*dftracer::utils::common::parse_bytes("4 MiB") ==
              4ull * 1024 * 1024);
    }

    TEST_CASE("malformed input returns nullopt") {
        CHECK_FALSE(dftracer::utils::common::parse_bytes("").has_value());
        CHECK_FALSE(dftracer::utils::common::parse_bytes("   ").has_value());
        CHECK_FALSE(dftracer::utils::common::parse_bytes("abc").has_value());
        CHECK_FALSE(dftracer::utils::common::parse_bytes("12xb").has_value());
        CHECK_FALSE(dftracer::utils::common::parse_bytes("5zz").has_value());
        CHECK_FALSE(dftracer::utils::common::parse_bytes("-4KB").has_value());
    }
}

TEST_SUITE("str_format::parse_duration_seconds") {
    TEST_CASE("bare number is seconds") {
        CHECK(*dftracer::utils::common::parse_duration_seconds("0") ==
              doctest::Approx(0.0));
        CHECK(*dftracer::utils::common::parse_duration_seconds("30") ==
              doctest::Approx(30.0));
    }

    TEST_CASE("named units") {
        CHECK(*dftracer::utils::common::parse_duration_seconds("45s") ==
              doctest::Approx(45.0));
        CHECK(*dftracer::utils::common::parse_duration_seconds("5m") ==
              doctest::Approx(300.0));
        CHECK(*dftracer::utils::common::parse_duration_seconds("2h") ==
              doctest::Approx(7200.0));
        CHECK(*dftracer::utils::common::parse_duration_seconds("1d") ==
              doctest::Approx(86400.0));
    }

    TEST_CASE("sub-second units") {
        CHECK(*dftracer::utils::common::parse_duration_seconds("500ms") ==
              doctest::Approx(0.5));
        CHECK(*dftracer::utils::common::parse_duration_seconds("1000us") ==
              doctest::Approx(1e-3));
        CHECK(*dftracer::utils::common::parse_duration_seconds("1000000ns") ==
              doctest::Approx(1e-3));
    }

    TEST_CASE("unit aliases and case insensitivity") {
        CHECK(*dftracer::utils::common::parse_duration_seconds("5min") ==
              doctest::Approx(300.0));
        CHECK(*dftracer::utils::common::parse_duration_seconds("2hr") ==
              doctest::Approx(7200.0));
        CHECK(*dftracer::utils::common::parse_duration_seconds("90SEC") ==
              doctest::Approx(90.0));
        CHECK(*dftracer::utils::common::parse_duration_seconds("1.5H") ==
              doctest::Approx(5400.0));
    }

    TEST_CASE("fractional and whitespace") {
        CHECK(*dftracer::utils::common::parse_duration_seconds("1.5h") ==
              doctest::Approx(5400.0));
        CHECK(*dftracer::utils::common::parse_duration_seconds("  2m ") ==
              doctest::Approx(120.0));
    }

    TEST_CASE("malformed input returns nullopt") {
        CHECK_FALSE(
            dftracer::utils::common::parse_duration_seconds("").has_value());
        CHECK_FALSE(
            dftracer::utils::common::parse_duration_seconds("abc").has_value());
        CHECK_FALSE(
            dftracer::utils::common::parse_duration_seconds("10q").has_value());
        CHECK_FALSE(
            dftracer::utils::common::parse_duration_seconds("-5s").has_value());
    }
}
