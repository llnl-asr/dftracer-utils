#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/memory_config.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/memory/settings.h>
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../memory/fresh_process.h"

namespace dut = dftracer::utils;
namespace mem = dftracer::utils::memory;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;
constexpr const char* PREFIX = "dftracer-utils memory: peak=";

bool read_peak(const std::string& output, std::size_t& peak) {
    std::size_t at = output.find(PREFIX);
    if (at == std::string::npos) return false;
    unsigned long long value = 0;
    if (std::sscanf(output.c_str() + at, "dftracer-utils memory: peak=%llu",
                    &value) != 1) {
        return false;
    }
    peak = static_cast<std::size_t>(value);
    return true;
}

}  // namespace

TEST_CASE("child: hold a block and exit" * doctest::skip()) {
    fresh_process::announce();
    REQUIRE(dut::configure_memory_from_env());
    constexpr std::size_t BYTES = 64 * MIB;
    auto* block = static_cast<char*>(mem::allocate(BYTES, 64));
    REQUIRE(block != nullptr);
    for (std::size_t i = 0; i < BYTES; i += 4096) block[i] = 1;
    CHECK(mem::Manager::get().stats().live >= BYTES);
    mem::deallocate(block, BYTES);
}

TEST_CASE("the report shows a peak at least as large as a block held") {
    ::setenv(dut::constants::memory_settings::REPORT_ENV, "1", 1);
    auto result = fresh_process::run("child: hold a block and exit");
    ::unsetenv(dut::constants::memory_settings::REPORT_ENV);
    REQUIRE(result.ok());
    std::size_t peak = 0;
    REQUIRE(read_peak(result.output, peak));
    CHECK(peak >= 64 * MIB);
    CHECK(result.output.find(" live=") != std::string::npos);
    CHECK(result.output.find(" measured=") != std::string::npos);
    CHECK(result.output.find(" limit=") != std::string::npos);
    std::size_t at = result.output.find(" heap_peak=");
    REQUIRE(at != std::string::npos);
    unsigned long long heap_peak = 0;
    REQUIRE(std::sscanf(result.output.c_str() + at, " heap_peak=%llu",
                        &heap_peak) == 1);
    if (mem::active_backend() == mem::Backend::PageHeap) {
        CHECK(heap_peak >= 64 * MIB);
    }
}

TEST_CASE("no report without the variable") {
    ::unsetenv(dut::constants::memory_settings::REPORT_ENV);
    auto result = fresh_process::run("child: hold a block and exit");
    REQUIRE(result.ok());
    CHECK(result.output.find("dftracer-utils memory:") == std::string::npos);
}
