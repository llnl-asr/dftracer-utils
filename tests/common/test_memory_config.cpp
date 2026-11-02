// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/memory_config.h>
#include <dftracer/utils/memory/constants.h>
#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace dut = dftracer::utils;
namespace mem = dftracer::utils::memory;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;

struct EnvVar {
    EnvVar(const char* var, const char* value) : name(var) {
        ::setenv(var, value, 1);
    }
    ~EnvVar() { ::unsetenv(name); }
    const char* name;
};

void clear_memory_env() {
    for (const char* name :
         {dftracer::utils::common::constants::memory_settings::BUDGET_ENV,
          dftracer::utils::common::constants::memory_settings::HARD_LIMIT_ENV,
          dftracer::utils::common::constants::memory_settings::
              LIMIT_HEADROOM_ENV,
          dftracer::utils::common::constants::memory_settings::
              SPILL_HEADROOM_ENV}) {
        ::unsetenv(name);
    }
}

}  // namespace

TEST_CASE("the common and memory constant sets agree") {
    CHECK(std::strcmp(dftracer::utils::common::constants::memory_settings::
                          MEMORY_BACKEND_ENV,
                      mem::constants::MEMORY_BACKEND_ENV) == 0);
    CHECK(std::strcmp(
              dftracer::utils::common::constants::memory_settings::DECAY_MS_ENV,
              mem::constants::DECAY_MS_ENV) == 0);
    CHECK(std::strcmp(
              dftracer::utils::common::constants::memory_settings::BUDGET_ENV,
              mem::constants::BUDGET_ENV) == 0);
    CHECK(
        std::strcmp(
            dftracer::utils::common::constants::memory_settings::HARD_LIMIT_ENV,
            mem::constants::HARD_LIMIT_ENV) == 0);
    CHECK(std::strcmp(dftracer::utils::common::constants::memory_settings::
                          LIMIT_HEADROOM_ENV,
                      mem::constants::LIMIT_HEADROOM_ENV) == 0);
    CHECK(std::strcmp(dftracer::utils::common::constants::memory_settings::
                          SPILL_HEADROOM_ENV,
                      mem::constants::SPILL_HEADROOM_ENV) == 0);
    CHECK(std::strcmp(
              dftracer::utils::common::constants::memory_settings::DEBUG_ENV,
              mem::constants::DEBUG_ENV) == 0);
    CHECK(
        std::strcmp(
            dftracer::utils::common::constants::memory_settings::EXIT_CHECK_ENV,
            mem::constants::EXIT_CHECK_ENV) == 0);

    CHECK(
        dftracer::utils::common::constants::memory_settings::DEFAULT_DECAY_MS ==
        mem::constants::DEFAULT_DECAY_MS);
    CHECK(dftracer::utils::common::constants::memory_settings::
              DEFAULT_BUDGET_BYTES == mem::constants::DEFAULT_BUDGET_BYTES);
    CHECK(dftracer::utils::common::constants::memory_settings::
              DEFAULT_HARD_LIMIT_BYTES ==
          mem::constants::DEFAULT_HARD_LIMIT_BYTES);
    CHECK(dftracer::utils::common::constants::memory_settings::
              LIMIT_HEADROOM_PERCENT == mem::constants::LIMIT_HEADROOM_PERCENT);
    CHECK(dftracer::utils::common::constants::memory_settings::
              LIMIT_HEADROOM_MIN_BYTES ==
          mem::constants::LIMIT_HEADROOM_MIN_BYTES);
    CHECK(dftracer::utils::common::constants::memory_settings::
              SPILL_HEADROOM_PERCENT == mem::constants::SPILL_HEADROOM_PERCENT);
    CHECK(dftracer::utils::common::constants::memory_settings::
              SPILL_HEADROOM_MIN_BYTES ==
          mem::constants::SPILL_HEADROOM_MIN_BYTES);
    CHECK(dftracer::utils::common::constants::memory_settings::DEFAULT_DEBUG ==
          mem::constants::DEFAULT_DEBUG);
    CHECK(dftracer::utils::common::constants::memory_settings::
              DEFAULT_EXIT_CHECK == mem::constants::DEFAULT_EXIT_CHECK);
}

TEST_CASE("unset variables leave every field at its default") {
    clear_memory_env();
    auto config = dut::common::memory_config_from_env();
    REQUIRE(config);
    CHECK(config->budget == 0);
    CHECK(config->hard_cap == 0);
    CHECK(config->limit_headroom == 0);
    CHECK(config->spill_headroom == 0);
}

