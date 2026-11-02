#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/store/extent_size.h>
#include <doctest/doctest.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

namespace dstd = dftracer::utils::dstd;
namespace store = dftracer::utils::store;

constexpr std::uint32_t ONE_MIB = 1U << 20;
constexpr std::uint32_t FOUR_MIB = 4U << 20;
constexpr std::uint32_t SIXTEEN_MIB = 16U << 20;
constexpr std::uint32_t SIXTY_FOUR_MIB = 64U << 20;
constexpr std::uint32_t ONE_TWENTY_EIGHT_KIB = 128U << 10;

struct ExtentBytesOverride {
    explicit ExtentBytesOverride(const char* value) {
        ::setenv(
            dftracer::utils::common::constants::store::STORE_EXTENT_BYTES_ENV,
            value, 1);
    }
    ExtentBytesOverride(const ExtentBytesOverride&) = delete;
    ExtentBytesOverride& operator=(const ExtentBytesOverride&) = delete;
    ~ExtentBytesOverride() {
        ::unsetenv(
            dftracer::utils::common::constants::store::STORE_EXTENT_BYTES_ENV);
    }
};

struct TempDir {
    dftracer::utils::common::fs::path path;
    TempDir() {
        path = dftracer::utils::common::fs::temp_directory_path() /
               ("dftu_store_extent_size_" + std::to_string(::getpid()));
        dftracer::utils::common::fs::remove_all(path);
        dftracer::utils::common::fs::create_directories(path);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        dftracer::utils::common::fs::remove_all(path, ec);
    }
};

std::uint32_t chosen(std::size_t stripe_size,
                     dstd::Optional<std::uint64_t> override_bytes =
                         dstd::Optional<std::uint64_t>{}) {
    auto result =
        store::engine::choose_extent_size(stripe_size, override_bytes);
    REQUIRE(result.has_value());
    return *result;
}

void check_override_error(dstd::Expected<std::uint32_t, dstd::Error> result) {
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().is(dstd::Condition::InvalidArgument));
    CHECK(result.error().message().find(dftracer::utils::common::constants::
                                            store::STORE_EXTENT_BYTES_ENV) !=
          std::string_view::npos);
}

}  // namespace

TEST_CASE("store extent size: the default is 1 MiB") {
    CHECK(
        dftracer::utils::common::constants::store::STORE_DEFAULT_EXTENT_BYTES ==
        ONE_MIB);
    CHECK(dftracer::utils::common::constants::store::STORE_MIN_EXTENT_BYTES ==
          4096);
    CHECK(dftracer::utils::common::constants::store::STORE_MAX_EXTENT_BYTES ==
          SIXTEEN_MIB);
    CHECK(chosen(0) == ONE_MIB);
}

TEST_CASE("store extent size: a stripe size becomes the extent size") {
    CHECK(chosen(4096) == 4096);
    CHECK(chosen(ONE_MIB) == ONE_MIB);
    CHECK(chosen(FOUR_MIB) == FOUR_MIB);
    CHECK(chosen(SIXTEEN_MIB) == SIXTEEN_MIB);
}

TEST_CASE(
    "store extent size: a big stripe size is capped and a ragged one is "
    "rounded down") {
    CHECK(chosen(SIXTY_FOUR_MIB) == SIXTEEN_MIB);
    CHECK(chosen(ONE_MIB + 100) == ONE_MIB);
    CHECK(chosen(5000) == 4096);
    CHECK(chosen(1000) == 4096);
}

TEST_CASE("store extent size: an override wins over the stripe size") {
    CHECK(chosen(0, 4096) == 4096);
    CHECK(chosen(0, ONE_TWENTY_EIGHT_KIB) == ONE_TWENTY_EIGHT_KIB);
    CHECK(chosen(0, SIXTEEN_MIB) == SIXTEEN_MIB);
    CHECK(chosen(FOUR_MIB, 262144) == 262144);
}

TEST_CASE(
    "store extent size: an override off the 4 KiB grid or out of range is "
    "an error") {
    check_override_error(store::engine::choose_extent_size(0, 0));
    check_override_error(store::engine::choose_extent_size(0, 4095));
    check_override_error(store::engine::choose_extent_size(0, 5000));
    check_override_error(store::engine::choose_extent_size(0, 32U << 20U));
    check_override_error(
        store::engine::choose_extent_size(0, SIXTEEN_MIB + 4096));
}

TEST_CASE(
    "store extent size: a directory without a Lustre stripe uses the "
    "default") {
    TempDir dir;
    const std::size_t stripe =
        dftracer::utils::common::lustre_stripe(dir.path.string()).size;
    auto result = store::engine::extent_size_for(dir.path.string());
    REQUIRE(result.has_value());
    if (stripe == 0) {
        CHECK(*result == ONE_MIB);
    } else {
        CHECK(*result == chosen(stripe));
    }
}

TEST_CASE(
    "store extent size: DFTRACER_UTILS_STORE_EXTENT_BYTES overrides the "
    "directory") {
    TempDir dir;
    {
        ExtentBytesOverride override_value("262144");
        auto result = store::engine::extent_size_for(dir.path.string());
        REQUIRE(result.has_value());
        CHECK(*result == 262144);
    }
    {
        auto result = store::engine::extent_size_for(dir.path.string());
        REQUIRE(result.has_value());
        CHECK(*result ==
              chosen(dftracer::utils::common::lustre_stripe(dir.path.string())
                         .size));
    }
}

TEST_CASE(
    "store extent size: an unparsable or out-of-range override is an "
    "error") {
    TempDir dir;
    {
        ExtentBytesOverride override_value("abc");
        check_override_error(store::engine::extent_size_for(dir.path.string()));
    }
    {
        ExtentBytesOverride override_value("5000");
        check_override_error(store::engine::extent_size_for(dir.path.string()));
    }
}