TEST_CASE("values read as bytes or with a unit") {
    clear_memory_env();
    EnvVar budget(
        dftracer::utils::common::constants::memory_settings::BUDGET_ENV,
        "67108864");
    EnvVar cap(
        dftracer::utils::common::constants::memory_settings::HARD_LIMIT_ENV,
        "1GiB");
    EnvVar limit(
        dftracer::utils::common::constants::memory_settings::LIMIT_HEADROOM_ENV,
        "300MB");
    EnvVar spill(
        dftracer::utils::common::constants::memory_settings::SPILL_HEADROOM_ENV,
        "5MB");
    auto config = dut::common::memory_config_from_env();
    REQUIRE(config);
    CHECK(config->budget == 64 * MIB);
    CHECK(config->hard_cap == 1024 * MIB);
    CHECK(config->limit_headroom == 300 * MIB);
    CHECK(config->spill_headroom == 5 * MIB);
}

TEST_CASE("a bad value is refused and names its variable") {
    clear_memory_env();
    for (const char* name :
         {dftracer::utils::common::constants::memory_settings::BUDGET_ENV,
          dftracer::utils::common::constants::memory_settings::HARD_LIMIT_ENV,
          dftracer::utils::common::constants::memory_settings::
              LIMIT_HEADROOM_ENV,
          dftracer::utils::common::constants::memory_settings::
              SPILL_HEADROOM_ENV}) {
        for (const char* bad : {"12 parsecs", "-5", "lots"}) {
            EnvVar var(name, bad);
            auto config = dut::common::memory_config_from_env();
            REQUIRE_FALSE(config);
            CHECK(config.error().is_code(
                dut::common::ErrorCode::InvalidArgument));
            CHECK(config.error().message().find(name) != std::string::npos);
            CHECK(config.error().message().find(bad) != std::string::npos);
        }
    }
}

TEST_CASE("memory statuses map to error codes") {
    using dut::common::ErrorCode;
    using mem::Status;
    static_assert(dut::common::error_code_of(Status::OverBudget) ==
                  dftracer::utils::common::ErrorCode::MemoryBudgetExceeded);
    CHECK(dut::common::error_code_of(Status::OverBudget) ==
          dftracer::utils::common::ErrorCode::MemoryBudgetExceeded);
    CHECK(dut::common::error_code_of(Status::OverCap) ==
          dftracer::utils::common::ErrorCode::MemoryBudgetExceeded);
    CHECK(dut::common::error_code_of(Status::Pending) ==
          dftracer::utils::common::ErrorCode::MemoryBudgetExceeded);
    CHECK(dut::common::error_code_of(Status::PoolTableFull) ==
          dftracer::utils::common::ErrorCode::Internal);
    CHECK(dut::common::error_code_of(Status::SlotTableFull) ==
          dftracer::utils::common::ErrorCode::Internal);
    CHECK(dut::common::error_code_of(Status::Ok) ==
          dftracer::utils::common::ErrorCode::Unknown);
    CHECK(dut::common::error_code_of(Status::Cancelled) ==
          dftracer::utils::common::ErrorCode::Unknown);

    CHECK(std::string(dut::common::error_code_name(
              dftracer::utils::common::ErrorCode::MemoryBudgetExceeded)) ==
          "MEMORY_BUDGET_EXCEEDED");
    CHECK(dut::common::condition_of(
              dftracer::utils::common::ErrorCode::MemoryBudgetExceeded) ==
          dut::dstd::Condition::Internal);
}

TEST_CASE("a failed reservation maps to the budget error") {
    mem::Config c;
    c.budget = 64 * MIB;
    mem::configure(c);
    mem::PoolHandle pool = mem::Manager::get().root().child("config");
    mem::Reservation over = pool->reserve(65 * MIB);
    REQUIRE_FALSE(over);
    CHECK(dut::common::error_code_of(over.status()) ==
          dut::common::ErrorCode::MemoryBudgetExceeded);
    mem::configure(mem::Config{});
}

// The only test case that calls configure_memory_from_env: it applies once
// per process.
TEST_CASE("env values reach memory::configure once per process") {
    clear_memory_env();
    EnvVar budget(
        dftracer::utils::common::constants::memory_settings::BUDGET_ENV,
        "512MB");
    EnvVar cap(
        dftracer::utils::common::constants::memory_settings::HARD_LIMIT_ENV,
        "8GB");
    EnvVar limit(
        dftracer::utils::common::constants::memory_settings::LIMIT_HEADROOM_ENV,
        "10MB");
    EnvVar spill(
        dftracer::utils::common::constants::memory_settings::SPILL_HEADROOM_ENV,
        "5MB");
    REQUIRE(dut::common::configure_memory_from_env());
    mem::Config applied = mem::config();
    CHECK(applied.budget == 512 * MIB);
    CHECK(applied.hard_cap == 8 * 1024 * MIB);
    CHECK(applied.limit_headroom == 10 * MIB);
    CHECK(applied.spill_headroom == 5 * MIB);

    ::setenv(dftracer::utils::common::constants::memory_settings::BUDGET_ENV,
             "nonsense", 1);
    CHECK(dut::common::configure_memory_from_env());
    CHECK(mem::config().budget == 512 * MIB);
    mem::configure(mem::Config{});
}
